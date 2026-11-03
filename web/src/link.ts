// The phone's one WebSocket to the gateway: request and reply by id, events
// fanned out by name, binary frames decoded, and a session that survives the
// socket. iOS drops sockets whenever the app leaves the foreground, so
// reconnecting and resuming is the normal path, not the error path.

import {
  decodeBinary,
  encodeCommand,
  isReply,
  parseServerMessage,
  PING_INTERVAL_MS,
  Tag,
  type EventName,
  type GatewayEvent,
  type JsonValue,
  type LinkEvent,
  type Op,
  type OpusFrame,
  type SpectrumFrame,
} from "./protocol";

export const BACKOFF_MIN_MS = 500;
export const BACKOFF_MAX_MS = 8000;
export const RESUME_KEY = "revenant.resume";
// A request with no reply in this long is given up on: the socket may be one
// iOS kept open across a lock with nobody on the other end.
export const REQUEST_TIMEOUT_MS = 10_000;
// Silence for two ping intervals means the socket is dead whatever it claims.
export const STALE_MS = 2 * PING_INTERVAL_MS;
// The backoff starts over only once a link has stayed up this long, so a
// gateway that accepts and immediately drops does not get hammered.
export const STABLE_MS = 5_000;

// The subset of WebSocket the link uses, so tests can hand it a fake.
export interface SocketLike {
  binaryType: string;
  readyState: number;
  send(data: string): void;
  close(): void;
  onopen: ((ev: unknown) => void) | null;
  onclose: ((ev: unknown) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
}

export interface Timers {
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
}

export interface KeyStore {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

export interface LinkDeps {
  url: string;
  socket: (url: string) => SocketLike;
  timers: Timers;
  store: KeyStore;
  // Milliseconds, monotonic enough for staleness checks.
  now: () => number;
}

// Every gateway event by name, plus the link's own: "ready" once the socket is
// usable (after any resume settled), "down" when it closes.
export interface LinkEvents {
  ready: { resumed: boolean };
  down: Record<string, never>;
  spectrum: SpectrumFrame;
  opus: OpusFrame;
  link: LinkEvent;
  vrxStatus: GatewayEvent;
  decoded: GatewayEvent;
  transcript: GatewayEvent;
  transcriptionStatus: GatewayEvent;
  source: GatewayEvent;
  vrxRemoved: GatewayEvent;
  hls: GatewayEvent;
  hello: GatewayEvent;
  ping: GatewayEvent;
  vrxEventsEnded: GatewayEvent;
}
export type LinkEventName = keyof LinkEvents;

// Compile-time check that every protocol event has an entry above.
type MissingEvents = Exclude<EventName, LinkEventName>;
const _allEvents: MissingEvents extends never ? true : never = true;
void _allEvents;

export class LinkError extends Error {}

export interface RequestOptions {
  timeoutMs?: number;
}

interface Pending {
  resolve: (v: JsonValue) => void;
  reject: (e: Error) => void;
  timer: unknown;
}

export class Link {
  private ws: SocketLike | null = null;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private listeners = new Map<string, Set<(payload: never) => void>>();
  private backoff = BACKOFF_MIN_MS;
  private retry: unknown = null;
  private isReady = false;
  private stopped = true;
  // While a resume is in flight: its id, and the fresh token the gateway's own
  // hello offered on this socket, kept in case the resume is refused.
  private resumeId: number | null = null;
  private freshToken: string | null = null;
  private lastHeard = 0;
  private watchdog: unknown = null;
  private stableTimer: unknown = null;

  constructor(private readonly deps: LinkDeps) {}

  get ready(): boolean {
    return this.isReady;
  }

  start(): void {
    this.stopped = false;
    this.open();
    this.armWatchdog();
  }

  stop(): void {
    this.stopped = true;
    this.cancelRetry();
    if (this.watchdog !== null) this.deps.timers.clearTimeout(this.watchdog);
    this.watchdog = null;
    const ws = this.ws;
    this.ws = null;
    if (ws) {
      this.detach(ws);
      ws.close();
    }
    this.lost();
  }

  // Called when the app returns to the foreground or the network comes back:
  // a reconnect waiting out its backoff goes now, and a socket that has heard
  // nothing for too long (iOS often leaves one half open across a lock) is
  // replaced rather than trusted.
  wake(): void {
    if (this.stopped) return;
    this.backoff = BACKOFF_MIN_MS;
    if (this.ws !== null && this.isStale()) this.drop(this.ws);
    if (this.ws === null) {
      this.cancelRetry();
      this.open();
    }
  }

  on<K extends LinkEventName>(name: K, fn: (payload: LinkEvents[K]) => void): () => void {
    let set = this.listeners.get(name);
    if (!set) {
      set = new Set();
      this.listeners.set(name, set);
    }
    set.add(fn as (payload: never) => void);
    return () => void set.delete(fn as (payload: never) => void);
  }

  // Resolves with the reply's result, which is returned whole: setMode and
  // setBandwidth carry a new vrx and `replaced`, any reply may carry
  // statusUnavailable, and callers need all of it. Rejects with the gateway's
  // error text unchanged, or a LinkError if the socket is not ready or drops
  // before the reply.
  // timeoutMs overrides REQUEST_TIMEOUT_MS for ops the engine may spend longer
  // on, such as opening or enumerating devices.
  request(op: Op, args?: { [key: string]: JsonValue }, opts?: RequestOptions): Promise<JsonValue> {
    if (!this.isReady || this.ws === null) {
      return Promise.reject(new LinkError("not connected"));
    }
    return this.send(op, args, opts?.timeoutMs ?? REQUEST_TIMEOUT_MS);
  }

  private send(op: Op, args?: { [key: string]: JsonValue }, timeoutMs = REQUEST_TIMEOUT_MS): Promise<JsonValue> {
    const ws = this.ws;
    if (ws === null) return Promise.reject(new LinkError("not connected"));
    const id = this.nextId++;
    let text: string;
    try {
      text = encodeCommand(args === undefined ? { id, op } : { id, op, args });
    } catch (e) {
      return Promise.reject(e instanceof Error ? e : new Error(String(e)));
    }
    return new Promise<JsonValue>((resolve, reject) => {
      const timer = this.deps.timers.setTimeout(() => {
        if (this.pending.delete(id)) reject(new LinkError(`${op}: no reply in ${timeoutMs / 1000} s`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      ws.send(text);
    });
  }

  private open(): void {
    this.retry = null;
    let ws: SocketLike;
    try {
      ws = this.deps.socket(this.deps.url);
    } catch {
      this.scheduleRetry();
      return;
    }
    this.ws = ws;
    // A connect that never completes is as stale as a socket gone quiet.
    this.lastHeard = this.deps.now();
    ws.binaryType = "arraybuffer";
    ws.onopen = () => this.opened(ws);
    ws.onmessage = (ev) => this.message(ws, ev.data);
    ws.onerror = () => {};
    ws.onclose = () => this.closed(ws);
  }

  private opened(ws: SocketLike): void {
    if (ws !== this.ws) return;
    this.lastHeard = this.deps.now();
    const token = this.deps.store.getItem(RESUME_KEY);
    if (token === null) {
      this.becomeReady(false);
      return;
    }
    // resume is accepted only as a connection's first command, so it goes out
    // before anything else and requests wait for it to settle.
    this.freshToken = null;
    const p = this.send("resume", { token });
    this.resumeId = this.nextId - 1;
    p.then(
      () => this.resumeSettled(ws, true),
      (e: unknown) => {
        // No answer is not a refusal: the session may still be waiting on the
        // gateway, so keep the token and try again on a new socket.
        if (e instanceof LinkError) {
          if (ws === this.ws) this.drop(ws);
          return;
        }
        this.resumeSettled(ws, false);
      },
    );
  }

  private resumeSettled(ws: SocketLike, ok: boolean): void {
    if (ws !== this.ws) return;
    this.resumeId = null;
    if (!ok) {
      // The old session is gone; carry on with the one this socket was given.
      if (this.freshToken !== null) this.deps.store.setItem(RESUME_KEY, this.freshToken);
      else this.deps.store.removeItem(RESUME_KEY);
    }
    this.freshToken = null;
    this.becomeReady(ok);
  }

  private becomeReady(resumed: boolean): void {
    this.isReady = true;
    this.stableTimer = this.deps.timers.setTimeout(() => {
      this.stableTimer = null;
      this.backoff = BACKOFF_MIN_MS;
    }, STABLE_MS);
    this.emit("ready", { resumed });
  }

  private message(ws: SocketLike, data: unknown): void {
    if (ws !== this.ws) return;
    this.lastHeard = this.deps.now();
    if (data instanceof ArrayBuffer) {
      const f = decodeBinary(data);
      if (f === null) return;
      if (f.tag === Tag.Spectrum) this.emit("spectrum", f);
      else this.emit("opus", f);
      return;
    }
    if (typeof data !== "string") return;
    const m = parseServerMessage(data);
    if (m === null) return;
    if (isReply(m)) {
      const p = this.pending.get(m.id);
      if (!p) return;
      this.pending.delete(m.id);
      this.deps.timers.clearTimeout(p.timer);
      if (m.ok) p.resolve(m.result);
      else p.reject(new Error(m.error));
      return;
    }
    const ev = m as GatewayEvent;
    if (ev.event === "ping") {
      // The gateway closes a socket it has not heard from in 45 s. pong gets
      // no reply, so it is not tracked as pending.
      ws.send(encodeCommand({ id: this.nextId++, op: "pong" }));
    } else if (ev.event === "hello" && typeof ev.resume === "string") {
      if (this.resumeId !== null) this.freshToken = ev.resume;
      else this.deps.store.setItem(RESUME_KEY, ev.resume);
    }
    this.emit(ev.event, ev as never);
  }

  private closed(ws: SocketLike): void {
    if (ws !== this.ws) return;
    this.drop(ws);
  }

  // Abandons the current socket and schedules the next attempt.
  private drop(ws: SocketLike): void {
    this.detach(ws);
    if (ws.readyState < 2) ws.close();
    this.ws = null;
    this.lost();
    if (!this.stopped) this.scheduleRetry();
  }

  private isStale(): boolean {
    return this.deps.now() - this.lastHeard > STALE_MS;
  }

  private armWatchdog(): void {
    this.watchdog = this.deps.timers.setTimeout(() => {
      this.watchdog = null;
      if (this.stopped) return;
      if (this.ws !== null && this.isStale()) this.drop(this.ws);
      this.armWatchdog();
    }, PING_INTERVAL_MS);
  }

  private lost(): void {
    this.resumeId = null;
    this.freshToken = null;
    const wasReady = this.isReady;
    this.isReady = false;
    if (this.stableTimer !== null) {
      this.deps.timers.clearTimeout(this.stableTimer);
      this.stableTimer = null;
    }
    const pending = this.pending;
    this.pending = new Map();
    for (const p of pending.values()) {
      this.deps.timers.clearTimeout(p.timer);
      p.reject(new LinkError("connection lost"));
    }
    if (wasReady) this.emit("down", {});
  }

  private detach(ws: SocketLike): void {
    ws.onopen = ws.onclose = ws.onerror = null;
    ws.onmessage = null;
  }

  private scheduleRetry(): void {
    this.cancelRetry();
    const delay = this.backoff;
    this.backoff = Math.min(this.backoff * 2, BACKOFF_MAX_MS);
    this.retry = this.deps.timers.setTimeout(() => {
      if (!this.stopped && this.ws === null) this.open();
    }, delay);
  }

  private cancelRetry(): void {
    if (this.retry !== null) {
      this.deps.timers.clearTimeout(this.retry);
      this.retry = null;
    }
  }

  private emit(name: string, payload: unknown): void {
    const set = this.listeners.get(name);
    if (!set) return;
    for (const fn of set) {
      try {
        (fn as (p: unknown) => void)(payload);
      } catch (e) {
        // One bad listener must not starve the others or kill the socket.
        console.error(e);
      }
    }
  }
}

// wss on the page's own host and port: the gateway serves the app and the
// socket from one origin.
export function socketUrl(loc: { protocol: string; host: string }): string {
  return (loc.protocol === "https:" ? "wss://" : "ws://") + loc.host + "/ws";
}

// Foreground and network-back are when iOS lets a dropped socket reconnect.
export function wireLifecycle(
  link: Link,
  doc: { visibilityState: string; addEventListener(ev: string, fn: () => void): void },
  win: { addEventListener(ev: string, fn: () => void): void },
): void {
  doc.addEventListener("visibilitychange", () => {
    if (doc.visibilityState === "visible") link.wake();
  });
  win.addEventListener("online", () => link.wake());
}

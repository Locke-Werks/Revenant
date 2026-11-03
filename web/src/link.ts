// The phone's one WebSocket to the gateway: request and reply by id, events
// fanned out by name, binary frames decoded, and a session that survives the
// socket. iOS drops sockets whenever the app leaves the foreground, so
// reconnecting and resuming is the normal path, not the error path.

import {
  decodeBinary,
  encodeCommand,
  isReply,
  parseServerMessage,
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

interface Pending {
  resolve: (v: JsonValue) => void;
  reject: (e: Error) => void;
}

const OPEN = 1;

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

  constructor(private readonly deps: LinkDeps) {}

  get ready(): boolean {
    return this.isReady;
  }

  start(): void {
    this.stopped = false;
    this.open();
  }

  stop(): void {
    this.stopped = true;
    this.cancelRetry();
    const ws = this.ws;
    this.ws = null;
    if (ws) {
      this.detach(ws);
      ws.close();
    }
    this.lost();
  }

  // Called when the app returns to the foreground or the network comes back:
  // a reconnect waiting out its backoff goes now.
  wake(): void {
    if (this.stopped) return;
    this.backoff = BACKOFF_MIN_MS;
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
  request(op: Op, args?: { [key: string]: JsonValue }): Promise<JsonValue> {
    if (!this.isReady || this.ws === null) {
      return Promise.reject(new LinkError("not connected"));
    }
    return this.send(op, args);
  }

  private send(op: Op, args?: { [key: string]: JsonValue }): Promise<JsonValue> {
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
      this.pending.set(id, { resolve, reject });
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
    ws.binaryType = "arraybuffer";
    ws.onopen = () => this.opened(ws);
    ws.onmessage = (ev) => this.message(ws, ev.data);
    ws.onerror = () => {};
    ws.onclose = () => this.closed(ws);
  }

  private opened(ws: SocketLike): void {
    if (ws !== this.ws) return;
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
      () => this.resumeSettled(ws, false),
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
    this.backoff = BACKOFF_MIN_MS;
    this.emit("ready", { resumed });
  }

  private message(ws: SocketLike, data: unknown): void {
    if (ws !== this.ws) return;
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
    this.detach(ws);
    this.ws = null;
    this.lost();
    if (!this.stopped) this.scheduleRetry();
  }

  private lost(): void {
    this.resumeId = null;
    this.freshToken = null;
    const wasReady = this.isReady;
    this.isReady = false;
    const pending = this.pending;
    this.pending = new Map();
    for (const p of pending.values()) p.reject(new LinkError("connection lost"));
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

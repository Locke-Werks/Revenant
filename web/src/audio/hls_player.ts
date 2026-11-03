// The <audio> element that plays keep listening's HLS stream. Safari plays
// HLS natively, and iOS keeps an element playing with the phone locked, which
// is the whole reason this path exists.
//
// Two iOS rules shape it:
// - play() needs a user gesture the first time. The keep-listening tap comes
//   before the stream exists (the gateway starts encoding on that op), so the
//   tap plays a short silent clip instead; an element that has played once
//   from a gesture may be given a new src and played later without one.
// - The gateway answers 404 for the playlist until three segments exist,
//   which Safari treats as a fatal error. So the playlist is polled until it
//   is served before the element is pointed at it.

export interface MediaLike {
  src: string;
  currentTime: number;
  readonly seekable: { readonly length: number; end(i: number): number };
  readonly paused: boolean;
  play(): Promise<void>;
  pause(): void;
  load(): void;
  removeAttribute(name: string): void;
  addEventListener(type: "playing" | "error" | "ended" | "stalled", fn: () => void): void;
}

export interface HlsPlayerDeps {
  media: MediaLike;
  // True once the URL answers 200.
  probe(url: string): Promise<boolean>;
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(h: unknown): void;
  // A silent clip for the unlocking play().
  silence: string;
}

export type HlsStatus =
  | { kind: "idle" }
  // Waiting on the gateway's hls event or its first segments.
  | { kind: "starting" }
  | { kind: "playing" }
  // The gateway said it cannot encode, or the stream failed and retries ran out.
  | { kind: "failed"; error: string };

export const PROBE_EVERY_MS = 500;
// The first segments take about three segment lengths; give it well over that.
export const PROBE_GIVE_UP_MS = 20_000;
export const RETRY_AFTER_MS = 1_000;
export const MAX_RETRIES = 5;
// Playing but the clock not moving this long is a stream that died quietly
// (Safari does not always raise an error for a live playlist that stops).
export const STALL_MS = 6_000;
export const WATCH_EVERY_MS = 1_000;

export class HlsPlayer {
  private url: string | null = null;
  // What the element was last given. Not read back from media.src, which the
  // browser makes absolute.
  private loaded: string | null = null;
  private wanted = false;
  private unlocked = false;
  private timer: unknown = null;
  private probeStarted = 0;
  private elapsed = 0;
  private retries = 0;
  // Bumped on every change of intent or URL, so a stale probe does nothing.
  private gen = 0;
  private statusNow: HlsStatus = { kind: "idle" };
  private listeners: ((s: HlsStatus) => void)[] = [];
  private watch: unknown = null;
  private lastTime = -1;
  private stillFor = 0;
  private failed: () => void = () => {};

  constructor(private readonly deps: HlsPlayerDeps) {
    deps.media.addEventListener("playing", () => {
      if (this.wanted && this.url !== null && this.loaded === this.url) {
        this.retries = 0;
        this.set({ kind: "playing" });
        this.startWatch();
      }
    });
    const failed = () => {
      this.stopWatch();
      if (!this.wanted || this.url === null || this.loaded !== this.url) return;
      if (this.retries >= MAX_RETRIES) {
        this.set({ kind: "failed", error: "the background stream stopped and did not come back" });
        return;
      }
      this.retries++;
      this.set({ kind: "starting" });
      const gen = this.gen;
      this.schedule(() => {
        if (gen === this.gen) this.begin();
      }, RETRY_AFTER_MS);
    };
    deps.media.addEventListener("error", failed);
    // A live stream that ends was cut off at the gateway.
    deps.media.addEventListener("ended", failed);
    this.failed = failed;
    // stalled fires on ordinary buffering too; it only counts once the
    // clock has also stood still for a while.
    deps.media.addEventListener("stalled", () => {
      if (this.statusNow.kind === "playing" && this.stillFor >= STALL_MS / 2) failed();
    });
  }

  get status(): HlsStatus {
    return this.statusNow;
  }

  get playing(): boolean {
    return this.statusNow.kind === "playing";
  }

  onStatus(fn: (s: HlsStatus) => void): void {
    this.listeners.push(fn);
  }

  // Call inside the user's tap, before anything awaits.
  unlock(): void {
    if (this.unlocked) return;
    this.unlocked = true;
    const m = this.deps.media;
    if (this.loaded === null || m.paused) {
      this.loaded = this.deps.silence;
      m.src = this.deps.silence;
      m.play().catch(() => {
        // Refused: the element stays locked and a later play() fails too,
        // which surfaces as an error status.
        this.unlocked = false;
      });
    }
  }

  // The gateway's hls event: a URL, or null with the reason it has none.
  setUrl(url: string | null, error?: string): void {
    // The same URL again after a failure is the gateway re-announcing a
    // stream that is back: start over.
    if (url === this.url && error === undefined && this.statusNow.kind !== "failed") return;
    this.url = url;
    this.gen++;
    this.cancel();
    if (!this.wanted) return;
    if (url === null) {
      this.set({ kind: "failed", error: error ?? "the gateway has no background stream" });
      return;
    }
    this.retries = 0;
    this.begin();
  }

  play(): void {
    this.wanted = true;
    this.gen++;
    this.cancel();
    this.retries = 0;
    if (this.url === null) {
      this.set({ kind: "starting" });
      return;
    }
    const m = this.deps.media;
    if (this.loaded === this.url) {
      // Paused at a cut: jump to the live edge and play the element that is
      // already activated, which iOS allows without a gesture. A fresh load
      // only if that is refused.
      const gen = this.gen;
      this.set({ kind: "starting" });
      if (m.seekable.length > 0) m.currentTime = m.seekable.end(m.seekable.length - 1);
      m.play().catch(() => {
        if (gen === this.gen) this.begin();
      });
      return;
    }
    this.begin();
  }

  // Part of a cut to live: paused with the stream kept, see play().
  pause(): void {
    this.wanted = false;
    this.gen++;
    this.cancel();
    this.stopWatch();
    this.deps.media.pause();
    this.set({ kind: "idle" });
  }

  stop(): void {
    this.wanted = false;
    this.gen++;
    this.cancel();
    this.stopWatch();
    const m = this.deps.media;
    m.pause();
    if (this.loaded !== null && this.loaded !== this.deps.silence) {
      this.loaded = null;
      m.removeAttribute("src");
      m.load();
    }
    this.set({ kind: "idle" });
  }

  private begin(): void {
    const url = this.url;
    if (url === null) return;
    const gen = this.gen;
    this.set({ kind: "starting" });
    this.probeStarted = this.elapsed;
    const attempt = () => {
      this.deps
        .probe(url)
        .catch(() => false)
        .then((ok) => {
          if (gen !== this.gen) return;
          if (ok) {
            const m = this.deps.media;
            this.loaded = url;
            m.src = url;
            m.play().catch((e: unknown) => {
              if (gen !== this.gen) return;
              this.set({ kind: "failed", error: `the phone refused to play: ${String(e)}` });
            });
            return;
          }
          if (this.elapsed - this.probeStarted >= PROBE_GIVE_UP_MS) {
            this.set({ kind: "failed", error: "the gateway never served the background stream" });
            return;
          }
          this.schedule(() => {
            if (gen === this.gen) attempt();
          }, PROBE_EVERY_MS);
        });
    };
    attempt();
  }

  private startWatch(): void {
    this.stopWatch();
    this.lastTime = this.deps.media.currentTime;
    const tick = () => {
      this.watch = this.deps.setTimeout(tick, WATCH_EVERY_MS);
      const t = this.deps.media.currentTime;
      if (t !== this.lastTime) {
        this.lastTime = t;
        this.stillFor = 0;
        return;
      }
      this.stillFor += WATCH_EVERY_MS;
      if (this.stillFor >= STALL_MS) this.failed();
    };
    this.watch = this.deps.setTimeout(tick, WATCH_EVERY_MS);
  }

  private stopWatch(): void {
    if (this.watch !== null) this.deps.clearTimeout(this.watch);
    this.watch = null;
    this.stillFor = 0;
  }

  private schedule(fn: () => void, ms: number): void {
    this.cancel();
    this.timer = this.deps.setTimeout(() => {
      this.timer = null;
      this.elapsed += ms;
      fn();
    }, ms);
  }

  private cancel(): void {
    if (this.timer !== null) this.deps.clearTimeout(this.timer);
    this.timer = null;
  }

  private set(s: HlsStatus): void {
    const same = s.kind === this.statusNow.kind && (s.kind !== "failed" || (this.statusNow.kind === "failed" && s.error === this.statusNow.error));
    this.statusNow = s;
    if (!same) for (const fn of this.listeners) fn(s);
  }
}

// A 100 ms silent 8 kHz mono WAV, for the unlocking play().
export function silentWavUrl(): string {
  const samples = 800;
  const bytes = new Uint8Array(44 + samples);
  const dv = new DataView(bytes.buffer);
  const ascii = (at: number, s: string) => {
    for (let i = 0; i < s.length; i++) bytes[at + i] = s.charCodeAt(i);
  };
  ascii(0, "RIFF");
  dv.setUint32(4, 36 + samples, true);
  ascii(8, "WAVEfmt ");
  dv.setUint32(16, 16, true);
  dv.setUint16(20, 1, true); // PCM
  dv.setUint16(22, 1, true); // mono
  dv.setUint32(24, 8000, true);
  dv.setUint32(28, 8000, true);
  dv.setUint16(32, 1, true);
  dv.setUint16(34, 8, true);
  ascii(36, "data");
  dv.setUint32(40, samples, true);
  // 8-bit PCM is unsigned: 128 is silence.
  bytes.fill(128, 44);
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return `data:audio/wav;base64,${btoa(bin)}`;
}

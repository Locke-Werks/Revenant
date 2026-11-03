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

  constructor(private readonly deps: HlsPlayerDeps) {
    deps.media.addEventListener("playing", () => {
      if (this.wanted && this.url !== null && this.loaded === this.url) {
        this.retries = 0;
        this.set({ kind: "playing" });
      }
    });
    const failed = () => {
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
    if (url === this.url && error === undefined) return;
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
    this.begin();
  }

  // Part of a cut to live. The element is emptied rather than paused, so the
  // next play() starts at the live edge instead of minutes in the past.
  pause(): void {
    this.stop();
  }

  stop(): void {
    this.wanted = false;
    this.gen++;
    this.cancel();
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

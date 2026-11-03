// Live audio for one receiver: asks the gateway for its Opus stream, decodes
// each packet on the main thread (see opus_decoder.ts for why) and hands the
// PCM to the AudioWorklet, which owns the jitter buffer and plays it.
//
// iOS only lets an AudioContext start from a user gesture, and only within
// that gesture's own turn of the event loop. start() therefore creates and
// resumes the context before it awaits anything; callers must call it
// directly from the tap handler. resume() is for later attempts (returning to
// the foreground) and reports whether the context is actually running.

import type { LinkEvent, JsonValue, OpusFrame, GatewayEvent } from "../protocol";
import { createPcmDecoder, loadOpusModule, type PcmDecoder } from "./opus_decoder";
import { PROCESSOR_NAME, WORKLET_URL, type ToWorklet, type WorkletState } from "./messages";
import { SAMPLE_RATE } from "./jitter";

// The parts of Link this uses, so tests can stand in for it.
export interface LiveLink {
  request(op: "audio", args: { [key: string]: JsonValue }): Promise<JsonValue>;
  on(name: "opus", fn: (f: OpusFrame) => void): () => void;
  on(name: "ready", fn: (e: { resumed: boolean }) => void): () => void;
  on(name: "link", fn: (e: LinkEvent) => void): () => void;
  on(name: "vrxRemoved", fn: (e: GatewayEvent) => void): () => void;
}

export interface AudioContextLike {
  // "suspended", "running", "closed", or Safari's "interrupted" while a call,
  // Siri or another app holds the audio session.
  readonly state: string;
  readonly sampleRate: number;
  resume(): Promise<void>;
  onStateChange(fn: () => void): void;
}

export interface AudioSink {
  push(seq: number, pcm: Float32Array): void;
  reset(): void;
  onState(fn: (s: WorkletState) => void): void;
  // Output level, ramped linearly over rampMs from wherever it is now.
  gain(value: number, rampMs: number): void;
}

export interface LiveAudioDeps {
  link: LiveLink;
  // Called once, synchronously, inside the first start()'s gesture.
  context(): AudioContextLike;
  sink(ctx: AudioContextLike): Promise<AudioSink>;
  decoder(): Promise<PcmDecoder>;
}

export interface LiveStats {
  // Opus frames for a receiver other than the one playing.
  ignored: number;
  // Packets the decoder refused; the jitter buffer plays them as silence.
  undecodable: number;
}

export class LiveAudio {
  readonly stats: LiveStats = { ignored: 0, undecodable: 0 };
  // The worklet's last report, for a debug readout.
  worklet: WorkletState | null = null;

  private ctx: AudioContextLike | null = null;
  private sinkP: Promise<AudioSink> | null = null;
  private decoderP: Promise<PcmDecoder> | null = null;
  private sink: AudioSink | null = null;
  private decoder: PcmDecoder | null = null;
  private active: number | null = null;
  // Bumped by every start and stop, so a start that finishes after a newer
  // call does not claim the stream.
  private gen = 0;
  // The receiver whose packets the decoder's state came from.
  private decodedVrx: number | null = null;
  // What the caller last asked for: start's vrx, or null after stop.
  private wanted: number | null = null;
  // Keep listening has HLS playing instead: frames are dropped, but the
  // gateway's audio selection stays, because HLS follows it.
  private held = false;
  // Output level wanted, applied when the sink exists.
  private level = 1;

  constructor(private readonly deps: LiveAudioDeps) {
    deps.link.on("opus", (f) => this.onOpus(f));
    deps.link.on("ready", () => {
      // A gateway that could not resume the session has no audio selection;
      // one that could does not mind hearing it again. Either way the old
      // stream's tail is stale.
      if (this.active === null) return;
      this.flush();
      void deps.link.request("audio", { vrx: this.active, on: true }).catch(() => {});
    });
    deps.link.on("link", (e) => {
      for (const r of e.recreated ?? []) this.retarget(r.old, r.new);
      for (const l of e.lost ?? []) if (l.vrx === this.active) this.clear();
    });
    deps.link.on("vrxRemoved", (e) => {
      if (typeof e.vrx === "number" && e.vrx === this.active) this.clear();
    });
  }

  // The receiver playing, or null.
  get vrx(): number | null {
    return this.active;
  }

  // Audio is coming out: the context runs and the jitter buffer has filled.
  get ready(): boolean {
    return this.ctx?.state === "running" && this.worklet?.playing === true;
  }

  async start(vrx: number): Promise<void> {
    const gen = ++this.gen;
    this.wanted = vrx;
    const ctx = this.context();
    // Not awaited before the rest: see the file comment.
    const resumed = this.unlock();
    if (ctx.sampleRate !== SAMPLE_RATE) {
      throw new Error(`audio runs at ${ctx.sampleRate} Hz; live audio needs ${SAMPLE_RATE}`);
    }
    this.sinkP ??= this.deps.sink(ctx).then((s) => {
      s.onState((st) => (this.worklet = st));
      if (this.level !== 1) s.gain(this.level, 0);
      return (this.sink = s);
    });
    this.decoderP ??= this.deps.decoder().then((d) => (this.decoder = d));
    try {
      await Promise.all([this.sinkP, this.decoderP, resumed]);
      if (gen !== this.gen) return;
      if (this.active !== vrx) {
        this.active = null;
        this.flush();
        // Opus decoding carries state from packet to packet; another
        // receiver's stream must not start from it.
        if (this.decodedVrx !== null && this.decodedVrx !== vrx) {
          this.decodedVrx = null;
          await this.decoder?.reset();
        }
      }
      await this.deps.link.request("audio", { vrx, on: true });
      if (gen !== this.gen) {
        // A stop() came in while the gateway was turning this on: turn it
        // off again. A newer start() owns the selection itself, and an off
        // for this id could clear what it just chose.
        if (this.wanted === null) void this.deps.link.request("audio", { vrx, on: false }).catch(() => {});
        return;
      }
      this.active = vrx;
    } catch (e) {
      // A failed setup is not cached, so the next tap tries again.
      if (!this.sink) this.sinkP = null;
      if (!this.decoder) this.decoderP = null;
      if (gen === this.gen) this.active = null;
      throw e;
    }
  }

  stop(): void {
    ++this.gen;
    this.wanted = null;
    const vrx = this.active;
    this.clear();
    if (vrx !== null) void this.deps.link.request("audio", { vrx, on: false }).catch(() => {});
  }

  // Creates and resumes the context. For a gesture whose start() must wait
  // on something else first (adding the receiver it will play): call this in
  // the handler itself, then start() when the receiver exists.
  unlock(): Promise<void> {
    return this.context().resume().catch(() => {});
  }

  async resume(): Promise<boolean> {
    const ctx = this.ctx;
    if (!ctx) return false;
    try {
      await ctx.resume();
    } catch {
      // Refused outside a gesture: report it, the caller offers a tap.
    }
    return ctx.state === "running";
  }

  // Stops playing live without giving up the receiver: keep listening's HLS
  // is encoded from the same selection, so an audio off would end it.
  hold(): void {
    this.held = true;
    this.flush();
  }

  // Takes frames again after hold(), muted, so the buffer can fill while HLS
  // is still the one heard. fadeIn() makes it audible.
  release(): void {
    this.held = false;
    this.setLevel(0, 0);
  }

  get holding(): boolean {
    return this.held;
  }

  fadeIn(ms: number): void {
    this.setLevel(1, ms);
  }

  // The gateway moved the audio selection to a new id (setMode, setBandwidth,
  // or an engine restart); the stream carries on under it.
  // The new receiver's stream starts again from seq 0 and is a separate
  // Opus stream, so the buffer and the decoder start over too.
  retarget(oldVrx: number, newVrx: number): void {
    if (this.wanted === oldVrx) this.wanted = newVrx;
    if (this.active !== oldVrx) return;
    this.active = newVrx;
    this.flush();
    this.decodedVrx = null;
    void this.decoder?.reset();
  }

  private context(): AudioContextLike {
    if (!this.ctx) {
      const ctx = this.deps.context();
      ctx.onStateChange(() => {
        // Suspended or interrupted: what is buffered is old by the time
        // output comes back, so start that from an empty buffer.
        if (ctx.state !== "running") this.flush();
      });
      this.ctx = ctx;
    }
    return this.ctx;
  }

  private setLevel(value: number, rampMs: number): void {
    this.level = value;
    this.sink?.gain(value, rampMs);
  }

  private clear(): void {
    this.active = null;
    this.flush();
  }

  private flush(): void {
    this.sink?.reset();
    if (this.worklet) this.worklet = { ...this.worklet, playing: false };
  }

  private onOpus(f: OpusFrame): void {
    if (this.held) return;
    if (this.active === null || f.vrx !== BigInt(this.active)) {
      this.stats.ignored++;
      return;
    }
    // Packets are not queued for a context that is not playing: they would
    // all be stale, and nothing would drain them.
    if (!this.sink || !this.decoder || this.ctx?.state !== "running") return;
    this.decodedVrx = this.active;
    const pcm = this.decoder.decode(f.packet);
    if (!pcm) {
      this.stats.undecodable++;
      return;
    }
    this.sink.push(f.seq, pcm);
  }
}

// ---- Browser wiring ---------------------------------------------------------

export function browserLiveAudioDeps(link: LiveLink): LiveAudioDeps {
  let real: AudioContext | null = null;
  return {
    link,
    context() {
      // 48 kHz to match the decoder; the browser resamples to the hardware.
      // "interactive" asks for the smallest output buffer.
      const opts: AudioContextOptions = { sampleRate: SAMPLE_RATE, latencyHint: "interactive" };
      const c = new AudioContext(opts);
      real = c;
      return {
        get state() {
          return c.state as string;
        },
        get sampleRate() {
          return c.sampleRate;
        },
        resume: () => c.resume(),
        onStateChange: (fn) => c.addEventListener("statechange", fn),
      };
    },
    async sink() {
      const c = real!;
      await c.audioWorklet.addModule(WORKLET_URL);
      const node = new AudioWorkletNode(c, PROCESSOR_NAME, { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1] });
      // A gain stage so the HLS to live cut can fade in rather than step.
      const level = new GainNode(c, { gain: 1 });
      node.connect(level).connect(c.destination);
      const post = (m: ToWorklet, transfer: Transferable[] = []) => node.port.postMessage(m, transfer);
      return {
        // The frame is a fresh copy from the decoder, so its buffer can move.
        push: (seq, pcm) => post({ t: "push", seq, pcm }, [pcm.buffer]),
        reset: () => post({ t: "reset" }),
        onState: (fn) => {
          node.port.onmessage = (e: MessageEvent<WorkletState>) => fn(e.data);
        },
        gain: (value, rampMs) => {
          const p = level.gain;
          const t = c.currentTime;
          p.cancelScheduledValues(t);
          p.setValueAtTime(p.value, t);
          if (rampMs <= 0) p.setValueAtTime(value, t);
          else p.linearRampToValueAtTime(value, t + rampMs / 1000);
        },
      };
    },
    decoder: () => loadOpusModule().then(createPcmDecoder),
  };
}

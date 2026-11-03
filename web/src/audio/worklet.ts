// The AudioWorklet half of live audio: owns the jitter buffer and plays it.
// Bundled on its own into worklet.js; it runs in AudioWorkletGlobalScope.
//
// Decoded 20 ms frames arrive on the port from the main thread. Keeping the
// buffer here, on the audio clock, means a stall on the main thread (a busy
// waterfall frame, a GC) delays only the pushes, which the buffer absorbs,
// and never the output.

import { JitterBuffer } from "./jitter";
import { PROCESSOR_NAME, type ToWorklet, type WorkletState } from "./messages";

// Not in TypeScript's DOM lib, which describes the main thread's side only.
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
}
declare function registerProcessor(name: string, ctor: new () => AudioWorkletProcessor): void;

// About once a second of output, in 128-sample render quanta.
const REPORT_QUANTA = 375;

class LiveProcessor extends AudioWorkletProcessor {
  private jb = new JitterBuffer();
  private wasPlaying = false;
  private quanta = 0;

  constructor() {
    super();
    this.port.onmessage = (e: MessageEvent<ToWorklet>) => {
      const m = e.data;
      if (m.t === "push") this.jb.push(m.seq, m.pcm);
      else this.jb.reset();
    };
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0]?.[0];
    if (out) this.jb.pullInto(out);
    const playing = this.jb.playing;
    if (playing !== this.wasPlaying || ++this.quanta >= REPORT_QUANTA) {
      this.wasPlaying = playing;
      this.quanta = 0;
      this.report();
    }
    // Stays alive with nothing buffered: silence, not a torn-down node.
    return true;
  }

  private report(): void {
    const s: WorkletState = { t: "state", playing: this.jb.playing, targetMs: this.jb.targetMs, bufferedMs: this.jb.bufferedMs, ...this.jb.stats };
    this.port.postMessage(s);
  }
}

registerProcessor(PROCESSOR_NAME, LiveProcessor);

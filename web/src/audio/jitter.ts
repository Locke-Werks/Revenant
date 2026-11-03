// The live audio jitter buffer. Pure: it knows nothing of the network or Web
// Audio, so it runs inside the AudioWorklet and under node alike.
//
// Packets are 20 ms of 48 kHz mono PCM, keyed by the gateway's u32 seq. Time
// is counted in samples pulled, which is the audio clock: "ten seconds without
// an underrun" means ten seconds actually played, not wall time.

export const SAMPLE_RATE = 48_000;
export const FRAME = 960;
export const FRAME_MS = 20;
export const START_TARGET_MS = 80;
export const MAX_TARGET_MS = 300;
export const GROW_MS = 20;
export const SHRINK_MS = 10;
export const QUIET_SAMPLES = 10 * SAMPLE_RATE;
// A seq more than this far ahead is a new stream (a receiver restarted, or
// the link was down for a while), not a gap worth filling with silence.
export const RESET_JUMP_FRAMES = 1000 / FRAME_MS;
// Buffered audio beyond target + this is dropped. Without it a burst after a
// stall, or a phone clock slightly slower than the gateway's, would add delay
// that never goes away.
export const TRIM_SLACK_MS = 60;

export interface JitterStats {
  underruns: number;
  // Packets that arrived after their play position had passed.
  late: number;
  // Missing seqs played as silence.
  concealed: number;
  resets: number;
  // Frames dropped to bring a backlog back to the target.
  trimmed: number;
}

const SILENCE = new Float32Array(FRAME);

// Signed distance a - b in seqs, correct across the 2^32 wrap.
const seqDiff = (a: number, b: number) => (a - b) | 0;
const seqNext = (s: number) => (s + 1) >>> 0;

export class JitterBuffer {
  targetMs = START_TARGET_MS;
  readonly stats: JitterStats = { underruns: 0, late: 0, concealed: 0, resets: 0, trimmed: 0 };

  private frames = new Map<number, Float32Array>();
  // Next seq to play, and the highest seq held; null when empty since reset.
  private next: number | null = null;
  private high = 0;
  private cur: Float32Array | null = null;
  private curPos = 0;
  private isPlaying = false;
  // True once a frame has been taken since the last reset, after which an
  // earlier seq is late rather than the true start of the stream.
  private consumed = false;
  private quiet = 0;

  get playing(): boolean {
    return this.isPlaying;
  }

  get bufferedMs(): number {
    const curLeft = this.cur ? this.cur.length - this.curPos : 0;
    const ahead = this.next === null || this.frames.size === 0 ? 0 : seqDiff(this.high, this.next) + 1;
    return ((curLeft + Math.max(0, ahead) * FRAME) * 1000) / SAMPLE_RATE;
  }

  reset(): void {
    this.frames.clear();
    this.next = null;
    this.cur = null;
    this.curPos = 0;
    this.isPlaying = false;
    this.consumed = false;
  }

  push(seq: number, pcm: Float32Array): void {
    seq >>>= 0;
    if (this.next === null) {
      this.next = seq;
      this.high = seq;
    }
    const d = seqDiff(seq, this.next);
    if (d < 0) {
      if (this.consumed || -d > RESET_JUMP_FRAMES) {
        this.stats.late++;
        return;
      }
      // Reordered before anything played: this is the real start.
      this.next = seq;
    } else if (d > RESET_JUMP_FRAMES) {
      this.stats.resets++;
      this.reset();
      this.next = seq;
      this.high = seq;
    }
    this.frames.set(seq, pcm);
    if (seqDiff(seq, this.high) > 0) this.high = seq;
  }

  pull(n: number): Float32Array {
    const out = new Float32Array(n);
    this.pullInto(out);
    return out;
  }

  // Fills out completely: audio when there is some, zeros otherwise. The
  // worklet calls this with its own output so the render thread allocates
  // nothing.
  pullInto(out: Float32Array): void {
    out.fill(0);
    if (!this.isPlaying) {
      if (this.frames.size === 0 || this.bufferedMs < this.targetMs) return;
      this.isPlaying = true;
    }
    this.trim();
    let at = 0;
    while (at < out.length) {
      if (!this.cur || this.curPos >= this.cur.length) {
        if (!this.take()) {
          this.underrun();
          return;
        }
      }
      const cur = this.cur!;
      const k = Math.min(out.length - at, cur.length - this.curPos);
      out.set(cur.subarray(this.curPos, this.curPos + k), at);
      this.curPos += k;
      at += k;
    }
    this.quiet += out.length;
    if (this.quiet >= QUIET_SAMPLES) {
      this.quiet = 0;
      this.targetMs = Math.max(START_TARGET_MS, this.targetMs - SHRINK_MS);
    }
  }

  // Moves the next frame into cur: the packet, or silence for a missing seq
  // that later packets have passed. False when nothing is left to play.
  private take(): boolean {
    if (this.next === null || this.frames.size === 0) return false;
    const pcm = this.frames.get(this.next);
    if (pcm) {
      this.frames.delete(this.next);
    } else {
      this.stats.concealed++;
    }
    this.cur = pcm ?? SILENCE;
    this.curPos = 0;
    this.next = seqNext(this.next);
    this.consumed = true;
    return true;
  }

  private underrun(): void {
    this.stats.underruns++;
    this.targetMs = Math.min(MAX_TARGET_MS, this.targetMs + GROW_MS);
    this.isPlaying = false;
    this.quiet = 0;
  }

  private trim(): void {
    while (this.bufferedMs > this.targetMs + TRIM_SLACK_MS && this.next !== null && this.frames.size > 0) {
      // Drop the partly played frame first, then whole frames from the front.
      if (this.cur && this.curPos < this.cur.length) {
        this.cur = null;
        this.curPos = 0;
      } else {
        this.frames.delete(this.next);
        this.next = seqNext(this.next);
        this.consumed = true;
      }
      this.stats.trimmed++;
    }
  }
}

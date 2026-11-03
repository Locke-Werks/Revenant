// Motion between spectrum rows, so the phone draws at its display's refresh
// (60 Hz, 120 on ProMotion) while rows arrive at the engine's rate, about
// 37 a second at 2.4 MS/s.
//
// The waterfall scrolls continuously: RowPacer turns row arrival times into a
// fraction of a row the history has moved since the newest row landed. The
// trace eases: TraceEaser takes each new row as a target and approaches it
// over a few tens of milliseconds. Both report when they have stopped, so a
// screen with nothing arriving draws nothing.

// The lag a burst of rows may leave the scroll behind by, in rows. Past it
// the scroll jumps: a burst after a stall is not worth animating second by
// second.
export const MAX_LAG_ROWS = 1;

// How much faster than the row rate a lagging scroll runs until it is back.
const CATCH_UP = 1.5;

// Gaps outside this are not the row rate: two rows handed over in one task,
// or a stall, a pause or a backgrounded page.
const MIN_GAP_MS = 4;
const MAX_GAP_MS = 250;

// Light enough that a rate change (a new source) settles in a second or so,
// heavy enough that one Wi-Fi hiccup does not change the speed.
const INTERVAL_WEIGHT = 0.1;

export class RowPacer {
  // The time between rows, in ms, as measured.
  interval = 1000 / 30;
  private measured = false;
  private lastAt: number | null = null;
  // Where the scroll was when the newest row landed. 0 is that row's start,
  // 1 is fully scrolled in; below 0 is the scroll still behind the rows.
  private from = 1;

  onRow(now: number): void {
    if (this.lastAt === null) {
      this.lastAt = now;
      this.from = 1;
      return;
    }
    // The new row pushes the history down by one more row than has scrolled,
    // so the picture carries on from where it is instead of jumping. Read at
    // the speed it was drawn at, before this gap changes the speed.
    const at = this.phase(now);
    this.from = Math.max(-MAX_LAG_ROWS, at - 1);
    const gap = now - this.lastAt;
    if (gap >= MIN_GAP_MS && gap <= MAX_GAP_MS) {
      this.interval = this.measured ? this.interval + (gap - this.interval) * INTERVAL_WEIGHT : gap;
      this.measured = true;
    }
    this.lastAt = now;
  }

  // The fraction of the newest row scrolled into view, at most 1.
  phase(now: number): number {
    if (this.lastAt === null) return 1;
    const dt = Math.max(0, now - this.lastAt);
    const rate = 1 / this.interval;
    let p: number;
    if (this.from < 0) {
      const fast = rate * CATCH_UP;
      const toZero = -this.from / fast;
      p = dt <= toZero ? this.from + fast * dt : (dt - toZero) * rate;
    } else {
      p = this.from + rate * dt;
    }
    return Math.min(1, p);
  }

  moving(now: number): boolean {
    return this.phase(now) < 1;
  }

  clear(): void {
    this.lastAt = null;
    this.from = 1;
  }
}

// The trace's time constant. Within 5% of a new row after three of them,
// 75 ms: about two rows at 37 a second, so a real change shows within a
// couple of rows and the frame-to-frame flicker between them is smoothed.
export const TRACE_EASE_MS = 25;

// Under half a quantisation level from the row is the row.
const SETTLED = 0.5;

export class TraceEaser {
  shown = new Float32Array(0);
  private target: ArrayLike<number> | null = null;
  private key: unknown = undefined;
  private last = 0;
  private active = false;

  // A row of another width or cut for another view is a different set of
  // frequencies under the same indices, so it is drawn as it comes.
  setTarget(px: ArrayLike<number>, now: number, key?: unknown): void {
    if (this.target === null || px.length !== this.shown.length || key !== this.key) {
      this.shown = Float32Array.from(px);
      this.target = px;
      this.key = key;
      this.active = false;
      return;
    }
    this.target = px;
    if (!this.active) this.last = now;
    this.active = true;
  }

  // Moves the trace to now. True while it is still moving.
  step(now: number): boolean {
    const target = this.target;
    if (!this.active || !target) return false;
    const dt = Math.max(0, now - this.last);
    this.last = Math.max(this.last, now);
    const k = 1 - Math.exp(-dt / TRACE_EASE_MS);
    const s = this.shown;
    let worst = 0;
    for (let i = 0; i < s.length; i++) {
      const v = s[i]! + (target[i]! - s[i]!) * k;
      s[i] = v;
      const d = Math.abs(target[i]! - v);
      if (d > worst) worst = d;
    }
    if (worst < SETTLED) {
      for (let i = 0; i < s.length; i++) s[i] = target[i]!;
      this.active = false;
    }
    return this.active;
  }

  moving(): boolean {
    return this.active;
  }

  clear(): void {
    this.shown = new Float32Array(0);
    this.target = null;
    this.key = undefined;
    this.active = false;
  }
}

// Physics of the dial strip. Pure and time-injected: every call carries the
// time it happened at, so tests drive a flick without timers or a clock.

// How far the ribbon travels for one fine step. Also the tick spacing.
export const PX_PER_STEP = 8;
export const FAST_MULTIPLIER = 100;
// Above this finger speed a drag moves in the coarse step.
export const FAST_PX_PER_MS = 0.8;
// A flick is over by here, whatever speed it started at.
export const SETTLE_MS = 600;
// e^(-600/110) is under half a percent, so the cut at SETTLE_MS is not seen.
const TAU_MS = 110;
// Slower than this at lift-off is a drag that stopped, not a flick.
const MIN_FLICK_PX_PER_MS = 0.15;
// A finger that rested this long before lifting is not flicking.
const STALE_MS = 80;
// Pointer events arrive unevenly; one interval is a poor speed.
const SMOOTHING = 0.5;

const FINE_STEPS: Record<string, number> = {
  cw: 10,
  usb: 10,
  lsb: 10,
  dsb: 10,
  am: 100,
  nfm: 100,
  raw: 100,
  wfm: 1000,
};

export function fineStep(mode: string): number {
  return FINE_STEPS[mode.toLowerCase()] ?? 100;
}

export function stepFor(mode: string, pxPerMs: number): number {
  const fine = fineStep(mode);
  return Math.abs(pxPerMs) >= FAST_PX_PER_MS ? fine * FAST_MULTIPLIER : fine;
}

export function roundToStep(hz: number, step: number): number {
  return Math.round(hz / step) * step;
}

interface Fling {
  v0: number;
  t0: number;
  // Milliseconds of the decay already handed out.
  done: number;
  hzPerPx: number;
}

export class DialPhysics {
  private acc = 0;
  private dragging = false;
  private lastT = Number.NaN;
  private v = 0;
  private fling: Fling | null = null;

  constructor(private mode: string) {}

  setMode(mode: string): void {
    this.mode = mode;
  }

  get moving(): boolean {
    return this.dragging || this.fling !== null || this.acc !== 0;
  }

  // The finger moved dxPx since the last call. Dragging right scrolls the
  // ribbon right, which brings lower frequencies under the centre line.
  drag(dxPx: number, tMs: number): void {
    this.fling = null;
    this.dragging = true;
    if (!Number.isNaN(this.lastT)) {
      const dt = Math.max(1, tMs - this.lastT);
      this.v = this.v * (1 - SMOOTHING) + (dxPx / dt) * SMOOTHING;
    }
    this.lastT = tMs;
    this.acc -= (dxPx / PX_PER_STEP) * stepFor(this.mode, this.v);
  }

  release(tMs: number): void {
    if (!this.dragging) return;
    this.dragging = false;
    const rested = tMs - this.lastT > STALE_MS;
    if (!rested && Math.abs(this.v) >= MIN_FLICK_PX_PER_MS) {
      this.fling = {
        v0: this.v,
        t0: tMs,
        done: 0,
        hzPerPx: stepFor(this.mode, this.v) / PX_PER_STEP,
      };
    }
    this.v = 0;
    this.lastT = Number.NaN;
  }

  // Frequency change since the last frame, in Hz.
  frame(tMs: number): number {
    const f = this.fling;
    if (f) {
      const b = Math.min(tMs - f.t0, SETTLE_MS);
      if (b > f.done) {
        const px = f.v0 * TAU_MS * (Math.exp(-f.done / TAU_MS) - Math.exp(-b / TAU_MS));
        this.acc -= px * f.hzPerPx;
        f.done = b;
      }
      if (b >= SETTLE_MS) this.fling = null;
    }
    const hz = this.acc;
    this.acc = 0;
    return hz;
  }

  // Another source took over the tuning, or a finger landed on a coasting
  // ribbon: nothing already computed may still land.
  stop(): void {
    this.fling = null;
    this.dragging = false;
    this.acc = 0;
    this.v = 0;
    this.lastT = Number.NaN;
  }
}

// Long press on a receiver chip, which offers to remove it. A plain tap still
// selects; the click that follows a long press is swallowed so the same touch
// does not do both, and a finger that moves is scrolling the chips.

import { LONG_PRESS_MS } from "../gestures";

export { LONG_PRESS_MS };

// Matches the waterfall's slop: a resting thumb jitters a few pixels.
const SLOP_PX = 8;

export interface ChipPressDeps {
  longPress(): void;
  schedule(fn: () => void, ms: number): () => void;
}

export class ChipPress {
  private cancel: (() => void) | null = null;
  private x = 0;
  private y = 0;
  private fired = false;

  constructor(private readonly deps: ChipPressDeps) {}

  down(x: number, y: number): void {
    this.stop();
    this.x = x;
    this.y = y;
    this.fired = false;
    this.cancel = this.deps.schedule(() => {
      this.cancel = null;
      this.fired = true;
      this.deps.longPress();
    }, LONG_PRESS_MS);
  }

  move(x: number, y: number): void {
    if (this.cancel && Math.hypot(x - this.x, y - this.y) > SLOP_PX) this.stop();
  }

  up(): void {
    this.stop();
  }

  // Whether the click that ends this touch should act.
  click(): boolean {
    const ok = !this.fired;
    this.fired = false;
    return ok;
  }

  private stop(): void {
    this.cancel?.();
    this.cancel = null;
  }
}

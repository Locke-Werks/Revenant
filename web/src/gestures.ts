// Turns pointer events on the waterfall into pan, zoom, tap and long press.
// The classifier is pure (pointer positions and an injected timer in, intents
// out) so it can be tested without a browser; attachGestures binds it to an
// element.

import type { Timers } from "./link";

// Movement under this many CSS pixels is a finger resting, not a drag.
export const TAP_SLOP_PX = 10;
export const LONG_PRESS_MS = 500;

export interface GestureSink {
  // One finger moves the active receiver by this much; dragEnd is the release.
  dragTune(dHz: number): void;
  dragEnd(): void;
  // Two fingers move the view.
  pan(dHz: number): void;
  zoom(factor: number, anchorHz: number): void;
  tap(hz: number): void;
  longPress(hz: number): void;
}

// Maps an x in CSS pixels across the element to a frequency, using the view on
// screen at the moment of the call.
export type HzAt = (xCss: number) => number;

interface Pt {
  x: number;
  y: number;
}

export class GestureClassifier {
  private pts = new Map<number, Pt>();
  private start: Pt | null = null;
  // Once a gesture moves, pinches, or long-presses it can no longer be a tap,
  // until every finger has lifted.
  private spent = false;
  // A second finger has touched since the first went down: the survivor of a
  // pinch must not turn into a receiver drag.
  private multi = false;
  private dragging = false;
  private timer: unknown = null;

  constructor(
    private readonly sink: GestureSink,
    private readonly hzAt: HzAt,
    private readonly timers: Timers,
  ) {}

  down(id: number, x: number, y: number): void {
    this.pts.set(id, { x, y });
    if (this.pts.size === 1) {
      this.start = { x, y };
      this.spent = false;
      this.multi = false;
      this.timer = this.timers.setTimeout(() => {
        this.timer = null;
        if (this.pts.size === 1 && !this.spent && this.start) {
          this.spent = true;
          this.sink.longPress(this.hzAt(this.start.x));
        }
      }, LONG_PRESS_MS);
    } else {
      this.spent = true;
      this.multi = true;
      this.endDrag();
      this.cancelTimer();
    }
  }

  move(id: number, x: number, y: number): void {
    const p = this.pts.get(id);
    if (!p) return;

    if (this.pts.size === 1) {
      if (this.multi) {
        p.x = x;
        p.y = y;
        return;
      }
      if (!this.spent && this.start && Math.hypot(x - this.start.x, y - this.start.y) < TAP_SLOP_PX) return;
      if (!this.spent) {
        this.spent = true;
        this.cancelTimer();
      }
      // The receiver goes where the finger goes: right is up in frequency.
      const dHz = this.hzAt(x) - this.hzAt(p.x);
      p.x = x;
      p.y = y;
      this.dragging = true;
      if (dHz !== 0) this.sink.dragTune(dHz);
      return;
    }

    // Two (or more) fingers: the first two pinch. The frequency under their
    // old midpoint moves to the new midpoint, and the span scales by the
    // change in finger spread.
    const [a, b] = this.firstTwo();
    if (!a || !b) return;
    if (a.id !== id && b.id !== id) {
      p.x = x;
      p.y = y;
      return;
    }
    const other = a.id === id ? b : a;
    const oldMid = (a.p.x + b.p.x) / 2;
    const oldDist = Math.abs(a.p.x - b.p.x);
    p.x = x;
    p.y = y;
    const newMid = (p.x + other.p.x) / 2;
    const newDist = Math.abs(p.x - other.p.x);
    const anchorHz = this.hzAt(oldMid);
    // Fingers nearly on top of each other give a useless ratio.
    if (oldDist >= TAP_SLOP_PX && newDist >= TAP_SLOP_PX && newDist !== oldDist) {
      this.sink.zoom(newDist / oldDist, anchorHz);
    }
    // hzAt now reflects the zoom if the sink applied it synchronously.
    const dHz = anchorHz - this.hzAt(newMid);
    if (dHz !== 0) this.sink.pan(dHz);
  }

  up(id: number): void {
    if (!this.pts.has(id)) return;
    this.pts.delete(id);
    if (this.pts.size > 0) return;
    this.cancelTimer();
    this.endDrag();
    if (!this.spent && this.start) this.sink.tap(this.hzAt(this.start.x));
    this.spent = false;
    this.start = null;
  }

  // The browser took the pointer (a system gesture, a call coming in): nothing
  // the fingers did counts.
  cancel(id: number): void {
    this.spent = true;
    this.up(id);
  }

  private firstTwo(): [{ id: number; p: Pt } | undefined, { id: number; p: Pt } | undefined] {
    const it = this.pts.entries();
    const a = it.next().value;
    const b = it.next().value;
    return [a ? { id: a[0], p: a[1] } : undefined, b ? { id: b[0], p: b[1] } : undefined];
  }

  private endDrag(): void {
    if (!this.dragging) return;
    this.dragging = false;
    this.sink.dragEnd();
  }

  private cancelTimer(): void {
    if (this.timer !== null) {
      this.timers.clearTimeout(this.timer);
      this.timer = null;
    }
  }
}

export function attachGestures(el: HTMLElement, sink: GestureSink, hzAt: HzAt): GestureClassifier {
  const g = new GestureClassifier(sink, hzAt, {
    setTimeout: (fn, ms) => window.setTimeout(fn, ms),
    clearTimeout: (h) => window.clearTimeout(h as number),
  });
  // touch-action: none in CSS stops Safari scrolling and page zooming; iOS
  // still sends gesture* events for a pinch, which would zoom the page.
  el.style.touchAction = "none";
  const local = (e: PointerEvent) => {
    const r = el.getBoundingClientRect();
    return [e.clientX - r.left, e.clientY - r.top] as const;
  };
  el.addEventListener("pointerdown", (e) => {
    el.setPointerCapture(e.pointerId);
    const [x, y] = local(e);
    g.down(e.pointerId, x, y);
    e.preventDefault();
  });
  el.addEventListener("pointermove", (e) => {
    const [x, y] = local(e);
    g.move(e.pointerId, x, y);
  });
  el.addEventListener("pointerup", (e) => g.up(e.pointerId));
  el.addEventListener("pointercancel", (e) => g.cancel(e.pointerId));
  for (const ev of ["gesturestart", "gesturechange", "gestureend"]) {
    el.addEventListener(ev, (e) => e.preventDefault());
  }
  return g;
}

// The dial strip: a tick ribbon under a fixed centre line. Dragging it retunes
// the active receiver and a flick coasts. The physics live in ../dial; this
// is the pointer wiring, the drawing and the hand-off to the tune limiter.

import { DialPhysics, PX_PER_STEP, fineStep, roundToStep } from "../dial";
import { TuneLimiter } from "../tune_limiter";

export interface DialDeps {
  // The receiver the dial would move, or null when there is none.
  active(): { hz: number; mode: string } | null;
  // Shows hz on the receiver at once; false when it cannot be dragged.
  preview(hz: number): boolean;
  // One rate-limited tune to the engine.
  tune(hz: number): void;
  // The dial has settled and its final tune has been sent.
  end(): void;
  now(): number;
  schedule(fn: () => void, ms: number): () => void;
  frame(fn: (tMs: number) => void): () => void;
}

// Finger travel before a touch counts as a drag. A thumb resting on the strip
// jitters by a pixel or two and must not retune.
const SLOP_PX = 4;
const HEIGHT_PX = 56;
const MAJOR_EVERY = 10;
const ACCENT = "#e0524f";

export class DialStrip {
  readonly el: HTMLElement;
  private readonly canvas: HTMLCanvasElement;
  private readonly physics = new DialPhysics("nfm");
  private readonly limiter: TuneLimiter;
  // Where the dial has taken the receiver, unrounded; null outside a session.
  private dialHz: number | null = null;
  private pointer: number | null = null;
  private lastX = 0;
  private travelled = 0;
  private cancelFrame: (() => void) | null = null;

  constructor(private readonly deps: DialDeps) {
    this.limiter = new TuneLimiter((hz) => deps.tune(hz), deps.now, deps.schedule);
    this.el = document.createElement("div");
    this.el.className = "dial-strip";
    this.canvas = document.createElement("canvas");
    this.canvas.setAttribute("role", "slider");
    this.canvas.setAttribute("aria-label", "Fine tuning dial, drag to retune");
    this.el.append(this.canvas);

    this.canvas.addEventListener("pointerdown", (e) => this.down(e));
    this.canvas.addEventListener("pointermove", (e) => this.move(e));
    this.canvas.addEventListener("pointerup", (e) => this.up(e));
    this.canvas.addEventListener("pointercancel", (e) => this.up(e));
    new ResizeObserver(() => this.draw()).observe(this.el);
  }

  // Any other tune source calls this before it acts: the dial lets go of the
  // receiver without a final tune of its own.
  stop(): void {
    this.physics.stop();
    this.pointer = null;
    this.stopFrames();
    this.limiter.cancel();
    if (this.dialHz !== null) {
      this.dialHz = null;
      this.deps.end();
    }
  }

  private down(e: PointerEvent): void {
    if (this.pointer !== null) return;
    e.preventDefault();
    this.pointer = e.pointerId;
    this.canvas.setPointerCapture(e.pointerId);
    this.lastX = e.clientX;
    this.travelled = 0;
    // A finger on a coasting ribbon catches it where it is.
    this.physics.stop();
    this.stopFrames();
    // Before the first move, since the step of that move depends on it.
    const r = this.deps.active();
    if (r) this.physics.setMode(r.mode);
  }

  private move(e: PointerEvent): void {
    if (e.pointerId !== this.pointer) return;
    const dx = e.clientX - this.lastX;
    this.lastX = e.clientX;
    const before = this.travelled;
    this.travelled += Math.abs(dx);
    if (this.travelled < SLOP_PX) return;
    // The slop is paid out of the first move so the ribbon does not jump.
    this.physics.drag(before < SLOP_PX ? Math.sign(dx) * (this.travelled - SLOP_PX) : dx, e.timeStamp);
    this.pump(e.timeStamp);
  }

  private up(e: PointerEvent): void {
    if (e.pointerId !== this.pointer) return;
    this.pointer = null;
    this.physics.release(e.timeStamp);
    this.pump(e.timeStamp);
    if (this.physics.moving) this.startFrames();
    else this.settle();
  }

  private startFrames(): void {
    if (this.cancelFrame) return;
    const tick = (t: number) => {
      this.cancelFrame = null;
      this.pump(t);
      if (this.physics.moving) this.cancelFrame = this.deps.frame(tick);
      else if (this.pointer === null) this.settle();
    };
    this.cancelFrame = this.deps.frame(tick);
  }

  private stopFrames(): void {
    this.cancelFrame?.();
    this.cancelFrame = null;
  }

  private pump(tMs: number): void {
    const d = this.physics.frame(tMs);
    if (d === 0) return;
    if (this.dialHz === null) {
      const r = this.deps.active();
      if (!r) {
        this.physics.stop();
        return;
      }
      this.dialHz = r.hz;
    }
    this.dialHz = Math.max(1, this.dialHz + d);
    if (!this.deps.preview(this.dialHz)) {
      // Nothing to move, so the finger's travel must not pile up unseen.
      this.dialHz = null;
      this.physics.stop();
      return;
    }
    this.limiter.set(this.dialHz);
    // The receiver only hears whole Hz, so a slow coast would not redraw
    // through the list's change event.
    this.draw();
  }

  // The dial came to rest: land on a whole fine step, send it, and let the
  // receiver controller apply the engine's answer.
  private settle(): void {
    if (this.dialHz === null) return;
    const r = this.deps.active();
    const hz = roundToStep(this.dialHz, fineStep(r?.mode ?? "nfm"));
    this.dialHz = null;
    if (r && hz !== r.hz && this.deps.preview(hz)) this.limiter.set(hz);
    this.limiter.flush();
    this.deps.end();
    this.draw();
  }

  // Redraws from the receiver's own frequency, which the preview keeps in step
  // with the dial, so a retune from elsewhere moves the ribbon too.
  draw(): void {
    const w = this.el.clientWidth;
    if (w === 0) return;
    const dpr = window.devicePixelRatio || 1;
    if (this.canvas.width !== Math.round(w * dpr) || this.canvas.height !== Math.round(HEIGHT_PX * dpr)) {
      this.canvas.width = Math.round(w * dpr);
      this.canvas.height = Math.round(HEIGHT_PX * dpr);
      this.canvas.style.width = `${w}px`;
      this.canvas.style.height = `${HEIGHT_PX}px`;
    }
    const g = this.canvas.getContext("2d");
    if (!g) return;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, HEIGHT_PX);
    const r = this.deps.active();
    if (!r) return;
    const step = fineStep(r.mode);
    // The unrounded dial value while a session runs, so the ribbon is smooth
    // between the whole Hz the receiver is told.
    const centerHz = this.dialHz ?? r.hz;
    const centerX = w / 2;
    const first = Math.floor((centerHz - (centerX / PX_PER_STEP) * step) / step);
    const last = Math.ceil((centerHz + (centerX / PX_PER_STEP) * step) / step);
    // Labels sit on major ticks; the decimals are what tells one from the next.
    const majorHz = step * MAJOR_EVERY;
    const decimals = Math.max(0, 6 - Math.round(Math.log10(majorHz)));
    g.font = "11px system-ui, sans-serif";
    g.textAlign = "center";
    g.fillStyle = "#9a9aa4";
    g.strokeStyle = "#5a5a64";
    g.lineWidth = 1;
    for (let k = first; k <= last; k++) {
      const x = centerX + ((k * step - centerHz) / step) * PX_PER_STEP;
      const major = k % MAJOR_EVERY === 0;
      g.beginPath();
      g.moveTo(Math.round(x) + 0.5, HEIGHT_PX);
      g.lineTo(Math.round(x) + 0.5, HEIGHT_PX - (major ? 22 : 10));
      g.stroke();
      if (major) g.fillText((k * step / 1e6).toFixed(decimals), x, 16);
    }
    g.strokeStyle = ACCENT;
    g.lineWidth = 2;
    g.beginPath();
    g.moveTo(centerX, 0);
    g.lineTo(centerX, HEIGHT_PX);
    g.stroke();
  }
}

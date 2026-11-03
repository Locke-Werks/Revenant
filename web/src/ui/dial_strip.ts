// The dial strip: a tick ribbon under a fixed centre line. Dragging it retunes
// the active receiver and a flick coasts. The session logic lives in
// ../dial_session; this is the pointer wiring and the drawing.

import { PX_PER_STEP, fineStep } from "../dial";
import { DialSession, type DialDeps } from "../dial_session";

export interface DialStripDeps extends DialDeps {
  frame(fn: (tMs: number) => void): () => void;
}

// Finger travel before a touch counts as a drag. A thumb resting on the strip
// jitters by a pixel or two and must not retune.
const SLOP_PX = 4;
const HEIGHT_PX = 56;
const MAJOR_EVERY = 10;
const ACCENT = "#d6262a";
export const EMPTY_HINT = "Drag to tune. Long-press to add a receiver";

export class DialStrip {
  readonly el: HTMLElement;
  private readonly canvas: HTMLCanvasElement;
  private readonly session: DialSession;
  private pointer: number | null = null;
  private lastX = 0;
  private travelled = 0;
  private cancelFrame: (() => void) | null = null;

  constructor(private readonly deps: DialStripDeps) {
    this.session = new DialSession({
      ...deps,
      preview: (hz) => {
        const ok = deps.preview(hz);
        // The receiver only hears whole Hz, so a slow coast would not redraw
        // through the list's change event.
        this.draw();
        return ok;
      },
    });
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

  // Any other tune source calls this before it acts: what the dial already
  // showed is sent to its receiver, then it lets go.
  stop(): void {
    this.pointer = null;
    this.stopFrames();
    this.session.stop();
  }

  // After anything that can change the active receiver.
  check(): void {
    this.session.check();
    if (!this.session.moving) this.stopFrames();
  }

  private down(e: PointerEvent): void {
    if (this.pointer !== null) return;
    e.preventDefault();
    this.pointer = e.pointerId;
    this.canvas.setPointerCapture(e.pointerId);
    this.lastX = e.clientX;
    this.travelled = 0;
    this.stopFrames();
    this.session.down();
  }

  private move(e: PointerEvent): void {
    if (e.pointerId !== this.pointer) return;
    const dx = e.clientX - this.lastX;
    this.lastX = e.clientX;
    const before = this.travelled;
    this.travelled += Math.abs(dx);
    if (this.travelled < SLOP_PX) return;
    // The slop is paid out of the first move so the ribbon does not jump.
    this.session.drag(before < SLOP_PX ? Math.sign(dx) * (this.travelled - SLOP_PX) : dx, e.timeStamp);
  }

  private up(e: PointerEvent): void {
    if (e.pointerId !== this.pointer) return;
    this.pointer = null;
    this.session.release(e.timeStamp);
    if (this.session.moving) this.startFrames();
  }

  private startFrames(): void {
    if (this.cancelFrame) return;
    const tick = (t: number) => {
      this.cancelFrame = null;
      this.session.tick(t);
      if (this.session.moving) this.cancelFrame = this.deps.frame(tick);
    };
    this.cancelFrame = this.deps.frame(tick);
  }

  private stopFrames(): void {
    this.cancelFrame?.();
    this.cancelFrame = null;
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
    // With nothing to tune the strip stays, dimmed, and says how to begin.
    // A negative id is the radio's centre, tuned while no receiver is.
    const idle = !r || r.vrx < 0;
    this.el.classList.toggle("idle", idle);
    const hint = () => {
      g.font = "13px -apple-system, system-ui, sans-serif";
      g.textAlign = "center";
      g.textBaseline = "middle";
      g.fillStyle = "#8e8e97";
      g.fillText(EMPTY_HINT, w / 2, r ? HEIGHT_PX - 12 : HEIGHT_PX / 2);
    };
    if (!r) {
      hint();
      return;
    }
    const step = fineStep(r.mode);
    // The unrounded dial value while a session runs, so the ribbon is smooth
    // between the whole Hz the receiver is told.
    const centerHz = this.session.hz ?? r.hz;
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
    if (idle) hint();
  }
}

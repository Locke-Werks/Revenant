// A thin spectrum line above the waterfall, drawn from the latest row and
// placed by that row's own view, the same way the waterfall places it. The
// latest detections and the receivers' passbands sit under the line.

import type { DetectionBox } from "./markers";
import { drawReceivers, type ReceiverMark } from "./overlay";
import type { SpectrumFrame } from "./protocol";
import { hzToPx, rowTransform, type RowTransform, type View } from "./view";

export class SpectrumTrace {
  private ctx: CanvasRenderingContext2D | null;
  private row: SpectrumFrame | null = null;
  private view: View | null = null;
  private t: RowTransform = { a: 0, b: 0, limit: 0 };
  private boxes: readonly DetectionBox[] = [];
  private receivers: readonly ReceiverMark[] = [];
  private active: number | null = null;

  constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly accent: string,
  ) {
    this.ctx = canvas.getContext("2d");
  }

  resize(w: number, h: number): void {
    this.canvas.width = w;
    this.canvas.height = h;
  }

  setView(view: View): void {
    this.view = view;
  }

  // The frame's bytes are a view onto the message buffer; holding the latest
  // one keeps that buffer alive until the next row replaces it.
  setRow(f: SpectrumFrame): void {
    this.row = f;
  }

  setDetections(boxes: readonly DetectionBox[]): void {
    this.boxes = boxes;
  }

  setReceivers(list: readonly ReceiverMark[], active: number | null): void {
    this.receivers = list;
    this.active = active;
  }

  draw(): void {
    const ctx = this.ctx;
    if (!ctx) return;
    const w = this.canvas.width;
    const h = this.canvas.height;
    ctx.fillStyle = "#0a0a0c";
    ctx.fillRect(0, 0, w, h);
    const view = this.view;
    if (!view) return;
    this.drawBoxes(ctx, view, w, h);
    drawReceivers(ctx, view, this.receivers, this.active, this.accent);
    const f = this.row;
    if (!f || f.px.length === 0) return;
    const rw = f.px.length;
    const t = rowTransform(view, { centerHz: f.centerHz, spanHz: f.spanHz, widthPx: rw }, rw, this.t);
    // One sample per two device pixels is finer than the eye resolves at this
    // height and halves the path length.
    const step = 2;
    ctx.beginPath();
    let started = false;
    for (let x = 0; x <= w; x += step) {
      const u = t.a + t.b * (x / w);
      if (u < 0 || u >= 1) {
        started = false;
        continue;
      }
      const y = h - 1 - (f.px[Math.floor(u * rw)]! / 255) * (h - 2);
      if (started) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
      started = true;
    }
    ctx.strokeStyle = this.accent;
    ctx.lineWidth = Math.max(1, Math.round(window.devicePixelRatio || 1));
    ctx.stroke();
  }

  private drawBoxes(ctx: CanvasRenderingContext2D, view: View, w: number, h: number): void {
    if (this.boxes.length === 0) return;
    const v = { ...view, widthPx: w };
    const dpr = window.devicePixelRatio || 1;
    const fontPx = Math.round(10 * dpr);
    ctx.font = `${fontPx}px -apple-system, system-ui, sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (const b of this.boxes) {
      const x0 = Math.max(0, hzToPx(v, b.lowHz));
      const x1 = Math.min(w, hzToPx(v, b.highHz));
      if (!(x1 > x0)) continue;
      ctx.globalAlpha = b.alpha;
      ctx.fillStyle = this.accent;
      // The label's line is kept clear so text never sits on the trace fill.
      ctx.fillRect(x0, fontPx + 2, x1 - x0, h - fontPx - 2);
      if (b.label && ctx.measureText(b.label).width < x1 - x0) {
        ctx.globalAlpha = b.held ? 0.6 : 1;
        ctx.fillStyle = "#e6e6ea";
        ctx.fillText(b.label, (x0 + x1) / 2, 1);
      }
    }
    ctx.globalAlpha = 1;
  }
}

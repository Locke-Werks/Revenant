// A thin spectrum line above the waterfall, drawn from the latest row and
// placed by that row's own view, the same way the waterfall places it.

import type { SpectrumFrame } from "./protocol";
import { rowTransform, type RowTransform, type View } from "./view";

export class SpectrumTrace {
  private ctx: CanvasRenderingContext2D | null;
  private row: SpectrumFrame | null = null;
  private view: View | null = null;
  private t: RowTransform = { a: 0, b: 0, limit: 0 };

  constructor(private readonly canvas: HTMLCanvasElement) {
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

  draw(): void {
    const ctx = this.ctx;
    if (!ctx) return;
    const w = this.canvas.width;
    const h = this.canvas.height;
    ctx.fillStyle = "#1b1b1f";
    ctx.fillRect(0, 0, w, h);
    const f = this.row;
    const view = this.view;
    if (!f || !view || f.px.length === 0) return;
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
    ctx.strokeStyle = "#d6262a";
    ctx.lineWidth = Math.max(1, Math.round(window.devicePixelRatio || 1));
    ctx.stroke();
  }
}

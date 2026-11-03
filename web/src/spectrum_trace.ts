// A thin spectrum line above the waterfall, drawn from the latest row and
// placed by that row's own view, the same way the waterfall places it. The
// latest detections and the receivers' passbands sit under the line.
//
// Drawn as the desktop draws its trace: the row is the gateway's trace row,
// smoothed and already quantised to the desktop spectrum's window, so byte 255
// is the top of the plot; under it the theme as a translucent gradient by
// height, and the line in the theme's top colour.

import { DEFAULT_DISPLAY, fillStops, type DisplayColours } from "./display";
import { DETECTION_COLOR, type DetectionBox } from "./markers";
import { drawReceivers, type ReceiverMark } from "./overlay";
import type { SpectrumFrame } from "./protocol";
import { hzToPx, rowTransform, type RowTransform, type View } from "./view";

// The trace's polyline in canvas pixels, x and y interleaved, one run per
// stretch of the canvas the row covers. Byte 0 is the foot of the plot and 255
// its top, since the gateway quantised the row to the window it is drawn in.
export function traceRuns(
  f: Pick<SpectrumFrame, "centerHz" | "spanHz" | "tracePx">,
  view: View,
  w: number,
  h: number,
  scratch: RowTransform = { a: 0, b: 0, limit: 0 },
): number[][] {
  const px = f.tracePx;
  const rw = px.length;
  const t = rowTransform(view, { centerHz: f.centerHz, spanHz: f.spanHz, widthPx: rw }, rw, scratch);
  // One sample per two device pixels is finer than the eye resolves at this
  // height and halves the path length.
  const step = 2;
  const runs: number[][] = [];
  let run: number[] | null = null;
  for (let x = 0; x <= w; x += step) {
    const u = t.a + t.b * (x / w);
    if (u < 0 || u >= 1) {
      run = null;
      continue;
    }
    const y = h - 1 - (px[Math.floor(u * rw)]! / 255) * (h - 2);
    if (!run) {
      run = [];
      runs.push(run);
    }
    run.push(x, y);
  }
  return runs;
}

export class SpectrumTrace {
  private ctx: CanvasRenderingContext2D | null;
  private row: SpectrumFrame | null = null;
  private view: View | null = null;
  private t: RowTransform = { a: 0, b: 0, limit: 0 };
  private boxes: readonly DetectionBox[] = [];
  private receivers: readonly ReceiverMark[] = [];
  private active: number | null = null;
  private colours: DisplayColours = DEFAULT_DISPLAY;
  private fill: CanvasGradient | null = null;
  private fillHeight = 0;

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

  setColours(colours: DisplayColours): void {
    this.colours = colours;
    this.fill = null;
  }

  private gradient(ctx: CanvasRenderingContext2D, h: number): CanvasGradient {
    if (this.fill && this.fillHeight === h) return this.fill;
    const g = ctx.createLinearGradient(0, 0, 0, h);
    for (const s of fillStops(this.colours.spectrum)) g.addColorStop(s.at, s.rgba);
    this.fill = g;
    this.fillHeight = h;
    return g;
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
    ctx.fillStyle = this.colours.background;
    ctx.fillRect(0, 0, w, h);
    const view = this.view;
    if (!view) return;
    this.drawBoxes(ctx, view, w, h);
    drawReceivers(ctx, view, this.receivers, this.active, this.accent);
    const f = this.row;
    if (!f || f.tracePx.length === 0) return;
    const runs = traceRuns(f, view, w, h, this.t);
    if (runs.length === 0) return;
    // The fill closes each run down to the foot of the plot.
    ctx.beginPath();
    for (const run of runs) {
      ctx.moveTo(run[0]!, h);
      for (let i = 0; i < run.length; i += 2) ctx.lineTo(run[i]!, run[i + 1]!);
      ctx.lineTo(run[run.length - 2]!, h);
      ctx.closePath();
    }
    ctx.fillStyle = this.gradient(ctx, h);
    ctx.fill();
    ctx.beginPath();
    for (const run of runs) {
      ctx.moveTo(run[0]!, run[1]!);
      for (let i = 2; i < run.length; i += 2) ctx.lineTo(run[i]!, run[i + 1]!);
    }
    ctx.strokeStyle = this.colours.trace;
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
      ctx.fillStyle = DETECTION_COLOR;
      // The label's line is kept clear so text never sits on the trace fill.
      const top = fontPx + 2;
      ctx.globalAlpha = b.alpha * 1.5;
      ctx.fillRect(x0, top, x1 - x0, h - top);
      ctx.globalAlpha = b.held ? 0.35 : 0.7;
      ctx.fillRect(Math.floor(x0), top, 1, h - top);
      ctx.fillRect(Math.max(Math.floor(x0) + 1, Math.ceil(x1) - 1), top, 1, h - top);
      if (b.label && ctx.measureText(b.label).width < x1 - x0) {
        ctx.globalAlpha = b.held ? 0.5 : 0.85;
        ctx.fillStyle = "#b8c4d0";
        ctx.fillText(b.label, (x0 + x1) / 2, 1);
      }
    }
    ctx.globalAlpha = 1;
  }
}

// The frequency scale between the spectrum trace and the waterfall. Labels sit
// on round 1, 2, 5 steps, the widest that still keeps them apart, so the
// numbers stay short and their spacing reads at a glance.

import { hzToPx, type View } from "../view";

export interface Tick {
  hz: number;
  x: number;
  label: string;
}

// The smallest 1, 2, 5 step leaving at least minLabelPx between labels.
export function tickStepHz(spanHz: number, widthPx: number, minLabelPx: number): number {
  if (!(spanHz > 0) || !(widthPx > 0)) return 1;
  const least = (spanHz * minLabelPx) / widthPx;
  let decade = 10 ** Math.floor(Math.log10(least));
  for (;;) {
    for (const m of [1, 2, 5]) {
      // A relative tolerance, so 5e5 computed as 4.9999999e5 still counts.
      if (m * decade >= least * (1 - 1e-9)) return m * decade;
    }
    decade *= 10;
  }
}

// Positions are in the units of widthPx.
export function rulerTicks(view: View, widthPx: number, minLabelPx: number): Tick[] {
  const step = tickStepHz(view.spanHz, widthPx, minLabelPx);
  const decimals = Math.max(0, Math.ceil(-Math.log10(step / 1e6) - 1e-9));
  const v = { ...view, widthPx };
  const lo = view.centerHz - view.spanHz / 2;
  const hi = view.centerHz + view.spanHz / 2;
  const out: Tick[] = [];
  for (let n = Math.ceil(lo / step); n * step <= hi; n++) {
    const hz = n * step;
    out.push({ hz, x: hzToPx(v, hz), label: (hz / 1e6).toFixed(decimals) });
  }
  return out;
}

export class Ruler {
  private readonly ctx: CanvasRenderingContext2D | null;
  private view: View | null = null;

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

  draw(): void {
    const ctx = this.ctx;
    if (!ctx) return;
    const w = this.canvas.width;
    const h = this.canvas.height;
    ctx.clearRect(0, 0, w, h);
    if (!this.view) return;
    const dpr = window.devicePixelRatio || 1;
    ctx.font = `${Math.round(10 * dpr)}px ui-monospace, "SF Mono", Menlo, monospace`;
    ctx.textAlign = "center";
    ctx.textBaseline = "bottom";
    const tickH = Math.round(4 * dpr);
    for (const t of rulerTicks(this.view, w, 70 * dpr)) {
      const x = Math.round(t.x) + 0.5;
      ctx.fillStyle = "#6b6b74";
      ctx.fillRect(x - 0.5, h - tickH, Math.max(1, Math.round(dpr)), tickH);
      ctx.fillStyle = "#9a9aa3";
      ctx.fillText(t.label, x, h - tickH - 1);
    }
  }
}

// Geometry for what is drawn over the spectrum and waterfall: detection boxes
// and receiver passbands. Pure, so the hertz-to-pixel arithmetic is tested
// without a canvas.

import type { Detection } from "./detections";
import { hzToPx, type View } from "./view";

// Used when the --accent CSS variable is missing or not a hex colour.
export const DEFAULT_ACCENT = "#d6262a";

// The 2D waterfall blends the accent into raw pixels, which needs numbers.
export function parseHexColor(css: string): [number, number, number] {
  let h = css.trim().replace(/^#/, "");
  if (/^[0-9a-f]{3}$/i.test(h)) h = h.replace(/./g, (c) => c + c);
  if (!/^[0-9a-f]{6}$/i.test(h)) return parseHexColor(DEFAULT_ACCENT);
  const n = parseInt(h, 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

export interface DetectionBox {
  lowHz: number;
  highHz: number;
  alpha: number;
  held: boolean;
  label: string;
}

// A box at zero confidence still has to be visible to be tapped, and one at
// full confidence must not hide the waterfall under it.
const MIN_ALPHA = 0.12;
const MAX_ALPHA = 0.45;
// Held tracks are the engine's memory of a signal that has gone quiet.
const HELD_FACTOR = 0.5;

export function detectionBoxes(d: readonly Detection[]): DetectionBox[] {
  const out: DetectionBox[] = [];
  for (const x of d) {
    const half = Math.max(0, x.bandwidthHz) / 2;
    const c = Math.min(1, Math.max(0, x.confidence));
    let alpha = MIN_ALPHA + (MAX_ALPHA - MIN_ALPHA) * c;
    if (x.state === "held") alpha *= HELD_FACTOR;
    out.push({ lowHz: x.centerHz - half, highHz: x.centerHz + half, alpha, held: x.state === "held", label: x.label });
  }
  return out;
}

// What a receiver demodulates when the gateway has not said its bandwidth:
// the desktop CLI's per-mode defaults, as offsets from the carrier, so the
// phone and the desktop draw the same passband for the same receiver.
const DEFAULT_PASSBAND: Record<string, [number, number]> = {
  am: [-5_000, 5_000],
  sam: [-5_000, 5_000],
  nfm: [-8_000, 8_000],
  wfm: [-100_000, 100_000],
  usb: [300, 2_700],
  lsb: [-2_700, -300],
  dsb: [-3_000, 3_000],
  cw: [-250, 250],
  raw: [-6_000, 6_000],
};
const FALLBACK_PASSBAND: [number, number] = [-6_000, 6_000];

// Sidebands sit on one side of the carrier; everything else straddles it.
export function passbandHz(mode: string, hz: number, bandwidthHz: number | null): { lowHz: number; highHz: number } {
  if (bandwidthHz === null || !(bandwidthHz > 0)) {
    const [lo, hi] = DEFAULT_PASSBAND[mode] ?? FALLBACK_PASSBAND;
    return { lowHz: hz + lo, highHz: hz + hi };
  }
  const bw = bandwidthHz;
  if (mode === "usb") return { lowHz: hz, highHz: hz + bw };
  if (mode === "lsb") return { lowHz: hz - bw, highHz: hz };
  return { lowHz: hz - bw / 2, highHz: hz + bw / 2 };
}

export interface PassbandPx {
  x0: number;
  x1: number;
  xc: number;
}

// Pixels across a canvas `widthPx` wide showing `view`. A passband narrower
// than a pixel is widened to one so it never vanishes when zoomed out; null
// when it is wholly off screen.
export function passbandPx(
  view: View,
  widthPx: number,
  r: { hz: number; mode: string; bandwidthHz: number | null },
): PassbandPx | null {
  const v = { ...view, widthPx };
  const p = passbandHz(r.mode, r.hz, r.bandwidthHz);
  let x0 = hzToPx(v, p.lowHz);
  let x1 = hzToPx(v, p.highHz);
  const xc = hzToPx(v, r.hz);
  if (x1 < 0 || x0 > widthPx) return null;
  if (x1 - x0 < 1) {
    const mid = (x0 + x1) / 2;
    x0 = mid - 0.5;
    x1 = mid + 0.5;
  }
  return { x0, x1, xc };
}

// A box's span in pixels on screen, clipped to the hertz the row it sits on
// actually covered, so a box never hangs past the edge of an old row after a
// retune. Null when nothing of it is on screen.
export function boxPx(
  view: View,
  widthPx: number,
  row: { centerHz: number; spanHz: number },
  box: { lowHz: number; highHz: number },
): { x0: number; x1: number } | null {
  const lo = Math.max(box.lowHz, row.centerHz - row.spanHz / 2);
  const hi = Math.min(box.highHz, row.centerHz + row.spanHz / 2);
  if (!(hi > lo)) return null;
  const v = { ...view, widthPx };
  const x0 = Math.max(0, hzToPx(v, lo));
  const x1 = Math.min(widthPx, hzToPx(v, hi));
  if (!(x1 > x0)) return null;
  return { x0, x1 };
}

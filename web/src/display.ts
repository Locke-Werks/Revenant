// The desktop's display colours, as the gateway's "display" event carries
// them. The phone has no display settings of its own: the gateway applies the
// desktop's scale to every row and sends the colours to draw it with, and this
// only decodes them.

import {
  DEFAULT_BACKGROUND,
  DEFAULT_SPECTRUM_HEX,
  DEFAULT_TRACE,
  DEFAULT_WATERFALL_HEX,
} from "./display_defaults";

export interface DisplayColours {
  // 256 RGBA entries each, indexed by a row's byte.
  waterfall: Uint8Array;
  spectrum: Uint8Array;
  // "#rrggbb".
  trace: string;
  background: string;
}

const HEX_COLOUR = /^#[0-9a-f]{6}$/i;
const HEX_PALETTE = /^[0-9a-f]{1536}$/i;

// 256 rrggbb entries to RGBA, or null for anything else.
export function paletteFromHex(hex: string): Uint8Array | null {
  if (!HEX_PALETTE.test(hex)) return null;
  const out = new Uint8Array(256 * 4);
  for (let i = 0; i < 256; i++) {
    out[i * 4] = parseInt(hex.slice(i * 6, i * 6 + 2), 16);
    out[i * 4 + 1] = parseInt(hex.slice(i * 6 + 2, i * 6 + 4), 16);
    out[i * 4 + 2] = parseInt(hex.slice(i * 6 + 4, i * 6 + 6), 16);
    out[i * 4 + 3] = 255;
  }
  return out;
}

export function parseDisplay(ev: { [key: string]: unknown }): DisplayColours | null {
  const { waterfall, spectrum, trace, background } = ev;
  if (typeof waterfall !== "string" || typeof spectrum !== "string") return null;
  if (typeof trace !== "string" || !HEX_COLOUR.test(trace)) return null;
  if (typeof background !== "string" || !HEX_COLOUR.test(background)) return null;
  const w = paletteFromHex(waterfall);
  const s = paletteFromHex(spectrum);
  if (!w || !s) return null;
  return { waterfall: w, spectrum: s, trace, background };
}

export const DEFAULT_DISPLAY: DisplayColours = {
  waterfall: paletteFromHex(DEFAULT_WATERFALL_HEX)!,
  spectrum: paletteFromHex(DEFAULT_SPECTRUM_HEX)!,
  trace: DEFAULT_TRACE,
  background: DEFAULT_BACKGROUND,
};

// The desktop's fill under the trace is translucent so the line stays legible
// where it crosses the bright end: alpha 140 of 255.
export const FILL_ALPHA = 140 / 255;

export interface FillStop {
  // 0 at the top of the plot, 1 at its foot.
  at: number;
  rgba: string;
}

// A vertical gradient of the spectrum palette, brightest at the top, in
// `count` stops: the desktop's fill is a function of height alone, so a quiet
// column's fill is as dark as its height says.
export function fillStops(spectrum: Uint8Array, count = 17): FillStop[] {
  const out: FillStop[] = [];
  const alpha = FILL_ALPHA.toFixed(3);
  for (let k = 0; k < count; k++) {
    const at = k / (count - 1);
    const i = Math.round((1 - at) * 255) * 4;
    out.push({ at, rgba: `rgba(${spectrum[i]}, ${spectrum[i + 1]}, ${spectrum[i + 2]}, ${alpha})` });
  }
  return out;
}

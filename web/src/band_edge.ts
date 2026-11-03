// Where a receiver may sit inside what the radio is delivering, and where to
// move the radio when a tune would leave it. Pure, so the controller's edge
// handling is testable without a link.

export interface Span {
  lowHz: number;
  highHz: number;
}

export interface TuneRange {
  lowHz: number;
  highHz: number;
}

// Where the receiver lands after a recentre, as a fraction of the span in
// from the edge it came from, so most of the span lies ahead of it and the
// next recentre is far off.
export const RECENTRE_FRACTION = 0.25;

// The usable part of the band: the source's own edges, narrowed to where the
// spectrum bins really lie when the gateway says.
export function usableSpan(centerHz: number, rateHz: number, dataLowHz?: number, dataHighHz?: number): Span {
  let lowHz = centerHz - rateHz / 2;
  let highHz = centerHz + rateHz / 2;
  if (typeof dataLowHz === "number" && Number.isFinite(dataLowHz)) lowHz = Math.max(lowHz, dataLowHz);
  if (typeof dataHighHz === "number" && Number.isFinite(dataHighHz)) highHz = Math.min(highHz, dataHighHz);
  return { lowHz, highHz };
}

// Passband edges as offsets from the carrier.
export interface Offsets {
  lowOff: number;
  highOff: number;
}

export function fits(span: Span, off: Offsets, hz: number): boolean {
  return hz + off.lowOff >= span.lowHz && hz + off.highOff <= span.highHz;
}

// The nearest carrier whose passband stays inside the span. A passband wider
// than the span centres on it.
export function clampInto(span: Span, off: Offsets, hz: number): number {
  const lo = span.lowHz - off.lowOff;
  const hi = span.highHz - off.highOff;
  if (lo > hi) return (span.lowHz + span.highHz) / 2 - (off.lowOff + off.highOff) / 2;
  return Math.min(Math.max(hz, lo), hi);
}

// Nearest frequency the tuner can reach. Null ranges means unknown, which
// leaves the engine to refuse; an empty list means it cannot tune at all.
export function clampToRanges(hz: number, ranges: TuneRange[] | null): number {
  if (!ranges || ranges.length === 0) return Math.max(1, hz);
  let best = hz;
  let bestD = Number.POSITIVE_INFINITY;
  for (const r of ranges) {
    const c = Math.min(Math.max(hz, r.lowHz), r.highHz);
    const d = Math.abs(c - hz);
    if (d < bestD) {
      bestD = d;
      best = c;
    }
  }
  return best;
}

// The centre that puts hz RECENTRE_FRACTION of the span in from the trailing
// edge, leaving the rest ahead in the direction of travel. The span keeps its width and its offsets from the centre.
export function recentreFor(span: Span, centerHz: number, hz: number, ranges: TuneRange[] | null): number {
  const width = span.highHz - span.lowHz;
  const mid = (span.lowHz + span.highHz) / 2;
  const up = hz >= mid;
  // Where the carrier sits relative to the usable span's middle afterwards.
  const fromMid = (up ? -1 : 1) * (0.5 - RECENTRE_FRACTION) * width;
  const newMid = hz - fromMid;
  return Math.round(clampToRanges(newMid + (centerHz - mid), ranges));
}

export function shift(span: Span, byHz: number): Span {
  return { lowHz: span.lowHz + byHz, highHz: span.highHz + byHz };
}

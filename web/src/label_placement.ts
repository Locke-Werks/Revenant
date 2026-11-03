// Where detection labels go on the label line above the spectrum.
//
// At a full span most bands are a few pixels wide, so a label that had to fit
// inside its band was almost never drawn. A label is centred over its band
// and may spill past it, as long as it stays on the canvas and clear of the
// label placed before it. Bands are taken left to right and the first to
// claim a stretch keeps it, which is simple and stable from frame to frame.

export interface LabelBand {
  // The band's edges and the label's measured width, in canvas pixels.
  x0: number;
  x1: number;
  width: number;
}

// The centre x for each band's label, or null where it does not fit.
export function placeLabels(bands: readonly LabelBand[], canvasWidth: number, gap = 4): (number | null)[] {
  const out: (number | null)[] = bands.map(() => null);
  const order = bands.map((_, i) => i).sort((a, b) => bands[a]!.x0 + bands[a]!.x1 - (bands[b]!.x0 + bands[b]!.x1));
  let lastRight = -Infinity;
  for (const i of order) {
    const b = bands[i]!;
    if (!(b.width > 0) || b.width > canvasWidth) continue;
    const half = b.width / 2;
    // Pulled inside the canvas, so a band at the edge still gets its label.
    const centre = Math.min(canvasWidth - half, Math.max(half, (b.x0 + b.x1) / 2));
    if (centre - half < lastRight + gap) continue;
    out[i] = centre;
    lastRight = centre + half;
  }
  return out;
}

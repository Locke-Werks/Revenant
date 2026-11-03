// What part of the spectrum the waterfall shows, and the arithmetic between
// pixels and hertz. Pure, so the gestures and the renderer share one tested
// definition of where a frequency lands.

export interface View {
  centerHz: number;
  spanHz: number;
  // Device pixels across the waterfall: what the gateway cuts rows to.
  widthPx: number;
}

// The gateway refuses wider rows than this (kMaxWidthPx).
export const MAX_WIDTH_PX = 8192;

// Narrower than this and a row is a handful of FFT bins smeared across the
// screen; there is nothing more to see.
export const MIN_SPAN_HZ = 1000;

// x runs 0 at the left edge to widthPx at the right edge; the centre frequency
// sits at widthPx / 2.
export function pxToHz(view: View, x: number): number {
  return view.centerHz + (x / view.widthPx - 0.5) * view.spanHz;
}

export function hzToPx(view: View, hz: number): number {
  return ((hz - view.centerHz) / view.spanHz + 0.5) * view.widthPx;
}

// Positive dHz moves the window up in frequency.
export function panBy(view: View, dHz: number): View {
  return { ...view, centerHz: view.centerHz + dHz };
}

// factor > 1 zooms in. The anchor frequency stays at the same pixel, which is
// what keeps a signal under the fingers during a pinch.
export function zoomAt(view: View, factor: number, anchorHz: number): View {
  if (!(factor > 0) || !Number.isFinite(factor)) return view;
  const spanHz = view.spanHz / factor;
  const frac = (anchorHz - view.centerHz) / view.spanHz;
  return { ...view, spanHz, centerHz: anchorHz - frac * spanHz };
}

export interface Band {
  centerHz: number;
  rateHz: number;
}

// Keeps the view inside what the source delivers: no wider than its rate, no
// narrower than MIN_SPAN_HZ, and its edges inside the source's band.
export function clampView(view: View, band: Band): View {
  const maxSpan = Math.max(band.rateHz, MIN_SPAN_HZ);
  const spanHz = Math.min(Math.max(view.spanHz, MIN_SPAN_HZ), maxSpan);
  const half = (maxSpan - spanHz) / 2;
  const centerHz = Math.min(Math.max(view.centerHz, band.centerHz - half), band.centerHz + half);
  const widthPx = Math.min(Math.max(1, Math.round(view.widthPx)), MAX_WIDTH_PX);
  return { centerHz, spanHz, widthPx };
}

// The engine's spectrum starts half a coarse channel below the band and so
// ends half a channel short of its top (SpectrumGeometry::bin_zero_hz), and
// the gateway fills pixels past the spectrum with 0. Allowing the view out to
// the band's nominal edge leaves an empty strip at the right of every row. A
// row that reaches a band edge and ends in a long run of zeros says where the
// data really stops; null when this row says nothing about that.
// Shorter runs are left alone: a quiet stretch of max-reduced noise rarely
// sits below the floor for more than a few pixels together.
export const EDGE_RUN_FRACTION = 0.01;

export function rowCoverage(
  row: { centerHz: number; spanHz: number; px: Uint8Array },
  band: Band,
): { lowHz: number | null; highHz: number | null } {
  const n = row.px.length;
  const out = { lowHz: null as number | null, highHz: null as number | null };
  if (n === 0 || !(row.spanHz > 0)) return out;
  const pxHz = row.spanHz / n;
  const minRun = Math.max(4, Math.ceil(n * EDGE_RUN_FRACTION));
  const rowLo = row.centerHz - row.spanHz / 2;
  const rowHi = row.centerHz + row.spanHz / 2;
  const bandLo = band.centerHz - band.rateHz / 2;
  const bandHi = band.centerHz + band.rateHz / 2;
  let r = 0;
  while (r < n && row.px[n - 1 - r] === 0) r++;
  if (r >= minRun && r < n && rowHi >= bandHi - pxHz) out.highHz = rowHi - r * pxHz;
  let l = 0;
  while (l < n && row.px[l] === 0) l++;
  if (l >= minRun && l < n && rowLo <= bandLo + pxHz) out.lowHz = rowLo + l * pxHz;
  return out;
}

// The band trimmed to where the spectrum has data.
export function coveredBand(band: Band, lowHz: number | null, highHz: number | null): Band {
  const lo = Math.max(band.centerHz - band.rateHz / 2, lowHz ?? -Infinity);
  const hi = Math.min(band.centerHz + band.rateHz / 2, highHz ?? Infinity);
  if (!(hi > lo)) return band;
  return { centerHz: (lo + hi) / 2, rateHz: hi - lo };
}

export function sameView(a: View | null, b: View | null): boolean {
  if (a === null || b === null) return a === b;
  return a.centerHz === b.centerHz && a.spanHz === b.spanHz && a.widthPx === b.widthPx;
}

// The row a frame was cut for and the view on screen differ while the gateway
// catches up with a pan or zoom. For a screen position s in [0, 1] the row's
// texel coordinate, as a fraction of a texture `texWidth` texels wide, is
// a + b * s; texels past `limit` hold nothing for this row. Computed in doubles
// on the CPU because centre frequencies near 1 GHz do not survive float32.
export interface RowTransform {
  a: number;
  b: number;
  limit: number;
}

export function rowTransform(
  current: { centerHz: number; spanHz: number },
  row: { centerHz: number; spanHz: number; widthPx: number },
  texWidth: number,
  out: RowTransform = { a: 0, b: 0, limit: 0 },
): RowTransform {
  const b = current.spanHz / row.spanHz;
  const a = (current.centerHz - row.centerHz) / row.spanHz + 0.5 - 0.5 * b;
  const k = row.widthPx / texWidth;
  out.a = a * k;
  out.b = b * k;
  out.limit = k;
  return out;
}

// setView goes out at most once per animation frame, and only when the view
// differs from what was last sent. A pinch moves the view on every pointer
// event; the gateway only needs where it ended up.
export class ViewSync {
  private latest: View | null = null;
  private sent: View | null = null;
  private scheduled = false;

  constructor(
    private readonly send: (v: View) => void,
    private readonly schedule: (fn: () => void) => void,
  ) {}

  update(view: View): void {
    this.latest = view;
    if (this.scheduled) return;
    this.scheduled = true;
    this.schedule(() => {
      this.scheduled = false;
      if (this.latest !== null && !sameView(this.latest, this.sent)) {
        this.sent = this.latest;
        this.send(this.latest);
      }
    });
  }

  // After a reconnect the gateway holds no view for this socket (or a resumed
  // one that may be stale), so the next update must go out even if unchanged.
  forget(): void {
    this.sent = null;
  }
}

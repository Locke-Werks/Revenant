// The gateway quantises each row to bytes over a fixed dB range, so a quiet
// band uses a sliver of the colour map. The phone tracks the floor and the
// peak of what is in view and stretches each row's bytes across that range,
// slowly enough that a passing burst does not pump the picture.

export interface Scale {
  floor: number;
  ceiling: number;
}

export interface RowDb {
  offsetDb: number;
  scaleDb: number;
  px: Uint8Array;
}

// Below this the remap would turn noise into full-scale colour.
export const MIN_RANGE_DB = 10;

export class AutoScale {
  private s: Scale | null = null;
  private t = 0;

  constructor(private readonly tauMs = 30_000) {}

  update(minDb: number, maxDb: number, tMs: number): Scale {
    const lo = Math.min(minDb, maxDb);
    const hi = Math.max(minDb, maxDb);
    if (!this.s) {
      this.s = widen(lo, hi);
      this.t = tMs;
      return { ...this.s };
    }
    const dt = tMs - this.t;
    if (!(dt > 0) || !Number.isFinite(lo) || !Number.isFinite(hi)) return { ...this.s };
    this.t = tMs;
    // Exact for any row rate, so a stalled link that resumes catches up in
    // proportion to the time it was away.
    const k = 1 - Math.exp(-dt / this.tauMs);
    this.s = widen(this.s.floor + (lo - this.s.floor) * k, this.s.ceiling + (hi - this.s.ceiling) * k);
    return { ...this.s };
  }
}

function widen(floor: number, ceiling: number): Scale {
  if (ceiling - floor >= MIN_RANGE_DB) return { floor, ceiling };
  const mid = (floor + ceiling) / 2;
  return { floor: mid - MIN_RANGE_DB / 2, ceiling: mid + MIN_RANGE_DB / 2 };
}

// The floor is a low percentile rather than the minimum, which a single dead
// bin at the band edge would drag down.
const FLOOR_PERCENTILE = 0.1;
const hist = new Uint32Array(256);

export function rowExtentsDb(row: RowDb): { minDb: number; maxDb: number } {
  const px = row.px;
  hist.fill(0);
  let peak = 0;
  for (let i = 0; i < px.length; i++) {
    const v = px[i]!;
    hist[v]!++;
    if (v > peak) peak = v;
  }
  const want = Math.floor(px.length * FLOOR_PERCENTILE);
  let seen = 0;
  let low = 0;
  for (; low < 256; low++) {
    seen += hist[low]!;
    if (seen > want) break;
  }
  return { minDb: row.offsetDb + low * row.scaleDb, maxDb: row.offsetDb + peak * row.scaleDb };
}

// Writes the row's bytes stretched over `s` into `out`, which must be at least
// as long as the row, and returns the filled part.
export function remapRow(row: RowDb, s: Scale, out: Uint8Array): Uint8Array {
  const lut = new Uint8Array(256);
  const k = 255 / (s.ceiling - s.floor);
  for (let v = 0; v < 256; v++) {
    const db = row.offsetDb + v * row.scaleDb;
    lut[v] = Math.max(0, Math.min(255, Math.round((db - s.floor) * k)));
  }
  const n = row.px.length;
  const dst = out.subarray(0, n);
  for (let i = 0; i < n; i++) dst[i] = lut[row.px[i]!]!;
  return dst;
}

// The waterfall's palette: the app background at the noise floor, through the
// accent red, to yellow-white for the strongest signals. Built once as a 256
// entry RGBA table, indexed by the row's byte value.

const STOPS: [number, number, number, number][] = [
  [0.0, 0x14, 0x14, 0x18],
  [0.25, 0x1b, 0x1b, 0x1f],
  [0.45, 0x5a, 0x14, 0x22],
  [0.65, 0xd6, 0x26, 0x2a],
  [0.85, 0xf2, 0x8c, 0x28],
  [1.0, 0xff, 0xf0, 0xa0],
];

export function colormap(): Uint8Array {
  const out = new Uint8Array(256 * 4);
  for (let i = 0; i < 256; i++) {
    const t = i / 255;
    let j = 0;
    while (j < STOPS.length - 2 && t > STOPS[j + 1]![0]) j++;
    const a = STOPS[j]!;
    const b = STOPS[j + 1]!;
    const f = Math.min(1, Math.max(0, (t - a[0]) / (b[0] - a[0])));
    out[i * 4] = Math.round(a[1] + (b[1] - a[1]) * f);
    out[i * 4 + 1] = Math.round(a[2] + (b[2] - a[2]) * f);
    out[i * 4 + 2] = Math.round(a[3] + (b[3] - a[3]) * f);
    out[i * 4 + 3] = 255;
  }
  return out;
}

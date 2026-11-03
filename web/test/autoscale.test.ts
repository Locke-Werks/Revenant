import { test } from "node:test";
import assert from "node:assert/strict";
import { AutoScale, remapRow, rowExtentsDb } from "../src/autoscale";
import { colormap } from "../src/colormap";
import { rulerTicks, tickStepHz } from "../src/ui/ruler";
import { isEditable } from "../src/insets";
import { clampView, coveredBand, rowCoverage } from "../src/view";

const TAU = 30_000;

test("the viewport pin leaves the scroll alone while something takes typing", () => {
  assert.equal(isEditable({ tagName: "INPUT" }), true);
  assert.equal(isEditable({ tagName: "INPUT", type: "text" }), true);
  assert.equal(isEditable({ tagName: "SELECT" }), true);
  assert.equal(isEditable({ tagName: "DIV", isContentEditable: true }), true);
  assert.equal(isEditable({ tagName: "INPUT", type: "range" }), false);
  assert.equal(isEditable({ tagName: "BUTTON" }), false);
  assert.equal(isEditable({ tagName: "BODY" }), false);
  assert.equal(isEditable(null), false);
});

test("a row ending in a long floor run at the band edge trims the band to the data", () => {
  const band = { centerHz: 98_100_000, rateHz: 2_400_000 };
  const px = new Uint8Array(780).fill(40);
  px.fill(0, 730);
  const c = rowCoverage({ centerHz: band.centerHz, spanHz: band.rateHz, px }, band);
  assert.equal(c.lowHz, null);
  assert.ok(Math.abs(c.highHz! - (96_900_000 + (730 * 2_400_000) / 780)) < 1);
  const b = coveredBand(band, c.lowHz, c.highHz);
  const v = clampView({ centerHz: band.centerHz, spanHz: band.rateHz, widthPx: 780 }, b);
  // The whole view now lies on data, so the row fills the width.
  assert.ok(v.centerHz + v.spanHz / 2 <= c.highHz! + 1e-6);
  assert.ok(v.centerHz - v.spanHz / 2 >= 96_900_000 - 1e-6);
  // A few quiet pixels, or a row that stops short of the band edge, say nothing.
  const quiet = new Uint8Array(780).fill(40);
  quiet.fill(0, 777);
  assert.equal(rowCoverage({ centerHz: band.centerHz, spanHz: band.rateHz, px: quiet }, band).highHz, null);
  assert.equal(rowCoverage({ centerHz: band.centerHz - 500_000, spanHz: 1_000_000, px }, band).highHz, null);
});

test("autoscale snaps to the first row", () => {
  const a = new AutoScale(TAU);
  assert.deepEqual(a.update(-120, -40, 0), { floor: -120, ceiling: -40 });
});

test("autoscale converges to a step change within about three tau, both extents moving", () => {
  const a = new AutoScale(TAU);
  a.update(-120, -40, 0);
  let s = { floor: 0, ceiling: 0 };
  // 20 rows a second, as the gateway sends them.
  for (let t = 50; t <= 3 * TAU; t += 50) s = a.update(-100, -20, t);
  assert.ok(Math.abs(s.floor - -100) < 0.06 * 20, `floor ${s.floor}`);
  assert.ok(Math.abs(s.ceiling - -20) < 0.06 * 20, `ceiling ${s.ceiling}`);
  // Half way through it has moved, but not arrived.
  const b = new AutoScale(TAU);
  b.update(-120, -40, 0);
  for (let t = 50; t <= TAU; t += 50) s = b.update(-100, -20, t);
  assert.ok(s.floor > -115 && s.floor < -101, `floor ${s.floor}`);
  assert.ok(s.ceiling > -35 && s.ceiling < -21, `ceiling ${s.ceiling}`);
});

test("autoscale never inverts, even when the inputs do", () => {
  const a = new AutoScale(TAU);
  a.update(-120, -40, 0);
  for (let t = 50; t <= 4 * TAU; t += 50) {
    const s = a.update(-20, -110, t);
    assert.ok(s.ceiling > s.floor, `at ${t}: ${s.floor} .. ${s.ceiling}`);
  }
  const b = new AutoScale(TAU);
  const s = b.update(-60, -60, 0);
  assert.ok(s.ceiling > s.floor);
});

test("autoscale ignores a clock that runs backwards or stalls", () => {
  const a = new AutoScale(TAU);
  a.update(-120, -40, 1000);
  const s = a.update(0, 50, 500);
  assert.deepEqual(s, { floor: -120, ceiling: -40 });
});

test("row extents: the floor is the low body of the row, the ceiling its peak", () => {
  const px = new Uint8Array(1000).fill(20);
  px[500] = 200;
  const e = rowExtentsDb({ offsetDb: -150, scaleDb: 0.5, px });
  assert.equal(e.minDb, -150 + 20 * 0.5);
  assert.equal(e.maxDb, -150 + 200 * 0.5);
});

test("remap stretches the floor to 0 and the ceiling to 255", () => {
  const px = Uint8Array.from([20, 60, 100, 0, 255]);
  const out = remapRow({ offsetDb: -150, scaleDb: 0.5, px }, { floor: -140, ceiling: -100 }, new Uint8Array(5));
  assert.deepEqual([...out], [0, 128, 255, 0, 255]);
});

// CIE L* from sRGB, the standard D65 formula.
function lightness(r: number, g: number, b: number): number {
  const lin = (c: number) => {
    const s = c / 255;
    return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  const y = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
  return y > 216 / 24389 ? 116 * Math.cbrt(y) - 16 : (24389 / 27) * y;
}

test("colour map lightness rises monotonically", () => {
  const m = colormap();
  let prev = -1;
  for (let i = 0; i < 256; i++) {
    const l = lightness(m[i * 4]!, m[i * 4 + 1]!, m[i * 4 + 2]!);
    assert.ok(l >= prev, `entry ${i}: L* ${l.toFixed(2)} after ${prev.toFixed(2)}`);
    prev = l;
  }
  assert.ok(prev > 90, "the top reaches near white");
});

test("ruler step is a 1, 2, 5 round number giving labels room", () => {
  // 2.4 MHz across 390 CSS px, labels at least 70 px apart: no more than 5 labels.
  assert.equal(tickStepHz(2_400_000, 390, 70), 500_000);
  assert.equal(tickStepHz(200_000, 390, 70), 50_000);
  assert.equal(tickStepHz(10_000, 390, 70), 2_000);
  assert.equal(tickStepHz(25_000, 390, 70), 5_000);
  assert.equal(tickStepHz(1_000_000, 390, 70), 200_000);
  for (const span of [1_234, 56_789, 3_200_000, 61_440_000]) {
    const step = tickStepHz(span, 390, 70);
    const mant = step / 10 ** Math.floor(Math.log10(step));
    assert.ok([1, 2, 5].includes(Math.round(mant)), `${span}: ${step}`);
    assert.ok((step / span) * 390 >= 70, `${span}: ${step} too tight`);
    assert.ok(((step / 2.5) / span) * 390 < 70, `${span}: ${step} too loose`);
  }
});

test("ruler ticks fall on multiples of the step, labelled to that precision", () => {
  const t = rulerTicks({ centerHz: 145_000_000, spanHz: 2_400_000, widthPx: 390 }, 390, 70);
  assert.ok(t.length >= 4 && t.length <= 5);
  for (const k of t) assert.equal(k.hz % 500_000, 0);
  assert.deepEqual(
    t.map((k) => k.label),
    ["144.0", "144.5", "145.0", "145.5", "146.0"].filter((l) => t.some((k) => k.label === l)),
  );
  const fine = rulerTicks({ centerHz: 145_012_500, spanHz: 25_000, widthPx: 390 }, 390, 70);
  assert.ok(fine.some((k) => k.label === "145.010"));
});

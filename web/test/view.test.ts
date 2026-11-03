import { test } from "node:test";
import assert from "node:assert/strict";
import { clampView, hzToPx, panBy, pxToHz, rowTransform, ViewSync, zoomAt, type View } from "../src/view";

const near = (a: number, b: number, eps = 1e-6) => assert.equal(Math.abs(a - b) <= eps, true);
const v: View = { centerHz: 100_000_000, spanHz: 2_400_000, widthPx: 1200 };

test("pxToHz and hzToPx round trip", () => {
  for (const x of [0, 1, 299.5, 600, 1199, 1200]) near(hzToPx(v, pxToHz(v, x)), x);
  for (const hz of [98_800_000, 100_000_000, 100_123_456.5]) near(pxToHz(v, hzToPx(v, hz)), hz, 1e-4);
  assert.equal(pxToHz(v, 600), 100_000_000);
  assert.equal(pxToHz(v, 0), 98_800_000);
});

test("zoom keeps the anchor frequency under the finger", () => {
  const anchorX = 300;
  const anchorHz = pxToHz(v, anchorX);
  const z = zoomAt(v, 2.5, anchorHz);
  near(z.spanHz, v.spanHz / 2.5);
  near(hzToPx(z, anchorHz), anchorX, 1e-6);
  const back = zoomAt(z, 1 / 2.5, anchorHz);
  near(back.centerHz, v.centerHz, 1e-3);
});

test("zoom ignores a nonsense factor", () => {
  assert.deepEqual(zoomAt(v, 0, 1), v);
  assert.deepEqual(zoomAt(v, Number.NaN, 1), v);
});

test("pan moves the centre", () => {
  assert.equal(panBy(v, 1000).centerHz, 100_001_000);
});

test("clampView keeps the view inside the source band", () => {
  const band = { centerHz: 100_000_000, rateHz: 2_400_000 };
  assert.equal(clampView({ ...v, spanHz: 9e9 }, band).spanHz, 2_400_000);
  assert.equal(clampView({ ...v, spanHz: 9e9 }, band).centerHz, 100_000_000);
  assert.equal(clampView({ ...v, spanHz: 10 }, band).spanHz, 1000);
  const edge = clampView({ centerHz: 0, spanHz: 400_000, widthPx: 1200 }, band);
  assert.equal(edge.centerHz, 100_000_000 - 1_000_000);
  assert.equal(clampView({ ...v, widthPx: 20000 }, band).widthPx, 8192);
});

test("rowTransform maps a row cut for the current view straight across", () => {
  const t = rowTransform(v, v, 1200);
  near(t.a, 0);
  near(t.b, 1);
  near(t.limit, 1);
});

test("rowTransform places an older row by its own centre and span", () => {
  // The view moved up a quarter span after the row was cut, so the row shows
  // shifted left: screen s is row fraction s + 0.25.
  const row = { centerHz: v.centerHz, spanHz: v.spanHz, widthPx: 600 };
  const cur = panBy(v, v.spanHz / 4);
  const t = rowTransform(cur, row, 1200);
  // The texture is twice as wide as the row, so row fractions halve.
  near(t.limit, 0.5);
  near(t.a, 0.25 * 0.5);
  near(t.b, 0.5);
  // Zoomed in 2x around the centre: screen 0 is row fraction 0.25.
  const z = rowTransform(zoomAt(v, 2, v.centerHz), { ...v }, 1200);
  near(z.a, 0.25);
  near(z.b, 0.5);
});

test("ViewSync sends once per frame, only when changed, and again after forget", () => {
  const sent: View[] = [];
  const frames: (() => void)[] = [];
  const sync = new ViewSync((x) => sent.push(x), (fn) => frames.push(fn));
  sync.update(v);
  sync.update(panBy(v, 10));
  sync.update(panBy(v, 20));
  assert.equal(frames.length, 1);
  frames.shift()!();
  assert.equal(sent.length, 1);
  assert.equal(sent[0]!.centerHz, v.centerHz + 20);
  sync.update(panBy(v, 20));
  frames.shift()!();
  assert.equal(sent.length, 1);
  sync.forget();
  sync.update(panBy(v, 20));
  frames.shift()!();
  assert.equal(sent.length, 2);
});

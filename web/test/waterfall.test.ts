import { test } from "node:test";
import assert from "node:assert/strict";
import { colormap } from "../src/colormap";
import { RING_ROWS, RowRing } from "../src/waterfall";
import { panBy, type View } from "../src/view";

const v: View = { centerHz: 100_000_000, spanHz: 2_000_000, widthPx: 1000 };

test("the ring wraps and keeps each row's own view", () => {
  const r = new RowRing(2000);
  for (let i = 0; i < RING_ROWS + 3; i++) r.push(v.centerHz + i, v.spanHz, 1000);
  assert.equal(r.head, 2);
  assert.equal(r.center[2], v.centerHz + RING_ROWS + 2);
  assert.equal(r.width[0], 1000);
});

test("placing rows: a fresh slot follows the view, an empty slot draws nothing", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  r.place(v);
  assert.deepEqual([...r.meta.subarray(slot * 4, slot * 4 + 3)], [0, 0.5, 0.5]);
  assert.deepEqual([...r.meta.subarray(4, 7)], [0, 0, 0]);
  // After a pan of half a span, the same row starts half way across its width.
  r.place(panBy(v, v.spanHz / 2), slot);
  assert.equal(r.meta[slot * 4], 0.25);
});

test("rows wider than the texture are clipped to it", () => {
  const r = new RowRing(512);
  const slot = r.push(1, 1, 8192);
  assert.equal(r.width[slot], 512);
});

test("colour map runs from the background to yellow-white, opaque", () => {
  const c = colormap();
  assert.equal(c.length, 1024);
  assert.deepEqual([...c.subarray(0, 4)], [0x14, 0x14, 0x18, 255]);
  assert.deepEqual([...c.subarray(1020, 1024)], [0xff, 0xf0, 0xa0, 255]);
  for (let i = 3; i < 1024; i += 4) assert.equal(c[i], 255);
});

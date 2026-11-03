import { test } from "node:test";
import assert from "node:assert/strict";
import { MAX_LAG_ROWS, RowPacer, TRACE_EASE_MS, TraceEaser } from "../src/smooth";

const close = (a: number, b: number, eps = 1e-6) => assert.ok(Math.abs(a - b) <= eps, `${a} vs ${b}`);

// Rows every 27 ms, the engine's 36.6 rows/s at 2.4 MS/s.
const GAP = 1000 / 36.6;

function steady(p: RowPacer, rows: number, gap = GAP, from = 0): number {
  let t = from;
  for (let i = 0; i < rows; i++) {
    p.onRow(t);
    t += gap;
  }
  return t - gap;
}

test("the first row is drawn where it lands and nothing moves", () => {
  const p = new RowPacer();
  p.onRow(1000);
  assert.equal(p.phase(1000), 1);
  assert.equal(p.moving(1000), false);
});

test("between steady rows the scroll moves at one row per interval", () => {
  const p = new RowPacer();
  const last = steady(p, 40);
  close(p.interval, GAP, 0.01);
  close(p.phase(last), 0, 0.01);
  close(p.phase(last + GAP / 2), 0.5, 0.02);
  assert.equal(p.moving(last + GAP / 2), true);
});

test("a row arriving continues the scroll rather than jumping", () => {
  const p = new RowPacer();
  const last = steady(p, 40);
  // Early by a third: the new row starts a third of a row behind.
  const at = last + (GAP * 2) / 3;
  const before = p.phase(at);
  p.onRow(at);
  close(p.phase(at), before - 1);
  // On time: the scroll had reached the row and starts the next from zero.
  const p2 = new RowPacer();
  const last2 = steady(p2, 40);
  p2.onRow(last2 + GAP);
  close(p2.phase(last2 + GAP), 0, 0.02);
});

test("a late row holds the scroll still instead of overshooting", () => {
  const p = new RowPacer();
  const last = steady(p, 40);
  assert.equal(p.phase(last + GAP * 3), 1);
  assert.equal(p.moving(last + GAP * 3), false);
});

test("a lag never grows past its bound, and is caught up faster than real time", () => {
  const p = new RowPacer();
  let t = steady(p, 40);
  // A burst: five rows at once after a stall.
  for (let i = 0; i < 5; i++) p.onRow(t + GAP);
  t += GAP;
  assert.ok(p.phase(t) >= -MAX_LAG_ROWS);
  // From the deepest lag, back to rest in under the time those rows span.
  const caught = p.phase(t + GAP * (MAX_LAG_ROWS + 1));
  assert.equal(caught, 1);
});

test("the interval follows a new row rate and ignores a pause", () => {
  const p = new RowPacer();
  let t = steady(p, 40);
  // A stall of seconds is not the row rate.
  p.onRow(t + 5000);
  close(p.interval, GAP, 0.5);
  t = steady(p, 80, 1000 / 50, t + 5000 + 20);
  close(p.interval, 20, 0.5);
});

test("clearing forgets the rows", () => {
  const p = new RowPacer();
  const last = steady(p, 10);
  p.clear();
  assert.equal(p.phase(last + 1), 1);
  assert.equal(p.moving(last + 1), false);
});

test("the trace eases toward a new row and settles inside about 75 ms", () => {
  const e = new TraceEaser();
  e.setTarget(new Uint8Array([0, 100, 200]), 0);
  assert.deepEqual([...e.shown], [0, 100, 200]);
  assert.equal(e.moving(), false);
  e.setTarget(new Uint8Array([100, 100, 0]), 0);
  assert.equal(e.step(TRACE_EASE_MS), true);
  // One time constant: 63% of the way.
  close(e.shown[0]!, 100 * (1 - Math.exp(-1)), 1e-3);
  close(e.shown[2]!, 200 * Math.exp(-1), 1e-3);
  assert.equal(e.shown[1], 100);
  // Three time constants is 95%; within half a level it snaps and stops.
  e.step(TRACE_EASE_MS * 3);
  assert.ok(Math.abs(e.shown[0]! - 100) < 5);
  e.step(TRACE_EASE_MS * 8);
  assert.equal(e.shown[0], 100);
  assert.equal(e.shown[2], 0);
  assert.equal(e.moving(), false);
  assert.equal(e.step(TRACE_EASE_MS * 9), false);
});

test("the easing is the same however the frames fall", () => {
  const a = new TraceEaser();
  const b = new TraceEaser();
  for (const e of [a, b]) {
    e.setTarget(new Uint8Array([0]), 0);
    e.setTarget(new Uint8Array([255]), 0);
  }
  // 120 Hz against 60 Hz over the same 50 ms.
  for (let t = 8.333; t <= 50.0001; t += 8.333) a.step(t);
  for (let t = 16.666; t <= 50.0001; t += 16.666) b.step(t);
  close(a.shown[0]!, b.shown[0]!, 0.2);
});

test("a row of another width or view snaps rather than blending different frequencies", () => {
  const e = new TraceEaser();
  e.setTarget(new Uint8Array([0, 0]), 0, 1);
  e.setTarget(new Uint8Array([255, 255, 255]), 10, 1);
  assert.deepEqual([...e.shown], [255, 255, 255]);
  e.setTarget(new Uint8Array([0, 0, 0]), 20, 2);
  assert.deepEqual([...e.shown], [0, 0, 0]);
  assert.equal(e.moving(), false);
});

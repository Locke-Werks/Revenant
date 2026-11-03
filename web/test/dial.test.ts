import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DialPhysics,
  FAST_MULTIPLIER,
  FAST_PX_PER_MS,
  PX_PER_STEP,
  SETTLE_MS,
  fineStep,
  roundToStep,
  stepFor,
} from "../src/dial";

test("fine step per mode", () => {
  for (const m of ["cw", "usb", "lsb", "dsb"]) assert.equal(fineStep(m), 10, m);
  for (const m of ["am", "nfm", "raw"]) assert.equal(fineStep(m), 100, m);
  assert.equal(fineStep("wfm"), 1000);
});

test("fine step ignores case and falls back for an unknown mode", () => {
  assert.equal(fineStep("USB"), 10);
  assert.equal(fineStep("WFM"), 1000);
  assert.equal(fineStep("something"), 100);
});

test("slow drags use the fine step, fast ones 100 times it", () => {
  assert.equal(stepFor("cw", 0), 10);
  assert.equal(stepFor("cw", FAST_PX_PER_MS * 0.99), 10);
  assert.equal(stepFor("cw", FAST_PX_PER_MS), 10 * FAST_MULTIPLIER);
  assert.equal(stepFor("wfm", 3), 1000 * FAST_MULTIPLIER);
  // Direction is not speed.
  assert.equal(stepFor("am", -3), 100 * FAST_MULTIPLIER);
  assert.equal(stepFor("am", -0.1), 100);
});

test("roundToStep lands on the step", () => {
  assert.equal(roundToStep(99_123_456, 1000), 99_123_000);
  assert.equal(roundToStep(99_123_500, 1000), 99_124_000);
  assert.equal(roundToStep(7_074_049, 10), 7_074_050);
  assert.equal(roundToStep(162_550_000, 100), 162_550_000);
});

test("a slow drag moves one fine step per PX_PER_STEP, against the finger", () => {
  const p = new DialPhysics("cw");
  p.drag(0.5, 0);
  p.drag(PX_PER_STEP, 100);
  // Ribbon follows the finger, so the centre line reads a lower frequency.
  const hz = p.frame(100);
  assert.ok(Math.abs(hz + ((PX_PER_STEP + 0.5) / PX_PER_STEP) * 10) < 1e-9, String(hz));
  assert.equal(p.frame(101), 0);
});

test("a fast drag moves in the coarse step", () => {
  const p = new DialPhysics("cw");
  p.drag(1, 0);
  p.drag(100, 10);
  p.drag(100, 20);
  const hz = p.frame(20);
  // 201 px at 10 Hz fine would be about 251 Hz; coarse is well past that.
  assert.ok(Math.abs(hz) > 10_000, String(hz));
  assert.ok(hz < 0);
});

test("a tap produces no delta and no motion", () => {
  const p = new DialPhysics("am");
  p.release(50);
  assert.equal(p.frame(60), 0);
  assert.equal(p.moving, false);
});

function flick(p: DialPhysics, pxPerMs: number): number {
  let t = 0;
  for (let i = 0; i < 6; i++) {
    t += 16;
    p.drag(pxPerMs * 16, t);
  }
  p.frame(t);
  p.release(t);
  return t;
}

test("momentum carries on after release and decays to rest within 600 ms", () => {
  const p = new DialPhysics("nfm");
  const t0 = flick(p, 1.5);
  assert.equal(p.moving, true);
  let total = 0;
  let last = Number.POSITIVE_INFINITY;
  let monotone = true;
  for (let t = t0 + 16; t <= t0 + SETTLE_MS + 16; t += 16) {
    const d = p.frame(t);
    assert.ok(d <= 0, "keeps the direction of the flick");
    if (Math.abs(d) > last + 1e-9) monotone = false;
    last = Math.abs(d);
    total += d;
  }
  assert.ok(total < 0);
  assert.ok(monotone, "friction only slows it");
  assert.equal(p.moving, false);
  assert.equal(p.frame(t0 + 2000), 0);
});

test("one late frame still delivers the rest and settles", () => {
  const p = new DialPhysics("nfm");
  const t0 = flick(p, 1.5);
  const d = p.frame(t0 + 5000);
  assert.ok(d < 0);
  assert.equal(p.moving, false);
});

test("momentum keeps the step the flick was released in", () => {
  const fast = new DialPhysics("cw");
  const tf = flick(fast, 3);
  const slow = new DialPhysics("cw");
  const ts = flick(slow, 0.3);
  const df = Math.abs(fast.frame(tf + 600));
  const ds = Math.abs(slow.frame(ts + 600));
  assert.ok(df > ds * 100, `${df} vs ${ds}`);
});

test("a finger that rests before lifting leaves no momentum", () => {
  const p = new DialPhysics("am");
  let t = 0;
  for (let i = 0; i < 5; i++) {
    t += 16;
    p.drag(30, t);
  }
  p.frame(t);
  p.release(t + 300);
  assert.equal(p.moving, false);
  assert.equal(p.frame(t + 316), 0);
});

test("a very slow lift leaves no momentum", () => {
  const p = new DialPhysics("am");
  let t = 0;
  for (let i = 0; i < 5; i++) {
    t += 16;
    p.drag(0.2, t);
  }
  p.frame(t);
  p.release(t);
  assert.equal(p.moving, false);
});

test("stop halts momentum at once", () => {
  const p = new DialPhysics("nfm");
  const t0 = flick(p, 2);
  assert.ok(p.frame(t0 + 16) < 0);
  p.stop();
  assert.equal(p.moving, false);
  assert.equal(p.frame(t0 + 32), 0);
  assert.equal(p.frame(t0 + 500), 0);
});

test("stop drops a drag's undelivered delta", () => {
  const p = new DialPhysics("am");
  p.drag(0, 0);
  p.drag(40, 100);
  p.stop();
  assert.equal(p.frame(100), 0);
  assert.equal(p.moving, false);
});

test("moving is true while the finger is down", () => {
  const p = new DialPhysics("am");
  p.drag(5, 10);
  assert.equal(p.moving, true);
  p.frame(10);
  assert.equal(p.moving, true);
  p.release(500);
  assert.equal(p.moving, false);
});

test("a mode change applies to the next drag", () => {
  const p = new DialPhysics("cw");
  p.setMode("wfm");
  p.drag(0.1, 0);
  p.drag(PX_PER_STEP, 200);
  const hz = p.frame(200);
  assert.ok(Math.abs(hz) > 900 && Math.abs(hz) < 1100, String(hz));
});

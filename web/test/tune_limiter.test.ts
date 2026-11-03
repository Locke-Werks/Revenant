import { test } from "node:test";
import assert from "node:assert/strict";
import { TuneLimiter, TUNE_INTERVAL_MS } from "../src/tune_limiter";

function rig() {
  let t = 1000;
  const sent: number[] = [];
  let pending: { fn: () => void; at: number } | null = null;
  const lim = new TuneLimiter(
    (hz) => sent.push(hz),
    () => t,
    (fn, ms) => {
      const p = { fn, at: t + ms };
      pending = p;
      return () => {
        if (pending === p) pending = null;
      };
    },
  );
  const advance = (ms: number) => {
    t += ms;
    if (pending && pending.at <= t) {
      const p = pending;
      pending = null;
      p.fn();
    }
  };
  return { lim, sent, advance, hasTimer: () => pending !== null };
}

test("the first value goes out at once", () => {
  const { lim, sent } = rig();
  lim.set(100);
  assert.deepEqual(sent, [100]);
});

test("values inside the interval collapse to the latest, sent when it elapses", () => {
  const { lim, sent, advance } = rig();
  lim.set(100);
  lim.set(200);
  lim.set(300);
  assert.deepEqual(sent, [100]);
  advance(TUNE_INTERVAL_MS - 1);
  assert.deepEqual(sent, [100]);
  advance(1);
  assert.deepEqual(sent, [100, 300]);
});

test("a value after a quiet interval goes out at once", () => {
  const { lim, sent, advance } = rig();
  lim.set(100);
  advance(TUNE_INTERVAL_MS);
  lim.set(200);
  assert.deepEqual(sent, [100, 200]);
});

test("never more than one send per interval", () => {
  const { lim, sent, advance } = rig();
  for (let i = 0; i < 20; i++) {
    lim.set(i);
    advance(10);
  }
  advance(TUNE_INTERVAL_MS);
  // 200 ms of input at 50 ms spacing: the first plus one per interval after.
  assert.equal(sent.length <= 5, true);
  assert.equal(sent[sent.length - 1], 19);
});

test("flush sends the last unsent value and stops the timer", () => {
  const { lim, sent, hasTimer } = rig();
  lim.set(100);
  lim.set(200);
  lim.flush();
  assert.deepEqual(sent, [100, 200]);
  assert.equal(hasTimer(), false);
});

test("flush with nothing unsent sends nothing", () => {
  const { lim, sent } = rig();
  lim.set(100);
  lim.flush();
  assert.deepEqual(sent, [100]);
});

test("flush counts as a send for the next interval", () => {
  const { lim, sent, advance } = rig();
  lim.set(100);
  lim.set(200);
  lim.flush();
  lim.set(300);
  assert.deepEqual(sent, [100, 200]);
  advance(TUNE_INTERVAL_MS);
  assert.deepEqual(sent, [100, 200, 300]);
});

test("nothing is sent after cancel", () => {
  const { lim, sent, advance, hasTimer } = rig();
  lim.set(100);
  lim.set(200);
  lim.cancel();
  advance(TUNE_INTERVAL_MS * 2);
  lim.flush();
  assert.deepEqual(sent, [100]);
  assert.equal(hasTimer(), false);
});

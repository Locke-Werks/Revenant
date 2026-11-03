import { test } from "node:test";
import assert from "node:assert/strict";
import { digitStep, DigitSwipe, readoutDigits, SWIPE_STEP_PX } from "../src/ui/top_bar";

test("a digit steps by its place value", () => {
  assert.equal(digitStep(98_100_000, 5, 1), 98_200_000);
  assert.equal(digitStep(98_100_000, 5, -1), 98_000_000);
  assert.equal(digitStep(98_100_000, 2, 1), 98_100_100);
});

test("stepping carries into the next digit", () => {
  assert.equal(digitStep(98_900_000, 5, 1), 99_000_000);
  assert.equal(digitStep(99_900_000, 5, 1), 100_000_000);
});

test("stepping borrows from the next digit", () => {
  assert.equal(digitStep(99_000_000, 5, -1), 98_900_000);
  assert.equal(digitStep(100_000_000, 5, -1), 99_900_000);
});

test("stepping clamps at zero", () => {
  assert.equal(digitStep(50_000, 5, -1), 0);
  assert.equal(digitStep(0, 2, -1), 0);
  assert.equal(digitStep(0, 2, 1), 100);
});

test("the readout labels each digit with its place and leaves the rest inert", () => {
  const r = readoutDigits(98_100_000);
  assert.equal(r.map((d) => d.ch).join(""), "98.10000");
  assert.deepEqual(
    r.map((d) => d.index),
    [7, 6, null, 5, 4, 3, 2, 1],
  );
});

test("the readout reaches the 10 Hz place", () => {
  const r = readoutDigits(14_074_100);
  assert.equal(r.map((d) => d.ch).join(""), "14.07410");
  assert.equal(r[r.length - 2]!.index, 2);
  assert.equal(r[r.length - 1]!.index, 1);
  assert.equal(digitStep(14_074_100, 1, 1), 14_074_110);
});

function harness(hz: number | null) {
  const log: string[] = [];
  const swipe = new DigitSwipe({
    begin: () => {
      log.push("begin");
      return hz;
    },
    step: (v) => log.push(`step ${v}`),
    end: () => log.push("end"),
  });
  return { swipe, log };
}

test("a tap is not a swipe and touches nothing", () => {
  const { swipe, log } = harness(98_100_000);
  swipe.down(5, 100);
  swipe.move(100 - (SWIPE_STEP_PX - 1));
  assert.equal(swipe.up(), false);
  assert.deepEqual(log, []);
});

test("sixteen pixels up is one step up", () => {
  const { swipe, log } = harness(98_100_000);
  swipe.down(5, 200);
  swipe.move(200 - SWIPE_STEP_PX);
  assert.deepEqual(log, ["begin", "step 98200000"]);
  assert.equal(swipe.up(), true);
  assert.deepEqual(log, ["begin", "step 98200000", "end"]);
});

test("down steps decrease and steps are counted from the touch start", () => {
  const { swipe, log } = harness(98_100_000);
  swipe.down(5, 200);
  swipe.move(200 + 2 * SWIPE_STEP_PX + 3);
  swipe.move(200 + 3 * SWIPE_STEP_PX);
  swipe.move(200);
  assert.deepEqual(log, ["begin", "step 97900000", "step 97800000", "step 98100000"]);
});

test("a move inside the same step sends nothing more", () => {
  const { swipe, log } = harness(98_100_000);
  swipe.down(5, 200);
  swipe.move(200 - SWIPE_STEP_PX);
  swipe.move(200 - SWIPE_STEP_PX - 4);
  assert.deepEqual(log, ["begin", "step 98200000"]);
});

test("a swipe past zero clamps", () => {
  const { swipe, log } = harness(50_000);
  swipe.down(5, 200);
  swipe.move(200 + 4 * SWIPE_STEP_PX);
  assert.deepEqual(log, ["begin", "step 0"]);
});

test("with no receiver a swipe does nothing and is not a swipe", () => {
  const { swipe, log } = harness(null);
  swipe.down(5, 200);
  swipe.move(100);
  assert.equal(swipe.up(), false);
  assert.deepEqual(log, ["begin"]);
});

test("a cancelled swipe still flushes", () => {
  const { swipe, log } = harness(98_100_000);
  swipe.down(5, 200);
  swipe.move(200 - SWIPE_STEP_PX);
  swipe.up();
  swipe.up();
  assert.deepEqual(log, ["begin", "step 98200000", "end"]);
});

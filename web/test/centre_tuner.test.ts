import { test } from "node:test";
import assert from "node:assert/strict";
import { CentreTuner } from "../src/centre_tuner";
import { fineStep, stepFor, FAST_PX_PER_MS } from "../src/dial";

function rig(ranges: { lowHz: number; highHz: number }[] | null = null) {
  const sent: number[] = [];
  const answers: (() => void)[] = [];
  const shown: (number | null)[] = [];
  const t = new CentreTuner({
    send: (hz) => {
      sent.push(hz);
      return new Promise<void>((resolve) => answers.push(resolve));
    },
    ranges: () => ranges,
    preview: (hz) => void shown.push(hz),
  });
  return { t, sent, shown, answer: async () => {
    answers.shift()!();
    for (let i = 0; i < 5; i++) await Promise.resolve();
  } };
}

test("one setCenter in flight; the newest value goes when it returns", async () => {
  const r = rig();
  r.t.set(98_000_000);
  r.t.set(97_900_000);
  r.t.set(97_800_000);
  assert.deepEqual(r.sent, [98_000_000]);
  assert.equal(r.t.hz, 97_800_000);
  await r.answer();
  assert.deepEqual(r.sent, [98_000_000, 97_800_000]);
  await r.answer();
  assert.equal(r.t.idle, true);
});

test("the centre stays inside the tuner's range", () => {
  const r = rig([{ lowHz: 24_000_000, highHz: 1_766_000_000 }]);
  r.t.set(10_000_000);
  assert.deepEqual(r.sent, [24_000_000]);
});

test("the readout follows the preview until the source settles", async () => {
  const r = rig();
  r.t.set(99_000_000);
  r.t.settled();
  assert.deepEqual(r.shown, [99_000_000]);
  await r.answer();
  r.t.settled();
  assert.deepEqual(r.shown, [99_000_000, null]);
});

test("with no receiver the dial steps 10 kHz fine and 1 MHz fast", () => {
  assert.equal(fineStep("centre"), 10_000);
  assert.equal(stepFor("centre", FAST_PX_PER_MS), 1_000_000);
});

import { test } from "node:test";
import assert from "node:assert/strict";
import { ChipPress, LONG_PRESS_MS } from "../src/ui/chip_press";

function rig() {
  const log: string[] = [];
  let pending: (() => void) | null = null;
  const p = new ChipPress({
    longPress: () => log.push("long"),
    schedule: (fn) => {
      pending = fn;
      return () => {
        pending = null;
      };
    },
  });
  return { p, log, fire: () => pending?.(), get armed() { return pending !== null; } };
}

test("a hold past the threshold is a long press, and the click after it is swallowed", () => {
  const r = rig();
  r.p.down(10, 10);
  assert.ok(r.armed);
  r.fire();
  assert.deepEqual(r.log, ["long"]);
  r.p.up();
  assert.equal(r.p.click(), false, "the release must not also select the chip");
  // The next ordinary tap goes through.
  r.p.down(10, 10);
  r.p.up();
  assert.equal(r.p.click(), true);
});

test("a short tap is a click and arms nothing", () => {
  const r = rig();
  r.p.down(0, 0);
  r.p.up();
  assert.equal(r.armed, false);
  assert.equal(r.p.click(), true);
  assert.deepEqual(r.log, []);
});

test("moving the finger, as when scrolling the chips, cancels the press", () => {
  const r = rig();
  r.p.down(0, 0);
  r.p.move(3, 0);
  assert.ok(r.armed, "a thumb's jitter does not cancel");
  r.p.move(20, 0);
  assert.equal(r.armed, false);
  r.p.up();
  assert.deepEqual(r.log, []);
});

test("the threshold matches the waterfall's long press", () => {
  assert.ok(LONG_PRESS_MS >= 400 && LONG_PRESS_MS <= 700);
});

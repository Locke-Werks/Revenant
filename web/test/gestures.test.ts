import { test } from "node:test";
import assert from "node:assert/strict";
import { GestureClassifier, LONG_PRESS_MS } from "../src/gestures";
import { hzToPx, panBy, pxToHz, zoomAt, type View } from "../src/view";

// A sink that applies pan and zoom to a live view, the way main.ts does, so the
// tests check where frequencies end up on screen.
function rig() {
  let view: View = { centerHz: 1_000_000, spanHz: 400_000, widthPx: 400 };
  const log: string[] = [];
  const timers = new Map<number, () => void>();
  let seq = 0;
  const g = new GestureClassifier(
    {
      pan: (d) => {
        view = panBy(view, d);
        log.push("pan");
      },
      zoom: (f, a) => {
        view = zoomAt(view, f, a);
        log.push("zoom");
      },
      dragTune: (d) => log.push(`drag ${d}`),
      dragEnd: () => log.push("dragEnd"),
      tap: (hz) => log.push(`tap ${hz}`),
      longPress: (hz) => log.push(`long ${hz}`),
    },
    (x) => pxToHz(view, x),
    {
      setTimeout: (fn) => {
        timers.set(++seq, fn);
        return seq;
      },
      clearTimeout: (h) => void timers.delete(h as number),
    },
  );
  const fire = () => {
    const fns = [...timers.values()];
    timers.clear();
    for (const f of fns) f();
  };
  return { g, log, fire, view: () => view, pending: () => timers.size };
}

test("a tap tunes at the frequency under the finger", () => {
  const { g, log } = rig();
  g.down(1, 100, 50);
  g.move(1, 103, 52);
  g.up(1);
  assert.deepEqual(log, ["tap 900000"]);
});

test("one finger drags the receiver, never the view, and does not tap", () => {
  const { g, log, view } = rig();
  g.down(1, 100, 50);
  g.move(1, 150, 50);
  g.move(1, 200, 50);
  g.up(1);
  // 1 px is 1000 Hz here; the first move past the slop carries the whole 50 px.
  assert.deepEqual(log, ["drag 50000", "drag 50000", "dragEnd"]);
  assert.equal(view().centerHz, 1_000_000);
});

test("movement under the slop is not a drag", () => {
  const { g, log } = rig();
  g.down(1, 100, 50);
  g.move(1, 104, 52);
  g.up(1);
  assert.deepEqual(log, ["tap 900000"]);
});

test("two fingers moving together pan the view and do not drag", () => {
  const { g, log, view } = rig();
  g.down(1, 100, 50);
  g.down(2, 200, 50);
  const anchorHz = pxToHz(view(), 150);
  g.move(1, 150, 50);
  g.move(2, 250, 50);
  g.up(1);
  g.up(2);
  assert.equal(log.includes("pan"), true);
  assert.equal(log.some((l) => l.startsWith("drag")), false);
  assert.equal(Math.abs(hzToPx(view(), anchorHz) - 200) < 1e-9, true);
});

test("a finger left after a pinch does nothing until all lift", () => {
  const { g, log } = rig();
  g.down(1, 100, 50);
  g.down(2, 200, 50);
  g.up(2);
  g.move(1, 300, 50);
  g.up(1);
  assert.equal(log.some((l) => l.startsWith("drag") || l.startsWith("tap")), false);
});

test("a second finger landing mid-drag ends the drag", () => {
  const { g, log } = rig();
  g.down(1, 100, 50);
  g.move(1, 150, 50);
  g.down(2, 300, 50);
  g.up(2);
  g.up(1);
  assert.deepEqual(log, ["drag 50000", "dragEnd"]);
});

test("jitter after a long press never drags", () => {
  const { g, log, fire } = rig();
  g.down(1, 300, 50);
  fire();
  g.move(1, 301, 50);
  g.move(1, 320, 50);
  g.up(1);
  assert.deepEqual(log, ["long 1100000"]);
});

test("a cancelled drag still ends", () => {
  const { g, log } = rig();
  g.down(1, 100, 50);
  g.move(1, 150, 50);
  g.cancel(1);
  assert.deepEqual(log, ["drag 50000", "dragEnd"]);
});

test("long press fires once and suppresses the tap", () => {
  const { g, log, fire } = rig();
  g.down(1, 300, 50);
  fire();
  g.up(1);
  assert.deepEqual(log, ["long 1100000"]);
  assert.equal(LONG_PRESS_MS, 500);
});

test("moving cancels the long press", () => {
  const { g, log, pending } = rig();
  g.down(1, 300, 50);
  g.move(1, 340, 50);
  assert.equal(pending(), 0);
  g.up(1);
  assert.equal(log.some((l) => l.startsWith("long") || l.startsWith("tap")), false);
});

test("pinch zooms around the frequency under the fingers and does not tap", () => {
  const { g, log, view } = rig();
  g.down(1, 150, 50);
  g.down(2, 250, 50);
  const anchorHz = pxToHz(view(), 200);
  g.move(2, 350, 50); // spread 100 -> 200, midpoint 200 -> 250
  g.move(1, 50, 50); // spread 200 -> 300, midpoint 250 -> 200
  assert.equal(Math.abs(view().spanHz - 400_000 / 3) < 1e-6, true);
  assert.equal(Math.abs(hzToPx(view(), anchorHz) - 200) < 1e-6, true);
  g.up(1);
  g.up(2);
  assert.equal(log.includes("zoom"), true);
  assert.equal(log.some((l) => l.startsWith("tap") || l.startsWith("long")), false);
});

test("two quick fingers without movement are not a tap", () => {
  const { g, log, fire } = rig();
  g.down(1, 100, 50);
  g.down(2, 200, 50);
  fire();
  g.up(2);
  g.up(1);
  assert.deepEqual(log, []);
});

test("a cancelled pointer does not tap, and the next touch taps normally", () => {
  const { g, log } = rig();
  g.down(1, 100, 50);
  g.cancel(1);
  assert.deepEqual(log, []);
  g.down(2, 200, 50);
  g.up(2);
  assert.deepEqual(log, ["tap 1000000"]);
});

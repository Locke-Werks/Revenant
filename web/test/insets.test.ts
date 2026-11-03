import { test } from "node:test";
import assert from "node:assert/strict";
import { appGeometry, screenTallPx } from "../src/insets";

test("a viewport overshooting the glass by both insets puts the app a top inset down, screen tall", () => {
  // Measured on the owner's phone with ?insets: insets 62/34, innerHeight 1052.
  assert.deepEqual(appGeometry({ innerHeight: 1052, screenTall: 956, insetTop: 62, insetBottom: 34 }), { topPx: 62, heightPx: 956 });
});

test("an honest viewport is used as it is", () => {
  assert.deepEqual(appGeometry({ innerHeight: 956, screenTall: 956, insetTop: 62, insetBottom: 34 }), { topPx: 0, heightPx: 956 });
  assert.deepEqual(appGeometry({ innerHeight: 700, screenTall: 956, insetTop: 0, insetBottom: 0 }), { topPx: 0, heightPx: 700 });
});

test("an overshoot the insets do not explain is not guessed at", () => {
  assert.deepEqual(appGeometry({ innerHeight: 1000, screenTall: 956, insetTop: 62, insetBottom: 34 }), { topPx: 0, heightPx: 1000 });
});

test("the screen's length follows the orientation", () => {
  assert.equal(screenTallPx(440, 956, false), 956);
  assert.equal(screenTallPx(440, 956, true), 440);
});

import { test } from "node:test";
import assert from "node:assert/strict";
import { appHeightPx } from "../src/insets";

test("an installed iPhone app that reports a viewport taller than the screen fills the screen", () => {
  // Measured on the owner's phone with ?insets: insets 62/34, innerHeight 1052.
  const h = appHeightPx({ innerHeight: 1052, visualHeight: 1052, screenWidth: 440, screenHeight: 956, landscape: false });
  assert.equal(h, 956);
});

test("an honest viewport is used as it is", () => {
  assert.equal(appHeightPx({ innerHeight: 700, visualHeight: 700, screenWidth: 440, screenHeight: 956, landscape: false }), 700);
});

test("landscape takes the screen's short side as the ceiling", () => {
  assert.equal(appHeightPx({ innerHeight: 520, visualHeight: null, screenWidth: 440, screenHeight: 956, landscape: true }), 440);
});

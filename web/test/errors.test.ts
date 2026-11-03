import { test } from "node:test";
import assert from "node:assert/strict";
import { REPEAT_MS, shortError, ToastGate } from "../src/ui/errors";

test("the band-edge refusal becomes one short sentence", () => {
  const raw =
    "place: centre -1515153 Hz is outside the source's baseband span of +/-1200000 Hz. This frequency is relative " +
    "to the source's tuned centre, not absolute;\nsubtract EngineInfo::source_center from an absolute frequency";
  assert.equal(shortError(raw), "Outside the radio's range");
});

test("an unknown long refusal keeps its first sentence only", () => {
  const raw = "the engine would not do this thing for a reason. And here is a great deal more detail that nobody on a phone needs to read";
  assert.equal(shortError(raw), "the engine would not do this thing for a reason.");
});

test("a short message passes unchanged, first line only", () => {
  assert.equal(shortError("mode: \"xyz\" is not a mode"), "mode: \"xyz\" is not a mode");
  assert.equal(shortError("first\nsecond"), "first");
});

test("the same sentence inside the repeat window is not shown again", () => {
  const g = new ToastGate();
  assert.equal(g.admit("a", 0), true);
  assert.equal(g.admit("a", REPEAT_MS - 1), false);
  assert.equal(g.admit("b", 10), true);
  assert.equal(g.admit("a", REPEAT_MS + 1), true);
});

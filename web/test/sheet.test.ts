import { test } from "node:test";
import assert from "node:assert/strict";
import { readTab, SHEET_TABS } from "../src/ui/sheet";
import { layoutPreview } from "../src/install";

test("the bottom pane reopens on the tab it was left on", () => {
  for (const t of SHEET_TABS) assert.equal(readTab(t), t);
});

test("with nothing stored, or something stale, the bottom pane opens on Tune", () => {
  assert.equal(readTab(null), "tune");
  assert.equal(readTab(""), "tune");
  assert.equal(readTab("feed"), "tune");
  assert.equal(readTab("Signals"), "tune");
});

test("the layout preview needs both dev=1 and layout=1", () => {
  assert.equal(layoutPreview("?dev=1&layout=1"), true);
  assert.equal(layoutPreview("?layout=1"), false);
  assert.equal(layoutPreview("?dev=1"), false);
  assert.equal(layoutPreview(""), false);
});

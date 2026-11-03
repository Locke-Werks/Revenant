import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { rulerMarks } from "../src/insets";

test("ruler lines fall every 100 px below the height, none at 0", () => {
  assert.deepEqual(rulerMarks(450), [100, 200, 300, 400]);
  assert.deepEqual(rulerMarks(400), [100, 200, 300]);
});

// The screen's height is in points while the layout is in CSS px, and the two
// part ways as soon as iOS zooms the page; sizing from it left a dead band.
// Only the debug overlay may read it, to print it.
test("layout code does not size anything from the screen's height", () => {
  const files = [
    ...readdirSync("src").filter((f) => f.endsWith(".ts")).map((f) => `src/${f}`),
    ...readdirSync("src/ui").filter((f) => f.endsWith(".ts")).map((f) => `src/ui/${f}`),
  ];
  for (const f of files) {
    let text = readFileSync(f, "utf8");
    if (f === "src/insets.ts") text = text.replace(/export function showInsets[\s\S]*?\n}\n/, "");
    assert.equal(/screen\.(height|availHeight)/.test(text), false, f);
  }
  const css = readFileSync("static/app.css", "utf8");
  assert.equal(/--app-h|--app-top/.test(css), false);
});

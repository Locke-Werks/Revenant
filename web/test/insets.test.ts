import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { needsRepin, previousNormal, shortViewportFix, rulerMarks, widest, type LaunchCapture } from "../src/insets";

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
    // The overlay prints the screen; the re-pin reads its width along the
    // orientation, never to size anything.
    if (f === "src/insets.ts") {
      text = text.replace(/export function showInsets[\s\S]*?\n}\n/, "").replace(/export function pinViewportWidth[\s\S]*?\n}\n/, "")
        // Compares the viewport with the screen to detect the short case; the
        // size it applies is the inset, not the screen.
        .replace(/export function shortViewportFix[\s\S]*?\n}\n/, "")
        .replace(/export function trackShortViewport[\s\S]*?\n}\n/, "");
    }
    assert.equal(/screen\.(height|availHeight)/.test(text), false, f);
  }
  const css = readFileSync("static/app.css", "utf8");
  assert.equal(/--app-h|--app-top/.test(css), false);
});

test("over-wide elements are listed widest first, at most eight, narrower ones left out", () => {
  const items = [{ label: "a", width: 441 }, { label: "b", width: 440 }, { label: "c", width: 600 }];
  assert.deepEqual(widest(items, 440), ["c 600.0", "a 441.0"]);
  const many = Array.from({ length: 12 }, (_, i) => ({ label: String(i), width: 500 + i }));
  assert.equal(widest(many, 440).length, 8);
});

test("no canvas is given a pixel width taken from the page", () => {
  for (const f of ["src/ui/dial_strip.ts", "src/app.ts"]) {
    assert.equal(readFileSync(f, "utf8").includes("style.width = `${"), false, f);
  }
});

test("a zoomed-out page or a root wider than the device asks for a re-pin", () => {
  const g = { scale: 1, clientWidth: 440, screenWidth: 440, screenHeight: 956, landscape: false };
  assert.equal(needsRepin(g), false);
  assert.equal(needsRepin({ ...g, scale: 0.85 }), true);
  assert.equal(needsRepin({ ...g, clientWidth: 518, scale: null }), true);
  assert.equal(needsRepin({ ...g, clientWidth: 956, landscape: true }), false);
});

test("the previous normal launch is the newest without the overlay from before this load", () => {
  const c = (t: number, overlay: boolean): LaunchCapture => ({ t, overlay, maxScrollWidth: 484, at: 300, items: [], scale: 0.91, innerWidth: 484 });
  assert.equal(previousNormal([c(1, false), c(2, false), c(3, true)], 10)!.t, 2);
  assert.equal(previousNormal([c(1, false), c(20, false)], 10)!.t, 1);
  assert.equal(previousNormal([c(3, true)], 10), null);
});

test("an installed app one status bar short is extended by the top inset, and only then", () => {
  const g = { standalone: true, insetTop: 62, screenWidth: 440, screenHeight: 956, landscape: false };
  assert.equal(shortViewportFix({ ...g, innerHeight: 894 }), 62);
  assert.equal(shortViewportFix({ ...g, innerHeight: 956 }), 0);
  assert.equal(shortViewportFix({ ...g, innerHeight: 1125 }), 0);
  assert.equal(shortViewportFix({ ...g, innerHeight: 894, standalone: false }), 0);
  // Landscape: the screen's short side, with a landscape top inset.
  const l = { standalone: true, screenWidth: 440, screenHeight: 956, landscape: true };
  assert.equal(shortViewportFix({ ...l, insetTop: 20, innerHeight: 420 }), 20);
  assert.equal(shortViewportFix({ ...l, insetTop: 0, innerHeight: 440 }), 0);
  assert.equal(shortViewportFix({ ...l, insetTop: 20, innerHeight: 440 }), 0);
});

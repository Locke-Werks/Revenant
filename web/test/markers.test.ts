import { test } from "node:test";
import assert from "node:assert/strict";
import { detectionBoxes, parseHexColor, passbandHz, passbandPx } from "../src/markers";
import type { View } from "../src/view";

const v: View = { centerHz: 100_000_000, spanHz: 1_000_000, widthPx: 500 };

test("passbands straddle the carrier except for sidebands", () => {
  assert.deepEqual(passbandHz("nfm", 1_000_000, 12_000), { lowHz: 994_000, highHz: 1_006_000 });
  assert.deepEqual(passbandHz("usb", 1_000_000, 3_000), { lowHz: 1_000_000, highHz: 1_003_000 });
  assert.deepEqual(passbandHz("lsb", 1_000_000, 3_000), { lowHz: 997_000, highHz: 1_000_000 });
  // No bandwidth from the gateway yet: the mode's usual width.
  assert.deepEqual(passbandHz("wfm", 1_000_000, null), { lowHz: 900_000, highHz: 1_100_000 });
});

test("default passbands match the desktop's per-mode defaults", () => {
  const f = 1_000_000;
  const at = (m: string) => {
    const p = passbandHz(m, f, null);
    return [p.lowHz - f, p.highHz - f];
  };
  assert.deepEqual(at("am"), [-5_000, 5_000]);
  assert.deepEqual(at("nfm"), [-8_000, 8_000]);
  assert.deepEqual(at("wfm"), [-100_000, 100_000]);
  assert.deepEqual(at("usb"), [300, 2_700]);
  assert.deepEqual(at("lsb"), [-2_700, -300]);
  assert.deepEqual(at("dsb"), [-3_000, 3_000]);
  assert.deepEqual(at("cw"), [-250, 250]);
  assert.deepEqual(at("raw"), [-6_000, 6_000]);
});

test("passband pixels follow the view and the canvas width", () => {
  const p = passbandPx(v, 1000, { hz: v.centerHz + 100_000, mode: "nfm", bandwidthHz: 20_000 })!;
  assert.deepEqual(p, { x0: 590, x1: 610, xc: 600 });
  const usb = passbandPx(v, 1000, { hz: v.centerHz, mode: "usb", bandwidthHz: 3_000 })!;
  assert.deepEqual(usb, { x0: 500, x1: 503, xc: 500 });
});

test("a passband narrower than a pixel stays one pixel wide; off screen is null", () => {
  const p = passbandPx(v, 100, { hz: v.centerHz, mode: "cw", bandwidthHz: 500 })!;
  assert.equal(p.x1 - p.x0, 1);
  assert.equal(p.xc, 50);
  assert.equal(passbandPx(v, 100, { hz: v.centerHz + 2_000_000, mode: "nfm", bandwidthHz: 12_500 }), null);
});

test("accent colours parse from hex, anything else is the default", () => {
  assert.deepEqual(parseHexColor(" #0a0B0c"), [10, 11, 12]);
  assert.deepEqual(parseHexColor("#fff"), [255, 255, 255]);
  assert.deepEqual(parseHexColor("rgb(1,2,3)"), [0xd6, 0x26, 0x2a]);
});

test("detection boxes: alpha rises with confidence, held draws dimmer", () => {
  const d = (confidence: number, state: "live" | "held") => ({
    id: "1", centerHz: 1000, bandwidthHz: 200, snrDb: 10, confidence, state, label: "AM", labelConfidence: 1, lastSeen: 0,
  });
  const [lo, hi, held] = detectionBoxes([d(0.1, "live"), d(0.9, "live"), d(0.9, "held")]);
  assert.equal(lo!.lowHz, 900);
  assert.equal(lo!.highHz, 1100);
  assert.ok(hi!.alpha > lo!.alpha);
  assert.ok(held!.alpha < hi!.alpha);
  assert.equal(held!.held, true);
});

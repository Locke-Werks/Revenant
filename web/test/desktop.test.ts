import { test } from "node:test";
import assert from "node:assert/strict";
import { desktopOpenUri, parseDesktop, withQueryValue, type DesktopRadio } from "../src/desktop";
import { openUriFollowingDesktop } from "../src/ui/radio_panel";
import type { GatewayEvent } from "../src/protocol";

const LAST =
  "rtlsdr://0?agc=1&bw=3000000&direct=auto&freq=903536000&rate=3200000&gain=auto";

function ev(fields: Record<string, unknown>): GatewayEvent {
  return { event: "desktop", ...fields } as unknown as GatewayEvent;
}

const desktopRadio: DesktopRadio = {
  uri: LAST,
  backend: "rtlsdr",
  device: "0",
  params: [
    ["agc", "1"],
    ["bw", "3000000"],
    ["direct", "auto"],
    ["freq", "903536000"],
    ["rate", "3200000"],
    ["gain", "auto"],
  ],
};

const rtl = {
  uri: "rtlsdr://0",
  backend: "rtlsdr",
  serial: "00000001",
  tuneRanges: [{ lowHz: 24_000_000, highHz: 1_766_000_000 }],
  sampleRates: [],
  minRate: 225_001,
  maxRate: 3_200_000,
};

test("the desktop event is read into a radio and a bar", () => {
  const d = parseDesktop(
    ev({
      confidenceBar: 0.91,
      radio: {
        uri: LAST,
        backend: "rtlsdr",
        device: "0",
        params: { agc: "1", freq: "903536000" },
      },
    }),
  );
  assert.equal(d.confidenceBar, 0.91);
  assert.deepEqual(d.radio?.params, [
    ["agc", "1"],
    ["freq", "903536000"],
  ]);
  assert.equal(d.radio?.device, "0");
});

test("a desktop that saved nothing, or a malformed event, reads as nothing", () => {
  assert.deepEqual(parseDesktop(ev({})), { radio: null, confidenceBar: null });
  const bad = parseDesktop(ev({ confidenceBar: "high", radio: { uri: 3 } }));
  assert.equal(bad.radio, null);
  assert.equal(bad.confidenceBar, null);
});

test("withQueryValue keeps every other key in place", () => {
  assert.equal(withQueryValue("rtlsdr://0?a=1&freq=2&b=3", "freq", "9"), "rtlsdr://0?a=1&b=3&freq=9");
  assert.equal(withQueryValue("rtlsdr://0?a=1&freq=2", "freq", null), "rtlsdr://0?a=1");
  assert.equal(withQueryValue("rtlsdr://0", "rate", "5"), "rtlsdr://0?rate=5");
});

test("the same device opens with the desktop's front end and the phone's centre", () => {
  const uri = openUriFollowingDesktop(rtl, desktopRadio, 162_550_000);
  assert.equal(
    uri,
    "rtlsdr://0?agc=1&bw=3000000&direct=auto&freq=162550000&rate=3200000&gain=auto",
  );
});

test("a phone centre the tuner cannot reach keeps the desktop's freq", () => {
  const uri = openUriFollowingDesktop(rtl, desktopRadio, 7_100_000);
  assert.equal(uri, LAST);
});

test("no phone centre yet keeps the desktop's freq", () => {
  assert.equal(openUriFollowingDesktop(rtl, desktopRadio, null), LAST);
});

test("the device may match by serial", () => {
  const bySerial: DesktopRadio = { ...desktopRadio, device: "00000001" };
  const uri = openUriFollowingDesktop(rtl, bySerial, null);
  assert.ok(uri.includes("rate=3200000"));
  assert.ok(uri.includes("gain=auto"));
});

test("another device falls back to the phone's own composition", () => {
  const other = { ...rtl, uri: "rtlsdr://1", serial: "00000002" };
  assert.equal(
    openUriFollowingDesktop(other, desktopRadio, null),
    "rtlsdr://1?rate=2400000&freq=98100000",
  );
  const airspy = { ...rtl, uri: "airspy://0", backend: "airspy" };
  assert.equal(desktopOpenUri(airspy, desktopRadio, null, () => 0), null);
  assert.equal(
    openUriFollowingDesktop(rtl, null, null),
    "rtlsdr://0?rate=2400000&freq=98100000",
  );
});

test("a desktop URI without freq gets the phone's default on a tunable radio", () => {
  const noFreq: DesktopRadio = {
    ...desktopRadio,
    params: [
      ["rate", "3200000"],
      ["gain", "20"],
    ],
  };
  assert.equal(
    openUriFollowingDesktop(rtl, noFreq, null),
    "rtlsdr://0?rate=3200000&gain=20&freq=98100000",
  );
});

test("a radio with no tune range is never given freq", () => {
  const file = { uri: "rtlsdr://0", backend: "rtlsdr", tuneRanges: [], sampleRates: [], minRate: 0, maxRate: 0 };
  const uri = openUriFollowingDesktop(file, desktopRadio, 100_000_000);
  assert.ok(!uri.includes("freq="));
  assert.ok(uri.includes("rate=3200000"));
});

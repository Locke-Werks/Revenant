import { test } from "node:test";
import assert from "node:assert/strict";
import {
  decodersFor,
  encryptedFlag,
  modeLabel,
  parseCatalogue,
  shortRefusal,
  toggled,
  voiceStatus,
  type DecoderInfo,
} from "../src/decoders";
import { labelAt, labelText, type Detection } from "../src/detections";
import type { GatewayEvent } from "../src/protocol";
import { MODES } from "../src/ui/receivers";

const dec = (name: string, modes: string[]): DecoderInfo => ({ name, description: "", input: "realAudio", modes });

const CATALOGUE = [
  dec("pocsag", ["nfm"]),
  dec("ax25", ["nfm"]),
  dec("rtty", ["usb", "lsb"]),
  dec("p25p1", ["p25p1", "raw"]),
  dec("m17", ["p25p1", "raw"]),
  dec("rds", ["wfm"]),
];

test("the mode list offers the four digital voice modes after the analog ones", () => {
  for (const m of ["p25p1", "dstar", "tetra", "dmr"]) assert.ok((MODES as readonly string[]).includes(m), m);
  assert.ok(MODES.indexOf("p25p1") > MODES.indexOf("raw"));
  assert.equal(MODES[0], "nfm");
  assert.deepEqual(
    ["p25p1", "dstar", "tetra", "dmr", "nfm"].map(modeLabel),
    ["P25", "D-STAR", "TETRA", "DMR", "NFM"],
  );
});

test("decodersFor offers only the decoders that read the receiver's mode", () => {
  assert.deepEqual(decodersFor("nfm", CATALOGUE).map((d) => d.name), ["ax25", "pocsag"]);
  assert.deepEqual(decodersFor("usb", CATALOGUE).map((d) => d.name), ["rtty"]);
  assert.deepEqual(decodersFor("p25p1", CATALOGUE).map((d) => d.name), ["m17", "p25p1"]);
  assert.deepEqual(decodersFor("am", CATALOGUE), []);
  // An engine that does not say which modes a decoder reads leaves it to the gateway.
  assert.deepEqual(decodersFor("am", [dec("old", [])]).map((d) => d.name), ["old"]);
});

test("toggled adds a name that is off and removes one that is on", () => {
  assert.deepEqual(toggled([], "pocsag"), ["pocsag"]);
  assert.deepEqual(toggled(["pocsag", "ax25"], "pocsag"), ["ax25"]);
});

test("parseCatalogue keeps good entries and reads an absent voice report as unknown", () => {
  const c = parseCatalogue({
    decoders: [{ name: "pocsag", description: "POCSAG", input: "realAudio", modes: ["nfm"] }, { name: "" }, 3],
    voiceModes: ["p25p1", "dmr"],
  });
  assert.deepEqual(c.decoders.map((d) => d.name), ["pocsag"]);
  assert.deepEqual(c.voiceModes, ["p25p1", "dmr"]);
  assert.equal(parseCatalogue({ decoders: [], voiceModes: null }).voiceModes, null);
  assert.deepEqual(parseCatalogue(null).decoders, []);
});

test("a digital voice receiver shows encrypted, or no vocoder when its voice cannot play", () => {
  assert.equal(voiceStatus("dmr", ["p25p1", "dmr"], true), "encrypted");
  assert.equal(voiceStatus("dmr", ["p25p1", "dmr"], false), null);
  assert.equal(voiceStatus("dstar", ["p25p1"], false), "no vocoder");
  assert.equal(voiceStatus("tetra", ["p25p1", "dmr", "dstar"], false), "no vocoder");
  assert.equal(voiceStatus("dstar", null, false), null);
  assert.equal(voiceStatus("nfm", ["p25p1"], true), null);
});

test("encryptedFlag reads P25's encrypted and DMR's privacy flags", () => {
  const ev = (fields: unknown) => ({ event: "decoded", vrx: 1, fields }) as unknown as GatewayEvent;
  assert.equal(encryptedFlag(ev({ encrypted: true })), true);
  assert.equal(encryptedFlag(ev({ privacy: true })), true);
  assert.equal(encryptedFlag(ev({ encrypted: false, talkgroup: 5 })), false);
  assert.equal(encryptedFlag(ev({ talkgroup: 5 })), undefined);
  assert.equal(encryptedFlag(ev(undefined)), undefined);
});

test("shortRefusal keeps the first clause of the engine's sentence", () => {
  assert.equal(shortRefusal("names: p25p1 does not read a nfm receiver (it reads p25p1, raw)"), "p25p1 does not read a nfm receiver (it reads p25p1, raw)");
  assert.equal(shortRefusal("the rate is wrong. Try another; really"), "the rate is wrong");
  assert.equal(shortRefusal("x".repeat(200), 10).length, 10);
});

const det = (over: Partial<Detection>): Detection => ({
  id: "1",
  centerHz: 100_000_000,
  bandwidthHz: 20_000,
  snrDb: 10,
  confidence: 0.9,
  state: "live",
  label: "NFM",
  labelKind: "modulation",
  labelConfidence: 0.8,
  lastSeen: 0,
  ...over,
});

test("labelText shows the label with its confidence, and ? with none", () => {
  assert.equal(labelText(det({})), "NFM 80%");
  assert.equal(labelText(det({ label: "p25", labelConfidence: 0 })), "P25");
  assert.equal(labelText(det({ label: "" })), "?");
});

test("labelAt finds the narrowest labelled detection under a receiver", () => {
  const wide = det({ id: "1", bandwidthHz: 200_000, label: "WFM" });
  const narrow = det({ id: "2", bandwidthHz: 12_500, label: "P25", labelKind: "protocol" });
  const blank = det({ id: "3", bandwidthHz: 5_000, label: "" });
  assert.equal(labelAt(100_000_000, [wide, narrow, blank])?.id, "2");
  assert.equal(labelAt(100_050_000, [wide, narrow])?.id, "1");
  assert.equal(labelAt(101_000_000, [wide, narrow]), null);
});

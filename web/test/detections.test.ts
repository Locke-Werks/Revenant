import { test } from "node:test";
import assert from "node:assert/strict";
import { fitBandwidth, parseDetections, snapTap, sortForList, type Detection } from "../src/detections";
import type { GatewayEvent } from "../src/protocol";

const item = (over: Record<string, unknown> = {}) => ({
  id: "1",
  centerHz: 100_000_000,
  bandwidthHz: 200_000,
  snrDb: 20,
  confidence: 0.9,
  state: "live",
  label: "WFM",
  labelConfidence: 0.8,
  lastSeen: 5,
  ...over,
});

const ev = (items: unknown[]) => ({ event: "detections", items }) as unknown as GatewayEvent;

const det = (over: Partial<Detection> = {}): Detection => ({
  id: "1",
  centerHz: 100_000_000,
  bandwidthHz: 200_000,
  snrDb: 20,
  confidence: 0.9,
  state: "live",
  label: "",
  labelConfidence: 0,
  lastSeen: 0,
  ...over,
});

test("parseDetections reads every field and keeps the id a string", () => {
  const big = "18446744073709551615";
  const [d] = parseDetections(ev([item({ id: big, state: "held" })])) as [Detection];
  assert.equal(d.id, big);
  assert.equal(d.state, "held");
  assert.equal(d.centerHz, 100_000_000);
  assert.equal(d.label, "WFM");
});

test("parseDetections skips malformed items and keeps the rest", () => {
  const out = parseDetections(
    ev([
      item({ id: "a" }),
      item({ id: "b", centerHz: null }),
      item({ id: "c", snrDb: "x" }),
      item({ id: "d", confidence: Infinity }),
      item({ id: "e", state: "gone" }),
      item({ id: 7 }),
      null,
      "junk",
      item({ id: "f" }),
    ]),
  );
  assert.deepEqual(out.map((d) => d.id), ["a", "f"]);
});

test("parseDetections returns nothing when items is not an array", () => {
  assert.deepEqual(parseDetections({ event: "detections" } as unknown as GatewayEvent), []);
  assert.deepEqual(parseDetections({ event: "detections", items: 3 } as unknown as GatewayEvent), []);
});

test("sortForList orders by SNR descending, then id, without mutating", () => {
  const input = [
    det({ id: "9", snrDb: 10 }),
    det({ id: "2", snrDb: 30 }),
    det({ id: "1", snrDb: 10 }),
    det({ id: "10", snrDb: 10 }),
  ];
  const out = sortForList(input);
  assert.deepEqual(out.map((d) => d.id), ["2", "1", "9", "10"]);
  assert.equal(input[0]?.id, "9");
});

test("sortForList compares ids numerically past 2^53", () => {
  const out = sortForList([
    det({ id: "18446744073709551615" }),
    det({ id: "18446744073709551614" }),
  ]);
  assert.deepEqual(out.map((d) => d.id), ["18446744073709551614", "18446744073709551615"]);
});

test("snapTap snaps to the centre of the band holding the tap", () => {
  const d = [det({ centerHz: 100_000_000, bandwidthHz: 200_000 })];
  assert.equal(snapTap(100_050_000, d), 100_000_000);
});

test("snapTap returns the tap when no band holds it", () => {
  const d = [det({ centerHz: 100_000_000, bandwidthHz: 200_000 })];
  assert.equal(snapTap(101_000_000, d), 101_000_000);
  assert.equal(snapTap(5, []), 5);
});

test("snapTap picks the narrowest of overlapping bands", () => {
  const d = [
    det({ id: "1", centerHz: 100_000_000, bandwidthHz: 400_000 }),
    det({ id: "2", centerHz: 100_100_000, bandwidthHz: 25_000 }),
  ];
  assert.equal(snapTap(100_105_000, d), 100_100_000);
  assert.equal(snapTap(99_900_000, d), 100_000_000);
});

test("fitBandwidth picks the smallest choice that holds the width", () => {
  const choices = [12_500, 25_000, 200_000];
  assert.equal(fitBandwidth(10_000, choices), 12_500);
  assert.equal(fitBandwidth(25_000, choices), 25_000);
  assert.equal(fitBandwidth(30_000, choices), 200_000);
});

test("fitBandwidth falls back to the largest choice, whatever the order", () => {
  assert.equal(fitBandwidth(500_000, [200_000, 12_500, 25_000]), 200_000);
  assert.equal(fitBandwidth(20_000, [200_000, 12_500, 25_000]), 25_000);
});

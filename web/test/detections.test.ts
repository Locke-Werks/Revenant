import { test } from "node:test";
import assert from "node:assert/strict";
import {
  fitBandwidth,
  LingerTracker,
  LOST_LINGER_MS,
  LOST_MIN_OPACITY,
  lostClock,
  lostOpacity,
  parseDetections,
  snapTap,
  sortForList,
  type Detection,
} from "../src/detections";
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
  labelKind: "",
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

test("sortForList orders by centre frequency ascending, ties by id, without mutating", () => {
  const input = [
    det({ id: "9", centerHz: 150_000_000, snrDb: 10 }),
    det({ id: "2", centerHz: 90_000_000, snrDb: 5 }),
    det({ id: "1", centerHz: 150_000_000, snrDb: 40 }),
    det({ id: "10", centerHz: 150_000_000, snrDb: 10 }),
  ];
  const out = sortForList(input);
  assert.deepEqual(out.map((d) => d.id), ["2", "1", "9", "10"]);
  assert.equal(input[0]?.id, "9");
});

test("a level change does not move a row", () => {
  const a = det({ id: "1", centerHz: 100_000_000, snrDb: 10 });
  const b = det({ id: "2", centerHz: 101_000_000, snrDb: 20 });
  const before = sortForList([a, b]).map((d) => d.id);
  const after = sortForList([{ ...a, snrDb: 50 }, b]).map((d) => d.id);
  assert.deepEqual(after, before);
});

test("sortForList compares ids numerically past 2^53", () => {
  const out = sortForList([
    det({ id: "18446744073709551615" }),
    det({ id: "18446744073709551614" }),
  ]);
  assert.deepEqual(out.map((d) => d.id), ["18446744073709551614", "18446744073709551615"]);
});

// ---- Lost signals linger -----------------------------------------------------

const ids = (l: { id: string }[]) => l.map((d) => d.id);

test("a detection that leaves the list lingers as lost, then is removed", () => {
  const t = new LingerTracker();
  const a = det({ id: "1", centerHz: 100_000_000, bandwidthHz: 10_000 });
  t.update([a], 1000);
  t.update([], 1500);
  const [lost] = t.view(1500 + 4_200);
  assert.equal(lost?.id, "1");
  // Time since last listed: the last update that held it was at 1000.
  assert.equal(lost?.lostForMs, 4_700);
  assert.equal(t.hasLost(), true);
  assert.deepEqual(t.view(1000 + LOST_LINGER_MS - 1).length, 1);
  assert.deepEqual(t.view(1000 + LOST_LINGER_MS), []);
  assert.equal(t.hasLost(), false);
});

test("the fade runs from 1 to the floor over the linger", () => {
  assert.equal(lostOpacity(undefined), 1);
  assert.equal(lostOpacity(0), 1);
  assert.ok(Math.abs(lostOpacity(LOST_LINGER_MS / 2) - (1 + LOST_MIN_OPACITY) / 2) < 1e-9);
  assert.ok(Math.abs(lostOpacity(LOST_LINGER_MS) - LOST_MIN_OPACITY) < 1e-9);
  assert.ok(Math.abs(lostOpacity(LOST_LINGER_MS * 5) - LOST_MIN_OPACITY) < 1e-9);
  assert.ok(lostOpacity(LOST_LINGER_MS / 4) > lostOpacity(LOST_LINGER_MS / 2));
});

test("lost age reads as minutes and seconds", () => {
  assert.equal(lostClock(0), "0:00");
  assert.equal(lostClock(42_900), "0:42");
  assert.equal(lostClock(75_000), "1:15");
});

test("the same id coming back revives the row", () => {
  const t = new LingerTracker();
  const a = det({ id: "1" });
  t.update([a], 0);
  t.update([], 1000);
  assert.equal(t.view(2000)[0]?.lostForMs, 2000);
  t.update([a], 3000);
  const [back] = t.view(3000);
  assert.equal(back?.lostForMs, undefined);
  assert.equal(t.hasLost(), false);
});

test("a new id inside the lost band revives it, one outside does not", () => {
  const t = new LingerTracker();
  const a = det({ id: "1", centerHz: 100_000_000, bandwidthHz: 20_000 });
  t.update([a], 0);
  t.update([], 1000);
  t.update([det({ id: "2", centerHz: 100_030_000 })], 2000);
  assert.deepEqual(ids(t.view(2000)), ["2", "1"]);
  t.update([det({ id: "3", centerHz: 100_009_000 })], 3000);
  assert.deepEqual(ids(t.view(3000)), ["3"]);
});

test("a signal already listed does not revive a lost one under it", () => {
  const t = new LingerTracker();
  const wide = det({ id: "w", centerHz: 100_000_000, bandwidthHz: 200_000 });
  const narrow = det({ id: "n", centerHz: 100_010_000, bandwidthHz: 5_000 });
  t.update([wide, narrow], 0);
  t.update([wide], 1000);
  assert.deepEqual(ids(t.view(1000)), ["w", "n"]);
});

test("lost entries keep their last centre, label and width", () => {
  const t = new LingerTracker();
  const a = det({ id: "1", centerHz: 162_550_000, bandwidthHz: 9_000, label: "NFM", labelKind: "modulation" });
  t.update([a], 0);
  t.update([], 1000);
  const [lost] = t.view(1000);
  assert.equal(lost?.centerHz, 162_550_000);
  assert.equal(lost?.bandwidthHz, 9_000);
  assert.equal(lost?.label, "NFM");
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

import { test } from "node:test";
import assert from "node:assert/strict";
import { parseRds, RdsModel } from "../src/rds";
import type { GatewayEvent } from "../src/protocol";

const ev = (fields: Record<string, unknown>): GatewayEvent => ({ event: "rds", ...fields }) as GatewayEvent;

test("parseRds reads the station and treats blanks as absent", () => {
  assert.deepEqual(parseRds(ev({ vrx: 3, pi: 21670, callSign: "KQRS", ps: "KQRS FM", rt: " ", pty: null })), {
    vrx: 3,
    station: { pi: 21670, callSign: "KQRS", ps: "KQRS FM", rt: null, pty: null },
  });
  assert.equal(parseRds(ev({ ps: "X" })), null);
});

test("a RadioText reaches the feed once, however often it repeats", () => {
  const m = new RdsModel();
  assert.deepEqual(m.apply(ev({ vrx: 1, rt: "Now playing" })), { vrx: 1, newRt: "Now playing" });
  assert.deepEqual(m.apply(ev({ vrx: 1, rt: "Now playing", ps: "KQRS FM" })), { vrx: 1, newRt: null });
  assert.deepEqual(m.apply(ev({ vrx: 1, rt: null })), { vrx: 1, newRt: null });
  assert.deepEqual(m.apply(ev({ vrx: 1, rt: "Next up" })), { vrx: 1, newRt: "Next up" });
  // Per receiver: the same text on another one is its own message.
  assert.deepEqual(m.apply(ev({ vrx: 2, rt: "Next up" })), { vrx: 2, newRt: "Next up" });
});

test("the name is PS, then the call sign, and a dropped receiver has none", () => {
  const m = new RdsModel();
  assert.equal(m.name(1), null);
  m.apply(ev({ vrx: 1, callSign: "KQRS" }));
  assert.equal(m.name(1), "KQRS");
  m.apply(ev({ vrx: 1, callSign: "KQRS", ps: "92 KQRS" }));
  assert.equal(m.name(1), "92 KQRS");
  m.drop(1);
  assert.equal(m.name(1), null);
  // After a drop the same text is new again.
  m.apply(ev({ vrx: 1, rt: "Now playing" }));
  assert.deepEqual(m.apply(ev({ vrx: 1, rt: "Now playing" })), { vrx: 1, newRt: null });
});

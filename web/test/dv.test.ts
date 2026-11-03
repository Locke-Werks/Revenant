import { test } from "node:test";
import assert from "node:assert/strict";
import {
  callCells,
  followTarget,
  groupCells,
  hasDvReceiver,
  newestFirst,
  NO_DV,
  parseDv,
  parseRows,
  parseTrunk,
  pickCall,
  tabAfterDvChange,
  type CallRow,
} from "../src/dv";
import { readTab, SHEET_TABS } from "../src/ui/sheet";

function row(over: Partial<CallRow>): CallRow {
  return {
    id: 1,
    protocol: "P25",
    system: "NAC 344",
    target: "1201",
    alias: null,
    source: "7001",
    group: true,
    encrypted: false,
    emergency: false,
    frequencyHz: 851_050_000,
    vrx: 4,
    slot: 0,
    owner: "user",
    firstMs: 1000,
    lastMs: 2000,
    state: "live",
    count: 3,
    ...over,
  };
}

test("the DV tab is shown for any digital voice receiver and no other", () => {
  assert.equal(hasDvReceiver([]), false);
  assert.equal(hasDvReceiver([{ mode: "nfm" }, { mode: "wfm" }]), false);
  for (const mode of ["p25p1", "dmr", "dstar", "tetra"]) assert.equal(hasDvReceiver([{ mode: "am" }, { mode }]), true);
});

test("hiding the DV tab while it is open falls back to Tune, and leaves others alone", () => {
  assert.equal(tabAfterDvChange("dv", false), "tune");
  assert.equal(tabAfterDvChange("dv", true), "dv");
  assert.equal(tabAfterDvChange("signals", false), "signals");
});

test("the DV tab sits between Signals and Radio and can be reopened", () => {
  assert.deepEqual([...SHEET_TABS], ["tune", "signals", "dv", "radio"]);
  assert.equal(readTab("dv"), "dv");
});

test("calls are listed newest first", () => {
  const rows = newestFirst([row({ id: 1, firstMs: 10 }), row({ id: 3, firstMs: 30 }), row({ id: 2, firstMs: 30 })]);
  assert.deepEqual(
    rows.map((r) => r.id),
    [3, 2, 1],
  );
});

test("a call row reads time, talkgroup or alias, radio, receiver and state", () => {
  const c = callCells(row({ alias: "Fire Dispatch", owner: "trunk", encrypted: true }));
  assert.equal(c.talkgroup, "Fire Dispatch");
  assert.equal(c.radio, "7001");
  assert.equal(c.receiver, "#4 trunk");
  assert.equal(c.state, "live");
  assert.equal(c.encrypted, true);
  assert.match(c.time, /^\d\d:\d\d:\d\d$/);
  assert.equal(callCells(row({})).talkgroup, "TG 1201");
  assert.equal(callCells(row({ protocol: "D-STAR", group: false, target: "CQCQCQ" })).talkgroup, "CQCQCQ");
  const granted = callCells(row({ state: "granted" }));
  assert.equal(granted.state, "not followed");
  assert.equal(granted.receiver, "");
  assert.equal(callCells(row({ state: "ended" })).state, "ended");
});

test("a talkgroup row counts its calls", () => {
  assert.equal(groupCells(row({ count: 1 })).calls, "1 call");
  assert.equal(groupCells(row({ count: 5 })).calls, "5 calls");
});

test("tapping a call selects its receiver, unless it is gone or was never followed", () => {
  const picked: number[] = [];
  const select = (v: number) => picked.push(v);
  assert.equal(pickCall(row({ vrx: 4 }), [{ vrx: 4 }], select), true);
  assert.equal(pickCall(row({ vrx: 9 }), [{ vrx: 4 }], select), false);
  assert.equal(pickCall(row({ vrx: 4, state: "granted" }), [{ vrx: 4 }], select), false);
  assert.deepEqual(picked, [4]);
});

test("events parse into rows, switches and the trunk line", () => {
  const rows = parseRows({ event: "calls", items: [{ id: 7, target: "300", state: "granted", alias: "" }, 5] });
  assert.equal(rows.length, 1);
  assert.equal(rows[0]?.state, "granted");
  assert.equal(rows[0]?.alias, null);
  const dv = parseDv({ event: "dv", autoDv: true, follow: { vrx: 3, maxFollowers: 4 }, maxFollowers: 2 });
  assert.deepEqual(dv, { autoDv: true, follow: { vrx: 3, maxFollowers: 4 }, maxFollowers: 2 });
  assert.equal(parseTrunk({ event: "trunk", enabled: false, note: "" }).text, "Not following");
  assert.match(parseTrunk({ event: "trunk", enabled: true, controlVrx: 3, tsbks: 9, grants: 2, calls: [{ active: true }] }).text, /#3.*1 on the air/);
  assert.match(parseTrunk({ event: "trunk", fault: "old engine" }).text, /old engine/);
});

test("Follow is offered on a P25 receiver, or on the channel already followed", () => {
  assert.equal(followTarget({ vrx: 2, mode: "p25p1" }, NO_DV), 2);
  assert.equal(followTarget({ vrx: 2, mode: "nfm" }, NO_DV), null);
  assert.equal(followTarget(null, { ...NO_DV, follow: { vrx: 5, maxFollowers: 2 } }), 5);
});

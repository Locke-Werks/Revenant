import { test } from "node:test";
import assert from "node:assert/strict";
import { IDLE_RETRY_MS, RELOAD_GUARD_MS, reloadOnUpdate } from "../src/sw_reload";

function rig(controlled: boolean, stored: string | null = null) {
  let change: () => void = () => {};
  const timers: { fn: () => void; ms: number }[] = [];
  const s = { busy: false, reloads: 0, now: 100_000, store: new Map<string, string>() };
  if (stored !== null) s.store.set("revenant.swReloadAt", stored);
  reloadOnUpdate({
    controlled,
    onControllerChange: (fn) => void (change = fn),
    busy: () => s.busy,
    reload: () => void s.reloads++,
    now: () => s.now,
    setTimeout: (fn, ms) => void timers.push({ fn, ms }),
    store: { getItem: (k) => s.store.get(k) ?? null, setItem: (k, v) => void s.store.set(k, v) },
  });
  return { s, timers, change: () => change() };
}

test("a new worker taking over a controlled page reloads it once", () => {
  const r = rig(true);
  r.change();
  r.change();
  assert.equal(r.s.reloads, 1);
});

test("the first claim of an uncontrolled page does not reload", () => {
  const r = rig(false);
  r.change();
  assert.equal(r.s.reloads, 0);
  r.change();
  assert.equal(r.s.reloads, 1);
});

test("a reload just after another is refused, so a looping worker cannot loop the page", () => {
  const r = rig(true, String(100_000 - RELOAD_GUARD_MS + 1));
  r.change();
  assert.equal(r.s.reloads, 0);
});

test("a busy page waits until it is idle", () => {
  const r = rig(true);
  r.s.busy = true;
  r.change();
  assert.equal(r.s.reloads, 0);
  assert.equal(r.timers[0]!.ms, IDLE_RETRY_MS);
  r.s.busy = false;
  r.timers[0]!.fn();
  assert.equal(r.s.reloads, 1);
});

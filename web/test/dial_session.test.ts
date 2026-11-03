import { test } from "node:test";
import assert from "node:assert/strict";
import { DialSession } from "../src/dial_session";

function rig() {
  let t = 1000;
  const sent: { vrx: number; hz: number }[] = [];
  const ends: number[] = [];
  const state = { active: { vrx: 1, hz: 146_520_000, mode: "nfm" } as { vrx: number; hz: number; mode: string } | null };
  const previews: number[] = [];
  const s = new DialSession({
    active: () => state.active,
    preview: (hz) => {
      if (!state.active) return false;
      state.active = { ...state.active, hz: Math.round(hz) };
      previews.push(hz);
      return true;
    },
    tune: (vrx, hz) => sent.push({ vrx, hz }),
    end: () => ends.push(t),
    now: () => t,
    schedule: () => () => {},
  });
  return { s, sent, ends, state, previews, at: (n: number) => (t = n) };
}

function flick(s: DialSession, pxPerMs: number): number {
  s.down();
  let t = 0;
  for (let i = 0; i < 6; i++) {
    t += 16;
    s.drag(pxPerMs * 16, t);
  }
  s.release(t);
  return t;
}

test("stop flushes the last previewed value to the receiver it was shown on", () => {
  const r = rig();
  r.s.down();
  r.s.drag(0, 0);
  r.s.drag(5, 10);
  r.s.drag(30, 20);
  // The limiter sent the first value; later ones are still held back.
  const shown = r.state.active!.hz;
  assert.ok(shown < 146_520_000);
  r.s.stop();
  assert.equal(Math.round(r.sent.at(-1)!.hz), shown);
  assert.equal(r.sent.at(-1)!.vrx, 1);
  assert.equal(r.ends.length, 1);
  assert.equal(r.s.tuning, false);
});

test("stop with no session sends nothing", () => {
  const r = rig();
  r.s.down();
  r.s.stop();
  assert.deepEqual(r.sent, []);
  assert.equal(r.ends.length, 0);
});

test("a changed active receiver stops momentum and flushes to the old one", () => {
  const r = rig();
  const t0 = flick(r.s, 2);
  assert.equal(r.s.tuning, true);
  const shown = r.state.active!.hz;
  r.state.active = { vrx: 2, hz: 162_550_000, mode: "nfm" };
  r.s.check();
  assert.equal(r.s.tuning, false);
  assert.equal(r.s.physics.moving, false);
  assert.equal(r.sent.at(-1)!.vrx, 1);
  assert.equal(Math.round(r.sent.at(-1)!.hz), shown);
  const n = r.sent.length;
  r.s.tick(t0 + 100);
  r.s.tick(t0 + 700);
  assert.equal(r.sent.length, n);
  assert.equal(r.state.active.hz, 162_550_000);
});

test("a change noticed only at the next frame still never moves the new receiver", () => {
  const r = rig();
  const t0 = flick(r.s, 2);
  r.state.active = { vrx: 2, hz: 162_550_000, mode: "nfm" };
  r.s.tick(t0 + 16);
  assert.equal(r.state.active.hz, 162_550_000);
  assert.ok(r.sent.every((x) => x.vrx === 1));
});

test("the active receiver going away ends the session", () => {
  const r = rig();
  flick(r.s, 2);
  r.state.active = null;
  r.s.check();
  assert.equal(r.s.tuning, false);
  assert.equal(r.s.moving, false);
});

test("coasting to rest settles on a whole fine step and sends it", () => {
  const r = rig();
  const t0 = flick(r.s, 0.5);
  for (let t = t0 + 16; t <= t0 + 700; t += 16) r.s.tick(t);
  assert.equal(r.s.tuning, false);
  const last = r.sent.at(-1)!;
  assert.equal(last.hz % 100, 0);
  assert.equal(r.state.active!.hz, last.hz);
  assert.equal(r.ends.length, 1);
});

test("a tap starts no session", () => {
  const r = rig();
  r.s.down();
  r.s.release(50);
  assert.equal(r.s.tuning, false);
  assert.deepEqual(r.sent, []);
  assert.equal(r.ends.length, 0);
});

test("no active receiver means the drag goes nowhere", () => {
  const r = rig();
  r.state.active = null;
  r.s.down();
  r.s.drag(0, 0);
  r.s.drag(20, 10);
  assert.equal(r.s.tuning, false);
  assert.equal(r.s.physics.moving, false);
});

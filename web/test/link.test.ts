import { test } from "node:test";
import assert from "node:assert/strict";
import { BACKOFF_MAX_MS, LinkError, REQUEST_TIMEOUT_MS, STABLE_MS, Link, RESUME_KEY, wireLifecycle, type SocketLike } from "../src/link";
import { encodeSpectrum, PING_INTERVAL_MS } from "../src/protocol";

class FakeSocket implements SocketLike {
  binaryType = "blob";
  readyState = 0;
  sent: { id: number; op: string; args?: Record<string, unknown> }[] = [];
  closedByApp = false;
  onopen: ((ev: unknown) => void) | null = null;
  onclose: ((ev: unknown) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  constructor(readonly url: string) {}
  send(data: string) {
    this.sent.push(JSON.parse(data));
  }
  close() {
    this.closedByApp = true;
  }
  // Gateway side.
  open() {
    this.readyState = 1;
    this.onopen?.({});
  }
  drop() {
    this.readyState = 3;
    this.onclose?.({});
  }
  json(m: unknown) {
    this.onmessage?.({ data: JSON.stringify(m) });
  }
  binary(b: ArrayBuffer) {
    this.onmessage?.({ data: b });
  }
  last() {
    return this.sent[this.sent.length - 1]!;
  }
}

class FakeTimers {
  now = 0;
  private seq = 0;
  private items = new Map<number, { at: number; fn: () => void }>();
  setTimeout = (fn: () => void, ms: number) => {
    const id = ++this.seq;
    this.items.set(id, { at: this.now + ms, fn });
    return id;
  };
  clearTimeout = (h: unknown) => void this.items.delete(h as number);
  advance(ms: number) {
    const end = this.now + ms;
    for (;;) {
      let next: [number, { at: number; fn: () => void }] | undefined;
      for (const e of this.items) if (e[1].at <= end && (!next || e[1].at < next[1].at)) next = e;
      if (!next) break;
      this.items.delete(next[0]);
      this.now = next[1].at;
      next[1].fn();
    }
    this.now = end;
  }
  get count() {
    return this.items.size;
  }
}

class MemStore {
  m = new Map<string, string>();
  getItem = (k: string) => this.m.get(k) ?? null;
  setItem = (k: string, v: string) => void this.m.set(k, v);
  removeItem = (k: string) => void this.m.delete(k);
}

function rig(token?: string) {
  const sockets: FakeSocket[] = [];
  const timers = new FakeTimers();
  const store = new MemStore();
  if (token !== undefined) store.setItem(RESUME_KEY, token);
  const link = new Link({
    url: "wss://phone.test/ws",
    socket: (url) => {
      const s = new FakeSocket(url);
      sockets.push(s);
      return s;
    },
    timers,
    store,
    now: () => timers.now,
  });
  const ready: boolean[] = [];
  link.on("ready", (r) => ready.push(r.resumed));
  return { link, sockets, timers, store, ready, cur: () => sockets[sockets.length - 1]! };
}

const tick = () => new Promise((r) => setTimeout(r, 0));

test("resolves replies by id and rejects with the error text on ok:false", async () => {
  const { link, cur } = rig();
  link.start();
  cur().open();
  const a = link.request("tune", { vrx: 1, hz: 100 });
  const b = link.request("removeVrx", { vrx: 2 });
  const [ia, ib] = [cur().sent[0]!.id, cur().sent[1]!.id];
  assert.equal(ia === ib, false);
  cur().json({ id: ib, ok: false, error: "vrx: receiver 2 is not one of yours" });
  cur().json({ id: ia, ok: true, result: { vrx: 1, hz: 100 } });
  assert.deepEqual(await a, { vrx: 1, hz: 100 });
  await assert.rejects(b, { message: "vrx: receiver 2 is not one of yours" });
});

test("setMode reply is passed through whole, replaced and statusUnavailable included", async () => {
  const { link, cur } = rig();
  link.start();
  cur().open();
  const p = link.request("setMode", { vrx: 3, mode: "am" });
  const result = { vrx: 9, mode: "am", replaced: 3, statusUnavailable: true };
  cur().json({ id: cur().last().id, ok: true, result });
  assert.deepEqual(await p, result);
});

test("requests before ready are refused; pending ones reject when the socket drops", async () => {
  const { link, cur } = rig();
  await assert.rejects(link.request("tune", {}), /not connected/);
  link.start();
  await assert.rejects(link.request("tune", {}), /not connected/);
  cur().open();
  const p = link.request("tune", { vrx: 1, hz: 1 });
  cur().drop();
  await assert.rejects(p, /connection lost/);
});

test("answers every ping with pong", () => {
  const { link, cur } = rig();
  link.start();
  cur().open();
  cur().json({ event: "ping" });
  cur().json({ event: "ping" });
  const pongs = cur().sent.filter((c) => c.op === "pong");
  assert.equal(pongs.length, 2);
  assert.equal(pongs[0]!.id === pongs[1]!.id, false);
});

test("stores the hello token and sends resume first on reconnect", async () => {
  const { link, cur, store, timers, ready } = rig();
  link.start();
  cur().open();
  cur().json({ event: "hello", resume: "tok-a" });
  assert.equal(store.getItem(RESUME_KEY), "tok-a");
  assert.deepEqual(ready, [false]);

  cur().drop();
  timers.advance(500);
  cur().open();
  assert.equal(cur().sent.length, 1);
  assert.deepEqual(cur().sent[0], { id: cur().sent[0]!.id, op: "resume", args: { token: "tok-a" } });
  // Not ready until the resume settles, so nothing can jump ahead of it.
  assert.equal(link.ready, false);
  cur().json({ event: "hello", resume: "tok-fresh" });
  cur().json({ id: cur().sent[0]!.id, ok: true, result: { resumed: true } });
  await tick();
  assert.deepEqual(ready, [false, true]);
  assert.equal(store.getItem(RESUME_KEY), "tok-a");
});

test("a refused resume falls back to the fresh session's token", async () => {
  const { link, cur, store, ready } = rig("tok-old");
  link.start();
  cur().open();
  assert.equal(cur().sent[0]!.op, "resume");
  cur().json({ event: "hello", resume: "tok-new" });
  cur().json({ id: cur().sent[0]!.id, ok: false, error: "resume: unknown or expired token" });
  await tick();
  assert.deepEqual(ready, [false]);
  assert.equal(store.getItem(RESUME_KEY), "tok-new");
  assert.equal(link.ready, true);
});

// Drops the current socket and returns how long the next one took to appear.
function dropAndWait(sockets: FakeSocket[], timers: FakeTimers): number {
  const before = sockets.length;
  sockets[sockets.length - 1]!.drop();
  const t0 = timers.now;
  while (sockets.length === before) timers.advance(50);
  return timers.now - t0;
}

test("reconnects with backoff from 0.5 s doubling to 8 s, reset once ready", () => {
  const { link, sockets, timers } = rig();
  link.start();
  const delays: number[] = [];
  for (let i = 0; i < 7; i++) {
    const before = sockets.length;
    sockets[sockets.length - 1]!.drop();
    const t0 = timers.now;
    while (sockets.length === before) timers.advance(50);
    delays.push(timers.now - t0);
  }
  assert.deepEqual(delays, [500, 1000, 2000, 4000, 8000, 8000, 8000]);
  assert.equal(BACKOFF_MAX_MS, 8000);
  // Ready but dropped at once: the backoff does not start over.
  sockets[sockets.length - 1]!.open();
  assert.equal(dropAndWait(sockets, timers), 8000);
  // Up for STABLE_MS: it does.
  sockets[sockets.length - 1]!.open();
  timers.advance(STABLE_MS);
  assert.equal(dropAndWait(sockets, timers), 500);
});

test("coming back to the foreground or online reconnects at once", () => {
  const { link, sockets, timers } = rig();
  const docL: Record<string, () => void> = {};
  const winL: Record<string, () => void> = {};
  const doc = { visibilityState: "hidden", addEventListener: (e: string, f: () => void) => void (docL[e] = f) };
  wireLifecycle(link, doc, { addEventListener: (e: string, f: () => void) => void (winL[e] = f) });
  link.start();
  for (let i = 0; i < 4; i++) dropAndWait(sockets, timers);
  sockets[sockets.length - 1]!.drop(); // now waiting 8 s
  const n = sockets.length;
  docL["visibilitychange"]!();
  assert.equal(sockets.length, n, "hidden does not reconnect");
  doc.visibilityState = "visible";
  docL["visibilitychange"]!();
  assert.equal(sockets.length, n + 1);
  sockets[sockets.length - 1]!.open();
  timers.advance(8000);
  assert.equal(sockets.length, n + 1, "the pending retry was cancelled");
  sockets[sockets.length - 1]!.drop();
  timers.advance(499);
  winL["online"]!();
  assert.equal(sockets.length, n + 2);
  // A wake with a socket already up does not open a second one.
  winL["online"]!();
  assert.equal(sockets.length, n + 2);
});

test("events reach listeners: link with recreated and lost, source, vrxRemoved", () => {
  const { link, cur } = rig();
  const got: unknown[] = [];
  link.on("link", (e) => got.push(e.recreated, e.lost));
  link.on("source", (e) => got.push(e.centerHz));
  link.on("vrxRemoved", (e) => got.push(e.vrx));
  link.start();
  cur().open();
  cur().json({ event: "link", state: "up", detail: "", recreated: [{ old: 1, new: 7 }], lost: [{ vrx: 2, reason: "no" }] });
  cur().json({ event: "source", open: true, rateHz: 2_400_000, centerHz: 100_000_000 });
  cur().json({ event: "vrxRemoved", vrx: 2 });
  assert.deepEqual(got, [[{ old: 1, new: 7 }], [{ vrx: 2, reason: "no" }], 100_000_000, 2]);
});

test("binary spectrum frames are decoded with their own view", () => {
  const { link, cur } = rig();
  const rows: { centerHz: number; spanHz: number; width: number }[] = [];
  link.on("spectrum", (f) => rows.push({ centerHz: f.centerHz, spanHz: f.spanHz, width: f.px.length }));
  link.start();
  cur().open();
  assert.equal(cur().binaryType, "arraybuffer");
  cur().binary(
    encodeSpectrum({ offsetDb: -120, scaleDb: 0.5, sourceStart: 1n, centerHz: 1e8, spanHz: 2e6, px: new Uint8Array(64) }),
  );
  cur().binary(new ArrayBuffer(3));
  assert.deepEqual(rows, [{ centerHz: 1e8, spanHz: 2e6, width: 64 }]);
});

test("stop closes the socket and does not reconnect", () => {
  const { link, sockets, timers, cur } = rig();
  const downs: number[] = [];
  link.on("down", () => downs.push(1));
  link.start();
  cur().open();
  link.stop();
  assert.equal(cur().closedByApp, true);
  assert.equal(downs.length, 1);
  timers.advance(60_000);
  assert.equal(sockets.length, 1);
});

test("a socket silent past two ping intervals is replaced on wake", () => {
  const { link, sockets, timers, cur } = rig();
  link.start();
  cur().open();
  timers.advance(PING_INTERVAL_MS);
  cur().json({ event: "ping" });
  // Heard recently: wake keeps it.
  link.wake();
  assert.equal(sockets.length, 1);
  // Half open: nothing more arrives. Wake before the watchdog's next look.
  timers.now += 2 * PING_INTERVAL_MS + 1;
  link.wake();
  assert.equal(sockets.length, 2);
  assert.equal(sockets[0]!.closedByApp, true);
});

test("the watchdog replaces a silent socket without a wake", () => {
  const { link, sockets, timers, cur } = rig();
  link.start();
  cur().open();
  timers.advance(3 * PING_INTERVAL_MS + 1000);
  assert.equal(sockets.length >= 2, true);
  assert.equal(sockets[0]!.closedByApp, true);
});

test("a request on a dead socket rejects on timeout", async () => {
  const { link, timers, cur } = rig();
  link.start();
  cur().open();
  const p = link.request("tune", { vrx: 1, hz: 1 });
  timers.advance(REQUEST_TIMEOUT_MS - 1);
  let settled = false;
  p.catch(() => {}).finally(() => void (settled = true));
  await tick();
  assert.equal(settled, false);
  timers.advance(1);
  await assert.rejects(p, /no reply/);
  await p.catch((e: unknown) => assert.equal(e instanceof LinkError, true));
});

test("a request can carry its own timeout", async () => {
  const { link, timers, cur } = rig();
  link.start();
  cur().open();
  const p = link.request("openSource", { uri: "rtlsdr:0" }, { timeoutMs: 30_000 });
  let settled = false;
  p.catch(() => {}).finally(() => void (settled = true));
  timers.advance(REQUEST_TIMEOUT_MS + 1);
  await tick();
  assert.equal(settled, false);
  timers.advance(30_000 - REQUEST_TIMEOUT_MS - 1);
  await assert.rejects(p, /openSource: no reply in 30 s/);
});

test("a resume that times out keeps the token and reconnects", async () => {
  const { link, sockets, timers, store, cur } = rig("tok-keep");
  link.start();
  cur().open();
  timers.advance(REQUEST_TIMEOUT_MS);
  await tick();
  assert.equal(store.getItem(RESUME_KEY), "tok-keep");
  timers.advance(500);
  assert.equal(sockets.length, 2);
});

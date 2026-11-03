import { test } from "node:test";
import assert from "node:assert/strict";
import { colormap } from "../src/colormap";
import { BOX_CARRY_ROWS, RESTORE_WAIT_MS, RING_ROWS, RowRing, WaterfallHost, type Renderer } from "../src/waterfall";
import { panBy, type View } from "../src/view";

const v: View = { centerHz: 100_000_000, spanHz: 2_000_000, widthPx: 1000 };

test("the ring wraps and keeps each row's own view", () => {
  const r = new RowRing(2000);
  for (let i = 0; i < RING_ROWS + 3; i++) r.push(v.centerHz + i, v.spanHz, 1000);
  assert.equal(r.head, 2);
  assert.equal(r.center[2], v.centerHz + RING_ROWS + 2);
  assert.equal(r.width[0], 1000);
});

test("placing rows: a fresh slot follows the view, an empty slot draws nothing", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  r.place(v);
  assert.deepEqual([...r.meta.subarray(slot * 4, slot * 4 + 3)], [0, 0.5, 0.5]);
  assert.deepEqual([...r.meta.subarray(4, 7)], [0, 0, 0]);
  // After a pan of half a span, the same row starts half way across its width.
  r.place(panBy(v, v.spanHz / 2), slot);
  assert.equal(r.meta[slot * 4], 0.25);
});

const box = { lowHz: v.centerHz - 50_000, highHz: v.centerHz + 50_000, alpha: 0.3, held: false, label: "NFM" };

test("detection boxes are stamped on the row current when they arrive", () => {
  const r = new RowRing(2000);
  r.push(v.centerHz, v.spanHz, 1000);
  const before = r.push(v.centerHz, v.spanHz, 1000);
  r.stamp([box]);
  assert.deepEqual(r.boxes[before], [box]);
  assert.deepEqual(r.boxes[before - 1], []);
  // Rows between two events carry the latest list, then stop once it is stale.
  const next = r.push(v.centerHz, v.spanHz, 1000);
  assert.deepEqual(r.boxes[next], [box]);
  let last = next;
  for (let i = 0; i < BOX_CARRY_ROWS; i++) last = r.push(v.centerHz, v.spanHz, 1000);
  assert.deepEqual(r.boxes[last], []);
});

test("a later retune moves boxes with their rows", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  r.stamp([box]);
  const px = (view: View) => r.boxesPx(slot, view, 1000).map((p) => [Math.round(p.x0), Math.round(p.x1), p.box]);
  assert.deepEqual(px(v), [[475, 525, box]]);
  assert.deepEqual(px(panBy(v, v.spanHz / 4)), [[225, 275, box]]);
  // Off the old row's edge the box is clipped to what that row covered.
  const edge = { ...box, lowHz: v.centerHz + 950_000, highHz: v.centerHz + 1_100_000 };
  r.stamp([edge]);
  assert.deepEqual(px(panBy(v, v.spanHz / 2)), [[475, 500, edge]]);
});

test("boxes leave with their rows when the ring wraps", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  r.stamp([box]);
  for (let i = 0; i < RING_ROWS - 1; i++) r.push(v.centerHz, v.spanHz, 1000);
  assert.deepEqual(r.boxes[slot], [box]);
  r.push(v.centerHz, v.spanHz, 1000);
  assert.equal(r.head, slot);
  assert.deepEqual(r.boxes[slot], []);
  r.stamp([box]);
  r.clear();
  assert.deepEqual(r.boxes[slot], []);
});

test("rows wider than the texture are clipped to it", () => {
  const r = new RowRing(512);
  const slot = r.push(1, 1, 8192);
  assert.equal(r.width[slot], 512);
});

test("colour map runs from the background to yellow-white, opaque", () => {
  const c = colormap();
  assert.equal(c.length, 1024);
  assert.deepEqual([...c.subarray(0, 4)], [0x14, 0x14, 0x18, 255]);
  assert.deepEqual([...c.subarray(1020, 1024)], [0xff, 0xf0, 0xa0, 255]);
  for (let i = 3; i < 1024; i += 4) assert.equal(c[i], 255);
});

function fakeRenderer(kind: string, log: string[]): Renderer {
  return {
    resize: (w, h) => log.push(`${kind} resize ${w}x${h}`),
    setView: (x) => log.push(`${kind} view ${x.centerHz}`),
    pushRow: () => log.push(`${kind} row`),
    clear: () => log.push(`${kind} clear`),
    draw: () => log.push(`${kind} draw`),
    setDetections: () => log.push(`${kind} detections`),
    drawBoxes: () => log.push(`${kind} boxes`),
  };
}

function hostRig(glWorks: () => boolean) {
  const log: string[] = [];
  const timers = new Map<number, () => void>();
  let seq = 0;
  const host = new WaterfallHost(
    {
      gl: () => {
        if (!glWorks()) throw new Error("shader failed");
        return fakeRenderer("gl", log);
      },
      canvas2d: () => fakeRenderer("2d", log),
    },
    {
      setTimeout: (fn, ms) => {
        assert.equal(ms, RESTORE_WAIT_MS);
        timers.set(++seq, fn);
        return seq;
      },
      clearTimeout: (h) => void timers.delete(h as number),
    },
  );
  const fire = () => {
    const f = [...timers.values()];
    timers.clear();
    f.forEach((x) => x());
  };
  return { host, log, fire, pending: () => timers.size };
}

const row = { offsetDb: 0, scaleDb: 1, sourceStart: 0n, centerHz: 1, spanHz: 1, px: new Uint8Array(4) };

test("a GL setup failure falls back to 2D at once", () => {
  const { host, log } = hostRig(() => false);
  assert.equal(host.state, "2d");
  host.draw();
  assert.deepEqual(log, ["2d draw"]);
});

test("a lost context drops rows, then a restore rebuilds GL with size and view", () => {
  let ok = true;
  const { host, log, pending } = hostRig(() => ok);
  host.resize(300, 200);
  host.setView(v);
  host.contextLost();
  assert.equal(host.state, "lost");
  log.length = 0;
  host.pushRow(row);
  host.draw();
  assert.deepEqual(log, []);
  host.contextRestored();
  assert.equal(host.state, "gl");
  assert.equal(pending(), 0);
  assert.deepEqual(log, ["gl resize 300x200", "gl view 100000000"]);
});

test("a context that never comes back hands over to 2D", () => {
  const { host, log, fire } = hostRig(() => true);
  host.resize(10, 10);
  host.contextLost();
  log.length = 0;
  fire();
  assert.equal(host.state, "2d");
  assert.deepEqual(log, ["2d resize 10x10"]);
  host.contextRestored(); // too late: ignored
  assert.equal(host.state, "2d");
});

test("a restore whose rebuild fails goes to 2D", () => {
  let ok = true;
  const { host } = hostRig(() => ok);
  host.contextLost();
  ok = false;
  host.contextRestored();
  assert.equal(host.state, "2d");
});

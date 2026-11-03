import { test } from "node:test";
import assert from "node:assert/strict";
import { BOX_CARRY_MS, BOX_CARRY_ROWS, boxCarryRows, RESTORE_WAIT_MS, RING_ROWS, RowRing, rowAt, rowTop, slotAtY, WaterfallHost, type Renderer } from "../src/waterfall";
import type { Detection } from "../src/detections";
import { BOX_TAP_SLACK_PX, detectionBoxes, pickBox } from "../src/markers";
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

test("boxes carry the same time at any row rate", () => {
  for (const rate of [3.8, 36.6, 60]) {
    const rows = boxCarryRows(1000 / rate);
    assert.ok(Math.abs((rows * 1000) / rate - BOX_CARRY_MS) <= 1000 / rate / 2 + 1e-9, `${rate}`);
  }
  assert.equal(boxCarryRows(5000), 1);
  assert.equal(boxCarryRows(0.01), RING_ROWS);
  const r = new RowRing(2000);
  r.push(v.centerHz, v.spanHz, 1000);
  r.stamp([box], 3);
  let last = 0;
  for (let i = 0; i < 3; i++) last = r.push(v.centerHz, v.spanHz, 1000);
  assert.deepEqual(r.boxes[last], [box]);
  last = r.push(v.centerHz, v.spanHz, 1000);
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

// ---- Tapping a stamped detection box -------------------------------------------

const detAt = (id: string, centerHz: number, bandwidthHz: number, snrDb = 10): Detection => ({
  id,
  centerHz,
  bandwidthHz,
  snrDb,
  confidence: 0.9,
  state: "live",
  label: "NFM",
  labelKind: "modulation",
  labelConfidence: 0.8,
  lastSeen: 0,
});

test("a tap inside a live box finds its detection, a miss finds none", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  r.stamp(detectionBoxes([detAt("1", v.centerHz, 100_000)]));
  assert.equal(r.boxAt(slot, v, 1000, 500, BOX_TAP_SLACK_PX)?.detection?.id, "1");
  assert.equal(r.boxAt(slot, v, 1000, 478, BOX_TAP_SLACK_PX)?.detection?.id, "1");
  assert.equal(r.boxAt(slot, v, 1000, 600, BOX_TAP_SLACK_PX), null);
});

test("a sliver of a box is still tappable within the slack", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  r.stamp(detectionBoxes([detAt("1", v.centerHz, 1_000)]));
  assert.equal(r.boxAt(slot, v, 1000, 505, BOX_TAP_SLACK_PX)?.detection?.id, "1");
  assert.equal(r.boxAt(slot, v, 1000, 507, BOX_TAP_SLACK_PX), null);
});

test("a box on an old row from a signal that has gone is still tappable", () => {
  const r = new RowRing(2000);
  const old = r.push(v.centerHz, v.spanHz, 1000);
  r.stamp(detectionBoxes([detAt("1", v.centerHz, 100_000)]), 2);
  let last = old;
  for (let i = 0; i < 6; i++) last = r.push(v.centerHz, v.spanHz, 1000);
  assert.equal(r.boxAt(old, v, 1000, 500, BOX_TAP_SLACK_PX)?.detection?.id, "1");
  assert.equal(r.boxAt(last, v, 1000, 500, BOX_TAP_SLACK_PX), null);
});

test("the hit follows the row's own view after a retune", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  r.stamp(detectionBoxes([detAt("1", v.centerHz, 100_000)]));
  const moved = panBy(v, v.spanHz / 4);
  assert.equal(r.boxAt(slot, moved, 1000, 250, BOX_TAP_SLACK_PX)?.detection?.id, "1");
  assert.equal(r.boxAt(slot, moved, 1000, 500, BOX_TAP_SLACK_PX), null);
  // A different row centre puts the same hertz elsewhere on screen.
  const other = r.push(v.centerHz + 500_000, v.spanHz, 1000);
  r.stamp(detectionBoxes([detAt("2", v.centerHz + 500_000, 100_000)]));
  assert.equal(r.boxAt(other, v, 1000, 750, BOX_TAP_SLACK_PX)?.detection?.id, "2");
});

test("an empty slot and a row with no boxes give nothing", () => {
  const r = new RowRing(2000);
  assert.equal(r.boxAt(5, v, 1000, 500, BOX_TAP_SLACK_PX), null);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  assert.equal(r.boxAt(slot, v, 1000, 500, BOX_TAP_SLACK_PX), null);
});

test("a tap between two boxes goes to the one it is nearer the middle of", () => {
  const r = new RowRing(2000);
  const slot = r.push(v.centerHz, v.spanHz, 1000);
  // 400 to 600 px wide box over a narrow one at 520 px; the tap at 515 is
  // 5 px from the narrow centre (depth 5/6) and 15 px from the wide (0.15).
  r.stamp(detectionBoxes([detAt("w", v.centerHz + 30_000, 200_000, 30), detAt("n", v.centerHz + 40_000, 1_000, 10)]));
  assert.equal(r.boxAt(slot, v, 1000, 515, BOX_TAP_SLACK_PX)?.detection?.id, "w");
  assert.equal(r.boxAt(slot, v, 1000, 520, BOX_TAP_SLACK_PX)?.detection?.id, "n");
});

test("equal depth goes to the louder box, then the lower id", () => {
  const hits = [
    { x0: 90, x1: 110, box: detectionBoxes([detAt("5", 1, 1, 10)])[0]! },
    { x0: 90, x1: 110, box: detectionBoxes([detAt("4", 1, 1, 20)])[0]! },
    { x0: 90, x1: 110, box: detectionBoxes([detAt("3", 1, 1, 20)])[0]! },
  ];
  assert.equal(pickBox(hits, 100, 6)?.detection?.id, "3");
});

test("the tap's height maps to the ring slot the row was drawn in", () => {
  assert.equal(slotAtY(10, 0, 0), 10);
  assert.equal(slotAtY(10, 3.5 / RING_ROWS, 0), 7);
  // History scrolled down by a row: the row under the finger is one newer.
  assert.equal(slotAtY(10, 3.5 / RING_ROWS, 1), 8);
  assert.equal(slotAtY(2, 5.5 / RING_ROWS, 0), RING_ROWS - 3);
  assert.equal(slotAtY(10, 1, 0), null);
  assert.equal(slotAtY(-1, 0.5, 0), null);
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

function fakeRenderer(kind: string, log: string[], scrolls = kind === "gl"): Renderer {
  return {
    resize: (w, h) => log.push(`${kind} resize ${w}x${h}`),
    setView: (x) => log.push(`${kind} view ${x.centerHz}`),
    pushRow: () => log.push(`${kind} row`),
    setPalette: (p) => log.push(`${kind} palette ${p[0]}`),
    clear: () => log.push(`${kind} clear`),
    scrolls,
    draw: (shift = 0) => log.push(shift === 0 ? `${kind} draw` : `${kind} draw ${shift}`),
    setDetections: () => log.push(`${kind} detections`),
    drawBoxes: () => log.push(`${kind} boxes`),
    boxAt: () => null,
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

const row = {
  offsetDb: 0, scaleDb: 1, sourceStart: 0n, centerHz: 1, spanHz: 1, px: new Uint8Array(4),
  traceOffsetDb: 0, traceScaleDb: 1, tracePx: new Uint8Array(4),
};

test("the desktop's palette reaches the renderer and outlives a handover", () => {
  const { host, log, fire } = hostRig(() => true);
  const palette = new Uint8Array(1024).fill(7);
  host.setPalette(palette);
  assert.deepEqual(log, ["gl palette 7"]);
  host.resize(10, 10);
  host.contextLost();
  log.length = 0;
  fire();
  // Given before the resize, which clears a 2D canvas to the palette's bottom.
  assert.deepEqual(log, ["2d palette 7", "2d resize 10x10"]);
});

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

test("between rows each row is drawn, and boxed, over the same pixels", () => {
  // The shader picks a pixel's row with rowAt and the box overlay starts a
  // row at rowTop, so a box stays on its row through the scroll.
  for (const shift of [0, -0.25, -0.5, -0.999, -1, -1.6]) {
    for (const k of [0, 1, 7, RING_ROWS - 1]) {
      const top = rowTop(k, shift);
      assert.equal(rowAt(top + 1e-6, shift), k);
      assert.equal(rowAt(top + 1 - 1e-6, shift), k);
    }
  }
  // At rest the rows are where they always were.
  assert.equal(rowTop(5, 0), 5);
  // With the newest row a quarter in, the top quarter of the screen is it.
  assert.equal(rowAt(0.2, -0.75), 0);
  assert.equal(rowAt(0.3, -0.75), 1);
});

test("only GL scrolls between rows; the host says which it has", () => {
  let ok = true;
  const { host, log, fire } = hostRig(() => ok);
  assert.equal(host.scrolls, true);
  host.draw(-0.5);
  assert.deepEqual(log, ["gl draw -0.5"]);
  host.contextLost();
  assert.equal(host.scrolls, false);
  ok = false;
  fire();
  assert.equal(host.state, "2d");
  assert.equal(host.scrolls, false);
});

import { test } from "node:test";
import assert from "node:assert/strict";
import {
  CaptionLayer,
  captionFromEvent,
  CaptionStore,
  DETACHED,
  MAX_CAPTIONS,
  MAX_CAPTION_CHARS,
  placeCaption,
  RowClock,
  rowForSample,
  type Caption,
} from "../src/ui/captions";
import { RING_ROWS } from "../src/waterfall";
import {
  decodedEntry,
  FeedModel,
  FeedView,
  MAX_FEED,
  MAX_FEED_CHARS,
  transcriptEntry,
  type HostEl,
} from "../src/ui/feed";
import {
  downloadPercent,
  parseSpeechStatus,
  SpeechModel,
  speechLabel,
  speechLine,
  type SpeechStatus,
} from "../src/ui/speech_switch";
import type { GateLink } from "../src/ui/receivers";
import type { JsonValue } from "../src/protocol";

// ---- rowForSample ------------------------------------------------------------

const rows = [100, 200, 300, 400].map((source_start) => ({ source_start }));

test("rowForSample: an exact hit is that row", () => {
  assert.equal(rowForSample(rows, 100), 0);
  assert.equal(rowForSample(rows, 300), 2);
});

test("rowForSample: between rows is the row whose frame covers it", () => {
  assert.equal(rowForSample(rows, 250), 1);
  assert.equal(rowForSample(rows, 399), 2);
});

test("rowForSample: older than the oldest row is null", () => {
  assert.equal(rowForSample(rows, 99), null);
  assert.equal(rowForSample([], 5), null);
});

test("rowForSample: newer than the newest row is the newest row", () => {
  assert.equal(rowForSample(rows, 10_000), 3);
});

// ---- RowClock ----------------------------------------------------------------

test("RowClock ages count back from the newest row", () => {
  const c = new RowClock();
  for (const s of [100, 200, 300]) c.push(s);
  assert.equal(c.ageOf(300), 0);
  assert.equal(c.ageOf(250), 1);
  assert.equal(c.ageOf(100), 2);
  assert.equal(c.ageOf(99), null);
});

test("RowClock keeps the right rows after the ring wraps", () => {
  const c = new RowClock();
  const total = RING_ROWS + 50;
  for (let i = 0; i < total; i++) c.push(i * 1000);
  assert.equal(c.size, RING_ROWS);
  assert.equal(c.ageOf((total - 1) * 1000), 0);
  assert.equal(c.ageOf((total - 11) * 1000 + 5), 10);
  // The oldest retained row is index 50; anything before it was overwritten.
  assert.equal(c.ageOf(50 * 1000), RING_ROWS - 1);
  assert.equal(c.ageOf(49 * 1000 + 999), null);
  assert.equal(c.ageOf(0), null);
});

test("RowClock: a source that starts over drops what came before", () => {
  const c = new RowClock();
  for (let i = 0; i < 10; i++) c.push(1_000_000 + i * 1000);
  // The new source counts from zero.
  assert.equal(c.push(0), true);
  assert.equal(c.size, 1);
  assert.equal(c.ageOf(1_005_000), 0, "a sample past the new newest row clamps to it, it cannot reach old rows");
  assert.equal(c.push(1000), false);
  assert.equal(c.ageOf(500), 1);
});

test("RowClock ignores a row without a usable clock", () => {
  const c = new RowClock();
  c.push(100);
  assert.equal(c.push(Number.NaN), false);
  assert.equal(c.push(Number.POSITIVE_INFINITY), false);
  assert.equal(c.size, 1);
  assert.equal(c.ageOf(150), 0);
});

test("RowClock.clear forgets everything", () => {
  const c = new RowClock();
  c.push(10);
  c.clear();
  assert.equal(c.size, 0);
  assert.equal(c.ageOf(10), null);
});

// ---- Placement ---------------------------------------------------------------

const VIEW = { centerHz: 100_000_000, spanHz: 2_000_000, widthPx: 1000 };

function caption(over: Partial<Caption> = {}): Caption {
  return { id: 1, vrx: 1, hz: 100_000_000, text: "hello", sourceStart: 4_000, sourceEnd: 6_000, confidence: 1, ...over };
}

function clockOf(starts: number[]): RowClock {
  const c = new RowClock();
  for (const s of starts) c.push(s);
  return c;
}

// 21 rows, one every 1000 samples: ages run 0 (20000) to 20 (0).
const CLOCK_STARTS = Array.from({ length: 21 }, (_, i) => i * 1000);

test("placeCaption pins at the receiver's frequency and the rows its samples cover", () => {
  const p = placeCaption(caption(), clockOf(CLOCK_STARTS), VIEW, 1000, 2, 1024)!;
  assert.equal(p.visible, true);
  assert.equal(p.x, 500);
  // sourceEnd 6000 is age 14, sourceStart 4000 is age 16: rows 14..16.
  assert.equal(p.top, 14 * 2);
  assert.equal(p.height, 3 * 2);
});

test("placeCaption rides the waterfall's scroll between rows", () => {
  // A third of the newest row scrolled in: every row sits two thirds of a
  // row above where it rests, the caption with them, and its height is kept.
  const p = placeCaption(caption(), clockOf(CLOCK_STARTS), VIEW, 1000, 2, 1024, -2 / 3)!;
  assert.ok(Math.abs(p.top - (14 - 2 / 3) * 2) < 1e-9);
  assert.equal(p.height, 3 * 2);
});

test("placeCaption follows pan and zoom", () => {
  const panned = { ...VIEW, centerHz: 99_000_000 };
  assert.equal(placeCaption(caption(), clockOf(CLOCK_STARTS), panned, 1000, 2, 1024)!.x, 1000);
  const zoomed = { ...VIEW, spanHz: 1_000_000 };
  const edge = placeCaption(caption({ hz: 100_250_000 }), clockOf(CLOCK_STARTS), zoomed, 1000, 2, 1024)!;
  assert.equal(edge.x, 750);
});

test("placeCaption hides, without dropping, a caption panned out of view", () => {
  const p = placeCaption(caption({ hz: 120_000_000 }), clockOf(CLOCK_STARTS), VIEW, 1000, 2, 1024)!;
  assert.equal(p.visible, false);
});

test("placeCaption drops a caption whose rows are all gone", () => {
  assert.equal(placeCaption(caption({ sourceStart: 0, sourceEnd: 500 }), clockOf([1000, 2000, 3000]), VIEW, 1000, 2, 1024), null);
});

test("placeCaption keeps a caption whose start has scrolled off but whose end has not", () => {
  const p = placeCaption(caption({ sourceStart: 500, sourceEnd: 2500 }), clockOf([1000, 2000, 3000]), VIEW, 1000, 2, 1024)!;
  // The end is age 1; the start is older than the oldest row, so it runs to it (age 2).
  assert.equal(p.top, 2);
  assert.equal(p.height, 4);
});

test("placeCaption drops one that has scrolled past the rows on screen", () => {
  const clock = clockOf(CLOCK_STARTS);
  // 10 rows are visible; sourceEnd 6000 is at age 14.
  assert.equal(placeCaption(caption(), clock, VIEW, 1000, 2, 10), null);
});

test("placeCaption fades as the rows scroll toward the bottom", () => {
  const clock = clockOf(Array.from({ length: 100 }, (_, i) => i * 1000));
  const at = (age: number) => placeCaption(caption({ sourceStart: (99 - age) * 1000, sourceEnd: (99 - age) * 1000 }), clock, VIEW, 1000, 2, 100)!.opacity;
  assert.equal(at(0), 1);
  assert.ok(at(60) < at(40));
  assert.ok(at(90) < at(60));
  assert.ok(at(90) > 0);
});

// ---- The store ---------------------------------------------------------------

function add(store: CaptionStore, vrx: number, n: number) {
  store.add({ vrx, hz: 1, text: `t${n}`, sourceStart: n, sourceEnd: n + 1, confidence: 1 });
}

test("the store keeps at most MAX_CAPTIONS, dropping the oldest", () => {
  const store = new CaptionStore();
  for (let i = 0; i < MAX_CAPTIONS + 5; i++) add(store, 1, i);
  const texts = store.list().map((c) => c.text);
  assert.equal(texts.length, MAX_CAPTIONS);
  assert.equal(texts[0], "t5");
  assert.equal(texts[texts.length - 1], `t${MAX_CAPTIONS + 4}`);
});

test("the store remaps receiver ids in one step, so a swap holds", () => {
  const store = new CaptionStore();
  add(store, 1, 0);
  add(store, 2, 1);
  store.remap([
    { old: 1, new: 2 },
    { old: 2, new: 1 },
  ]);
  assert.deepEqual(store.list().map((c) => c.vrx), [2, 1]);
  store.remap([{ old: 1, new: 9 }]);
  assert.deepEqual(store.list().map((c) => c.vrx), [2, 9]);
});

test("a removed receiver's captions stay, detached from its id", () => {
  const store = new CaptionStore();
  add(store, 1, 0);
  add(store, 2, 1);
  add(store, 1, 2);
  store.detachReceiver(1);
  assert.deepEqual(store.list().map((c) => c.vrx), [DETACHED, 2, DETACHED]);
  // A restart remapping the old id 1 does not carry them to a new receiver.
  store.remap([{ old: 1, new: 5 }]);
  assert.deepEqual(store.list().map((c) => c.vrx), [DETACHED, 2, DETACHED]);
});

test("the store can drop one caption or all of them", () => {
  const store = new CaptionStore();
  add(store, 1, 0);
  add(store, 1, 1);
  store.remove(store.list()[0]!.id);
  assert.equal(store.list().length, 1);
  store.clear();
  assert.equal(store.list().length, 0);
});

// ---- Speech switch -----------------------------------------------------------

function st(over: Partial<SpeechStatus> = {}): SpeechStatus {
  return { enabled: true, modelState: "ready", modelName: "base.en", bytesDone: 0, bytesTotal: 0, detail: "", ...over };
}

test("speech label per state", () => {
  assert.equal(speechLabel(null), "Speech");
  assert.equal(speechLabel(st({ enabled: false })), "Speech off");
  assert.equal(speechLabel(st({ enabled: false, modelState: "failed" })), "Speech off");
  assert.equal(speechLabel(st({ modelState: "absent" })), "Speech: waiting for model");
  assert.equal(speechLabel(st({ modelState: "verifying" })), "Speech: verifying");
  assert.equal(speechLabel(st({ modelState: "loading" })), "Speech: loading");
  assert.equal(speechLabel(st({ modelState: "ready" })), "Speech on");
  assert.equal(speechLabel(st({ modelState: "failed", detail: "disk full" })), "Speech failed");
});

test("speech label shows the download percent from bytesDone over bytesTotal", () => {
  const dl = (done: number, total: number) => st({ modelState: "downloading", bytesDone: done, bytesTotal: total });
  assert.equal(speechLabel(dl(42, 100)), "Speech: downloading 42%");
  assert.equal(speechLabel(dl(1, 3)), "Speech: downloading 33%");
  assert.equal(speechLabel(dl(150, 100)), "Speech: downloading 100%");
  assert.equal(speechLabel(dl(0, 0)), "Speech: downloading");
  assert.equal(downloadPercent(dl(0, 0)), null);
  assert.equal(downloadPercent(st()), null);
});

test("the speech line carries the failure's own sentence", () => {
  assert.equal(speechLine(st({ modelState: "failed", detail: "no space left on device" })), "Speech failed: no space left on device");
  assert.equal(speechLine(st({ modelState: "failed" })), "Speech failed");
  assert.match(speechLine(st({ modelState: "downloading", bytesDone: 5, bytesTotal: 10, modelName: "base.en" })), /50%/);
  assert.equal(speechLine(st({ enabled: false })), "Speech is off");
});

test("parseSpeechStatus reads the gateway's object and refuses nonsense", () => {
  const s = parseSpeechStatus({ enabled: true, modelState: "downloading", modelName: "m", bytesDone: 3, bytesTotal: 9, detail: "", backend: "vulkan" });
  assert.deepEqual(s, { enabled: true, modelState: "downloading", modelName: "m", bytesDone: 3, bytesTotal: 9, detail: "" });
  assert.equal(parseSpeechStatus(null), null);
  assert.equal(parseSpeechStatus({ enabled: true, modelState: "sleeping" }), null);
  assert.equal(parseSpeechStatus({ modelState: "ready" }), null);
});

class SpeechLink implements GateLink {
  ready = true;
  sent: { op: string; args?: { [key: string]: JsonValue } }[] = [];
  private replies: { resolve: (v: JsonValue) => void; reject: (e: Error) => void }[] = [];
  private fns = new Map<string, Set<(e: never) => void>>();
  request(op: string, args?: { [key: string]: JsonValue }): Promise<JsonValue> {
    this.sent.push({ op, args });
    return new Promise((resolve, reject) => this.replies.push({ resolve, reject }));
  }
  on(name: string, fn: (e: never) => void): () => void {
    let s = this.fns.get(name);
    if (!s) this.fns.set(name, (s = new Set()));
    s.add(fn);
    return () => void s!.delete(fn);
  }
  emit(name: string, e: unknown): void {
    for (const fn of this.fns.get(name) ?? []) (fn as (e: unknown) => void)(e);
  }
  reply(v: JsonValue): void {
    this.replies.shift()!.resolve(v);
  }
  refuse(t: string): void {
    this.replies.shift()!.reject(new Error(t));
  }
}

async function flush(): Promise<void> {
  for (let i = 0; i < 10; i++) await Promise.resolve();
}

function status(over: { [key: string]: JsonValue } = {}): JsonValue {
  return { enabled: true, modelState: "ready", modelName: "m", bytesDone: 0, bytesTotal: 0, detail: "", ...over };
}

test("the switch asks for on, and the reply and later pushes set the state", async () => {
  const link = new SpeechLink();
  const m = new SpeechModel({ link });
  assert.equal(m.status, null);
  m.toggle();
  assert.deepEqual(link.sent[0], { op: "setTranscription", args: { on: true } });
  link.reply(status({ modelState: "downloading", bytesDone: 10, bytesTotal: 100 }));
  await flush();
  assert.equal(speechLabel(m.status), "Speech: downloading 10%");
  // The gateway pushes progress; the phone never asks again by itself.
  link.emit("transcriptionStatus", { event: "transcriptionStatus", ...(status({ modelState: "downloading", bytesDone: 60, bytesTotal: 100 }) as object) });
  assert.equal(speechLabel(m.status), "Speech: downloading 60%");
  link.emit("transcriptionStatus", { event: "transcriptionStatus", ...(status({ modelState: "ready" }) as object) });
  assert.equal(speechLabel(m.status), "Speech on");
  assert.equal(link.sent.length, 1);
});

test("a pushed state shows before the phone has touched the switch", () => {
  const link = new SpeechLink();
  const m = new SpeechModel({ link });
  link.emit("transcriptionStatus", { event: "transcriptionStatus", ...(status() as object) });
  assert.equal(speechLabel(m.status), "Speech on");
  m.toggle();
  assert.deepEqual(link.sent[0], { op: "setTranscription", args: { on: false } });
});

test("the switch turns speech off when it is on", async () => {
  const link = new SpeechLink();
  const m = new SpeechModel({ link });
  m.toggle();
  link.reply(status());
  await flush();
  m.toggle();
  assert.deepEqual(link.sent[1], { op: "setTranscription", args: { on: false } });
  link.reply(status({ enabled: false }));
  await flush();
  assert.equal(speechLabel(m.status), "Speech off");
});

test("a failed model shows the engine sentence, and a tap retries", async () => {
  const link = new SpeechLink();
  const m = new SpeechModel({ link });
  m.toggle();
  link.reply(status({ modelState: "failed", detail: "checksum did not match" }));
  await flush();
  assert.equal(speechLine(m.status), "Speech failed: checksum did not match");
  m.toggle();
  assert.deepEqual(link.sent[1], { op: "setTranscription", args: { on: true } });
});

test("every refusal is reported, including the same sentence twice", async () => {
  const link = new SpeechLink();
  const m = new SpeechModel({ link });
  const told: string[] = [];
  m.onRefused((t) => told.push(t));
  m.toggle();
  link.refuse("the engine does not transcribe");
  await flush();
  assert.equal(m.error, "the engine does not transcribe");
  m.toggle();
  assert.equal(m.error, null, "cleared when the next request starts");
  link.refuse("the engine does not transcribe");
  await flush();
  assert.deepEqual(told, ["the engine does not transcribe", "the engine does not transcribe"]);
  assert.equal(m.status, null);
});

test("a request that fails because the link dropped is not reported as a refusal", async () => {
  const link = new SpeechLink();
  const m = new SpeechModel({ link });
  const told: string[] = [];
  m.onRefused((t) => told.push(t));
  m.toggle();
  link.ready = false;
  link.refuse("connection lost");
  await flush();
  assert.deepEqual(told, []);
});

// ---- Feed --------------------------------------------------------------------

test("the feed lists the active receiver's entries newest first", () => {
  const feed = new FeedModel();
  feed.add({ kind: "transcript", vrx: 1, label: "Speech", text: "first" });
  feed.add({ kind: "decoded", vrx: 2, label: "p25p1", text: "other" });
  feed.add({ kind: "transcript", vrx: 1, label: "Speech", text: "second" });
  assert.deepEqual(feed.forReceiver(1).map((e) => e.text), ["second", "first"]);
  assert.deepEqual(feed.forReceiver(2).map((e) => e.text), ["other"]);
  assert.deepEqual(feed.forReceiver(null), []);
});

test("the feed keeps at most MAX_FEED entries, dropping the oldest", () => {
  const feed = new FeedModel();
  for (let i = 0; i < MAX_FEED + 30; i++) feed.add({ kind: "transcript", vrx: 1, label: "Speech", text: `t${i}` });
  const all = feed.forReceiver(1);
  assert.equal(all.length, MAX_FEED);
  assert.equal(all[0]!.text, `t${MAX_FEED + 29}`);
  assert.equal(all[all.length - 1]!.text, "t30");
});

test("the feed drops a receiver entries and clips long text", () => {
  const feed = new FeedModel();
  feed.add({ kind: "transcript", vrx: 1, label: "Speech", text: "a" });
  feed.add({ kind: "transcript", vrx: 2, label: "Speech", text: "b" });
  feed.dropReceiver(1);
  assert.deepEqual(feed.forReceiver(1), []);
  assert.equal(feed.forReceiver(2).length, 1);
  const long = transcriptEntry({ event: "transcript", vrx: 2, text: "x".repeat(MAX_FEED_CHARS + 100) })!;
  assert.equal(long.text.length, MAX_FEED_CHARS);
  assert.ok(long.text.endsWith("\u2026"));
  const d = decodedEntry({ event: "decoded", vrx: 2, decoder: "p25p1", kind: "", text: "y".repeat(MAX_FEED_CHARS + 1) })!;
  assert.equal(d.text.length, MAX_FEED_CHARS);
});

test("the feed follows a receiver's id changes", () => {
  const feed = new FeedModel();
  feed.add({ kind: "transcript", vrx: 1, label: "Speech", text: "a" });
  feed.add({ kind: "transcript", vrx: 2, label: "Speech", text: "b" });
  feed.remap([
    { old: 1, new: 2 },
    { old: 2, new: 1 },
  ]);
  assert.deepEqual(feed.forReceiver(2).map((e) => e.text), ["a"]);
  assert.deepEqual(feed.forReceiver(1).map((e) => e.text), ["b"]);
});

test("transcript and decoded events become entries", () => {
  assert.deepEqual(transcriptEntry({ event: "transcript", vrx: 3, text: " hello there ", mode: "nfm" }), {
    kind: "transcript",
    vrx: 3,
    label: "Speech",
    text: "hello there",
  });
  assert.equal(transcriptEntry({ event: "transcript", vrx: 3, text: "   " }), null);
  assert.equal(transcriptEntry({ event: "transcript", text: "no receiver" }), null);

  const withText = decodedEntry({ event: "decoded", vrx: 4, decoder: "p25p1", kind: "voice", text: "TG 1201", fields: {} });
  assert.deepEqual(withText, { kind: "decoded", vrx: 4, label: "p25p1 voice", text: "TG 1201" });
  const fieldsOnly = decodedEntry({ event: "decoded", vrx: 4, decoder: "p25p1", kind: "", text: "", fields: { talkgroup: 1201, source: 77 } });
  assert.equal(fieldsOnly!.text, "talkgroup 1201, source 77");
  assert.equal(decodedEntry({ event: "decoded", vrx: 4, decoder: "p25p1", kind: "", text: "", fields: {} }), null);
});

class FakeEl implements HostEl {
  className = "";
  private text = "";
  children: FakeEl[] = [];
  set textContent(v: string) {
    this.text = v;
  }
  get textContent(): string {
    return this.text;
  }
  // The feed must never take markup: any attempt to set it fails the test.
  set innerHTML(_v: string) {
    throw new Error("innerHTML used");
  }
  append(...nodes: FakeEl[]): void {
    this.children.push(...nodes);
  }
  replaceChildren(...nodes: FakeEl[]): void {
    this.children = nodes;
  }
}

test("the feed view puts engine text in as text, never as markup", () => {
  const feed = new FeedModel();
  const evil = `<img src=x onerror="alert(1)"> & "quotes"`;
  feed.add({ kind: "transcript", vrx: 1, label: "<b>Speech</b>", text: evil });
  const host = new FakeEl();
  const view = new FeedView(host, feed, () => new FakeEl());
  view.render(1);
  assert.equal(host.children.length, 1);
  const row = host.children[0]!;
  const joined = [row.textContent, ...row.children.map((c) => c.textContent)].join("|");
  assert.ok(joined.includes(evil), joined);
  assert.ok(joined.includes("<b>Speech</b>"), joined);
});

test("the feed view says so when there is nothing to show", () => {
  const host = new FakeEl();
  const view = new FeedView(host, new FeedModel(), () => new FakeEl());
  view.render(null);
  assert.equal(host.children.length, 1);
  assert.match(host.children[0]!.textContent, /No receiver/);
  view.render(1);
  assert.match(host.children[0]!.textContent, /Nothing decoded yet/);
});

// ---- captionFromEvent and the layer ------------------------------------------

test("captionFromEvent needs a receiver, a frequency, a sample range and text", () => {
  const ok = { event: "transcript" as const, vrx: 1, centerHz: 146_520_000, sourceStart: 10, sourceEnd: 20, text: " hi ", confidence: 0.9 };
  assert.deepEqual(captionFromEvent(ok), { vrx: 1, hz: 146_520_000, text: "hi", sourceStart: 10, sourceEnd: 20, confidence: 0.9 });
  for (const missing of ["vrx", "centerHz", "sourceStart", "sourceEnd", "text"]) {
    const e: { [k: string]: JsonValue } = { ...ok };
    delete e[missing];
    assert.equal(captionFromEvent(e as typeof ok), null, missing);
  }
  assert.equal(captionFromEvent({ ...ok, text: "   " }), null);
});

test("captionFromEvent truncates long text and repairs a backwards range", () => {
  const base = { event: "transcript" as const, vrx: 1, centerHz: 1, sourceStart: 50, sourceEnd: 20, text: "w".repeat(MAX_CAPTION_CHARS + 50) };
  const c = captionFromEvent(base)!;
  assert.equal(c.text.length, MAX_CAPTION_CHARS);
  assert.ok(c.text.endsWith("\u2026"));
  assert.equal(c.sourceEnd, 50);
  assert.equal(c.confidence, 1);
});

class FakeNode {
  className = "";
  textContent = "";
  hidden = false;
  style: { [k: string]: string } = {};
  children: FakeNode[] = [];
  parent: FakeNode | null = null;
  append(...nodes: FakeNode[]): void {
    for (const n of nodes) {
      n.parent = this;
      this.children.push(n);
    }
  }
  remove(): void {
    if (this.parent) this.parent.children = this.parent.children.filter((c) => c !== this);
    this.parent = null;
  }
}

test("the caption layer builds an element per live caption, moves it, hides it off view and removes it when dropped", () => {
  const host = new FakeNode();
  const store = new CaptionStore();
  const clock = clockOf(CLOCK_STARTS);
  const layer = new CaptionLayer(host as unknown as HTMLElement, store, clock, () => new FakeNode() as unknown as HTMLElement);
  const g = { view: VIEW, widthCss: 1000, rowHeightCss: 2, visibleRows: 1024 };
  store.add({ vrx: 1, hz: 100_000_000, text: "<b>hi</b>", sourceStart: 4_000, sourceEnd: 6_000, confidence: 0.2 });

  layer.layout(g);
  assert.equal(host.children.length, 1);
  const el = host.children[0]!;
  assert.equal(el.className, "caption doubt");
  assert.equal(el.children[0]!.textContent, "<b>hi</b>");
  assert.equal(el.hidden, false);
  assert.equal(el.style.transform, "translate(500.0px, 28.0px)");
  assert.equal(el.style.height, "6.0px");

  // Panned away: kept, hidden, and the same element.
  layer.layout({ ...g, view: { ...VIEW, centerHz: 130_000_000 } });
  assert.equal(host.children.length, 1);
  assert.equal(host.children[0], el);
  assert.equal(el.hidden, true);

  // Back in view, and the same element again.
  layer.layout(g);
  assert.equal(el.hidden, false);

  // Its rows scroll past what is visible: element and store entry go.
  layer.layout({ ...g, visibleRows: 10 });
  assert.equal(host.children.length, 0);
  assert.equal(store.list().length, 0);
});

test("the caption layer removes the element of a caption the store cleared", () => {
  const host = new FakeNode();
  const store = new CaptionStore();
  const layer = new CaptionLayer(host as unknown as HTMLElement, store, clockOf(CLOCK_STARTS), () => new FakeNode() as unknown as HTMLElement);
  const g = { view: VIEW, widthCss: 1000, rowHeightCss: 2, visibleRows: 1024 };
  store.add({ vrx: 1, hz: 100_000_000, text: "x", sourceStart: 4_000, sourceEnd: 6_000, confidence: 1 });
  layer.layout(g);
  assert.equal(host.children.length, 1);
  store.clear();
  layer.layout(g);
  assert.equal(host.children.length, 0);
});

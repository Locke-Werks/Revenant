import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DEFAULT_MODE,
  formatMHz,
  parseFrequency,
  readFrequency,
  ReceiverController,
  ReplaceGate,
  type ControllerAudio,
  type ControllerKeep,
  type GateLink,
} from "../src/ui/receivers";
import { BEHIND_BELOW, sourceBehind, StatusModel } from "../src/ui/status";
import { gainId, openUri, OPEN_SOURCE_TIMEOUT_MS, setCenter } from "../src/ui/radio_panel";

// ---- Opening a source --------------------------------------------------------

const rtl = {
  uri: "rtlsdr://0",
  backend: "rtlsdr",
  tuneRanges: [{ lowHz: 24_000_000, highHz: 1_766_000_000 }],
  sampleRates: [],
  minRate: 225_001,
  maxRate: 3_200_000,
};

test("an RTL-SDR opens with a centre and the desktop's rate", () => {
  assert.equal(openUri(rtl, null), "rtlsdr://0?rate=2400000&freq=98100000");
});

test("the centre the phone was on is kept when the tuner reaches it", () => {
  assert.equal(openUri(rtl, 162_550_000), "rtlsdr://0?rate=2400000&freq=162550000");
});

test("a centre outside the tuner's range falls back to the default", () => {
  assert.equal(openUri(rtl, 7_100_000), "rtlsdr://0?rate=2400000&freq=98100000");
});

test("a default outside every range is clamped into the first one", () => {
  const hf = { ...rtl, tuneRanges: [{ lowHz: 1_000_000, highHz: 30_000_000 }] };
  assert.equal(openUri(hf, null), "rtlsdr://0?rate=2400000&freq=30000000");
});

test("the synthetic scene opens at 2.4 MS/s, not its 20 MS/s default", () => {
  const syn = { uri: "synthetic:wideband", backend: "synthetic", tuneRanges: [], sampleRates: [20_000_000], minRate: 20_000_000, maxRate: 20_000_000 };
  assert.equal(openUri(syn, 98_100_000), "synthetic:wideband?rate=2400000");
});

test("a URI that already carries a query is extended, not replaced", () => {
  assert.equal(openUri({ ...rtl, uri: "rtlsdr://0?ppm=12" }, null), "rtlsdr://0?ppm=12&rate=2400000&freq=98100000");
});

test("a device that cannot run 2.4 MS/s gets no rate", () => {
  assert.equal(openUri({ ...rtl, maxRate: 2_000_000 }, null), "rtlsdr://0?freq=98100000");
});
import { sheetHint } from "../src/ui/sheet";
import { levelInBand } from "../src/ui/top_bar";
import type { GatewayEvent, JsonValue, LinkEvent } from "../src/protocol";

// ---- Frequency entry ---------------------------------------------------------

test("frequency entry accepts MHz, a unit suffix, or plain hertz", () => {
  for (const text of ["146.52", "146.52M", "146.52 MHz", "146520k", "146520 kHz", "146520000", "146520000Hz", " 146.520 "]) {
    assert.equal(parseFrequency(text), 146_520_000, text);
  }
  assert.equal(parseFrequency("1.2G"), 1_200_000_000);
  assert.equal(parseFrequency("7"), 7_000_000);
});

test("frequency entry refuses what is not a frequency", () => {
  for (const text of ["", "   ", "abc", "146.52x", "-5", "0", "1.2.3", "M"]) {
    assert.equal(parseFrequency(text), null, JSON.stringify(text));
  }
});

test("formatMHz shows four decimals", () => {
  assert.equal(formatMHz(146_520_000), "146.5200");
  assert.equal(formatMHz(7_074_000), "7.0740");
});

// ---- Fakes -------------------------------------------------------------------

type Handler = (e: never) => void;

class FakeLink implements GateLink {
  ready = true;
  sent: { op: string; args?: { [key: string]: JsonValue }; timeoutMs?: number }[] = [];
  private replies: { resolve: (v: JsonValue) => void; reject: (e: Error) => void }[] = [];
  private fns = new Map<string, Set<Handler>>();
  request(op: string, args?: { [key: string]: JsonValue }, opts?: { timeoutMs?: number }): Promise<JsonValue> {
    this.sent.push({ op, args, timeoutMs: opts?.timeoutMs });
    return new Promise((resolve, reject) => this.replies.push({ resolve, reject }));
  }
  on(name: string, fn: Handler): () => void {
    let s = this.fns.get(name);
    if (!s) this.fns.set(name, (s = new Set()));
    s.add(fn);
    return () => void s!.delete(fn);
  }
  emit(name: string, e: unknown): void {
    for (const fn of this.fns.get(name) ?? []) (fn as (e: unknown) => void)(e);
  }
  // Answers the oldest unanswered request.
  reply(result: JsonValue): void {
    this.replies.shift()!.resolve(result);
  }
  refuse(text: string): void {
    this.replies.shift()!.reject(new Error(text));
  }
  last() {
    return this.sent[this.sent.length - 1]!;
  }
}

class FakeAudio implements ControllerAudio {
  vrx: number | null = null;
  unlocks = 0;
  started: number[] = [];
  refuse: string | null = null;
  resumeOk = true;
  constructor(gate: ReplaceGate) {
    // Stands in for LiveAudio, which clears itself on a removal of its id.
    gate.on("vrxRemoved", (e: GatewayEvent) => {
      if (e.vrx === this.vrx) this.vrx = null;
    });
  }
  unlock(): Promise<void> {
    this.unlocks++;
    return Promise.resolve();
  }
  start(vrx: number): Promise<void> {
    if (this.refuse !== null) return Promise.reject(new Error(this.refuse));
    this.started.push(vrx);
    this.vrx = vrx;
    return Promise.resolve();
  }
  stop(): void {
    this.vrx = null;
  }
  resume(): Promise<boolean> {
    return Promise.resolve(this.resumeOk);
  }
  retarget(oldVrx: number, newVrx: number): void {
    if (this.vrx === oldVrx) this.vrx = newVrx;
  }
}

class FakeKeep implements ControllerKeep {
  following: number | null = null;
  removedSeen: number[] = [];
  now: { hz: number; mode: string } | null = null;
  constructor(gate: ReplaceGate) {
    gate.on("vrxRemoved", (e: GatewayEvent) => {
      if (e.vrx === this.following) this.removedSeen.push(e.vrx as number);
    });
  }
  retarget(oldVrx: number, newVrx: number): void {
    if (this.following === oldVrx) this.following = newVrx;
  }
  setNowPlaying(now: { hz: number; mode: string }): void {
    this.now = now;
  }
}

async function flush(): Promise<void> {
  for (let i = 0; i < 10; i++) await Promise.resolve();
}

function rig() {
  const link = new FakeLink();
  const gate = new ReplaceGate(link);
  const audio = new FakeAudio(gate);
  const keep = new FakeKeep(gate);
  const toasts: string[] = [];
  const notes: (string | null)[] = [];
  const ctl = new ReceiverController({
    link: gate,
    audio,
    keep,
    toast: (t) => toasts.push(t),
    audioNote: (t) => notes.push(t),
  });
  return { link, gate, audio, keep, ctl, toasts, notes, chips: () => ctl.list.list().map((r) => `${r.vrx}:${r.hz}:${r.mode}`) };
}

function status(vrx: number, hz: number, mode = "nfm", extra: { [key: string]: JsonValue } = {}): JsonValue {
  return { vrx, hz, mode, bandwidthHz: 12_500, squelchDbfs: -80, clamped: false, ...extra };
}

// ---- Receivers ---------------------------------------------------------------

test("a tap with no receiver adds one in NFM, selects it and listens", async () => {
  const r = rig();
  r.ctl.tap(146_520_000.4);
  assert.equal(r.audio.unlocks, 1);
  assert.deepEqual(r.link.last(), { op: "addVrx", args: { hz: 146_520_000, mode: DEFAULT_MODE }, timeoutMs: undefined });
  r.link.reply(status(1, 146_520_000));
  await flush();
  assert.deepEqual(r.chips(), ["1:146520000:nfm"]);
  assert.equal(r.ctl.list.active, 1);
  assert.deepEqual(r.audio.started, [1]);
  assert.deepEqual(r.keep.now, { hz: 146_520_000, mode: "nfm" });
});

test("a tap with an active receiver tunes it", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  r.ctl.tap(146_940_000);
  assert.deepEqual(r.link.last(), { op: "tune", args: { vrx: 1, hz: 146_940_000 }, timeoutMs: undefined });
  r.link.reply(status(1, 146_940_000));
  await flush();
  assert.deepEqual(r.chips(), ["1:146940000:nfm"]);
});

async function dragRig() {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  return r;
}

test("replies to a drag's tunes do not move the preview back mid-drag", async () => {
  const r = await dragRig();
  r.ctl.preview(146_600_000);
  r.ctl.tuneDragged(146_600_000);
  // The limiter holds a newer value; the preview has moved on.
  r.ctl.preview(146_700_000);
  r.link.reply(status(1, 146_600_000));
  await flush();
  assert.deepEqual(r.chips(), ["1:146700000:nfm"]);
});

test("the engine's answer to the last tune lands when the drag ends", async () => {
  const r = await dragRig();
  r.ctl.preview(146_700_000);
  r.ctl.tuneDragged(146_700_000);
  r.link.reply(status(1, 146_705_000));
  await flush();
  assert.deepEqual(r.chips(), ["1:146700000:nfm"]);
  r.ctl.endDrag();
  assert.deepEqual(r.chips(), ["1:146705000:nfm"]);
});

test("a held reply older than the final flush is dropped", async () => {
  const r = await dragRig();
  r.ctl.preview(146_600_000);
  r.ctl.tuneDragged(146_600_000);
  r.link.reply(status(1, 146_600_000));
  await flush();
  r.ctl.preview(146_700_000);
  r.ctl.tuneDragged(146_700_000); // the final flush
  r.ctl.endDrag();
  assert.deepEqual(r.chips(), ["1:146700000:nfm"]);
  r.link.reply(status(1, 146_700_000));
  await flush();
  assert.deepEqual(r.chips(), ["1:146700000:nfm"]);
});

test("a status event cannot move the previewed frequency mid-drag", async () => {
  const r = await dragRig();
  r.ctl.preview(146_700_000);
  r.link.emit("vrxStatus", { event: "vrxStatus", vrx: 1, hz: 146_520_000, audioError: "x" });
  assert.deepEqual(r.chips(), ["1:146700000:nfm"]);
});

test("a long press adds a second receiver and makes it active", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  r.ctl.longPress(162_550_000);
  assert.equal(r.link.last().op, "addVrx");
  r.link.reply(status(2, 162_550_000));
  await flush();
  assert.deepEqual(r.chips(), ["1:146520000:nfm", "2:162550000:nfm"]);
  assert.equal(r.ctl.list.active, 2);
  assert.deepEqual(r.audio.started, [1, 2]);
});

test("chips follow add, status and remove events", async () => {
  const r = rig();
  r.ctl.longPress(100_000_000);
  r.link.reply(status(4, 100_000_000, "wfm"));
  await flush();
  r.link.emit("vrxStatus", { event: "vrxStatus", vrx: 4, audioError: "the receiver's audio is at 24000 Hz" });
  assert.equal(r.ctl.list.get(4)!.audioError, "the receiver's audio is at 24000 Hz");
  assert.deepEqual(r.toasts, ["the receiver's audio is at 24000 Hz"]);
  // A status for an id this phone never added is not a chip.
  r.link.emit("vrxStatus", { event: "vrxStatus", vrx: 9, audioError: "x" });
  assert.deepEqual(r.chips(), ["4:100000000:wfm"]);
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 4, reason: "the source was retuned away from it" });
  assert.deepEqual(r.chips(), []);
  assert.equal(r.ctl.list.active, null);
  assert.equal(r.toasts[1], "Receiver at 100.0000 MHz removed: the source was retuned away from it");
});

test("a refused command shows the engine's sentence and changes nothing", async () => {
  const r = rig();
  r.ctl.tap(100_000_000);
  r.link.refuse("add_vrx: 100000000 Hz is outside the source's band");
  await flush();
  assert.deepEqual(r.toasts, ["add_vrx: 100000000 Hz is outside the source's band"]);
  assert.deepEqual(r.chips(), []);
});

test("statusUnavailable shows the receiver as sent, with no error", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000, "nfm", { statusUnavailable: true }));
  await flush();
  assert.deepEqual(r.chips(), ["1:146520000:nfm"]);
  assert.equal(r.ctl.list.get(1)!.statusUnavailable, true);
  assert.deepEqual(r.toasts, []);
});

test("a clamped receiver says why", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000, "nfm", { clamped: true, clampReason: "bandwidth clamped to the channel" }));
  await flush();
  assert.deepEqual(r.toasts, ["bandwidth clamped to the channel"]);
});

async function listening(r: ReturnType<typeof rig>, vrx: number) {
  r.ctl.tap(146_520_000);
  r.link.reply(status(vrx, 146_520_000));
  await flush();
  r.keep.following = vrx;
}

test("setMode replace: reply before the old id's removal keeps audio on", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.setMode("am");
  assert.deepEqual(r.link.last().args, { vrx: 1, mode: "am" });
  r.link.reply(status(2, 146_520_000, "am", { replaced: 1 }));
  await flush();
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "replaced" });
  assert.equal(r.audio.vrx, 2);
  assert.equal(r.keep.following, 2);
  assert.equal(r.ctl.list.active, 2);
  assert.deepEqual(r.chips(), ["2:146520000:am"]);
  assert.deepEqual(r.toasts, []);
  assert.deepEqual(r.keep.removedSeen, []);
});

test("setBandwidth replace: the old id's removal before the reply keeps audio on", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.setBandwidth(25_000);
  assert.deepEqual(r.link.last().args, { vrx: 1, hz: 25_000 });
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "replaced" });
  // Held until the reply says where the receiver went.
  assert.equal(r.audio.vrx, 1);
  assert.deepEqual(r.chips(), ["1:146520000:nfm"]);
  r.link.reply(status(3, 146_520_000, "nfm", { bandwidthHz: 25_000, replaced: 1 }));
  await flush();
  assert.equal(r.audio.vrx, 3);
  assert.equal(r.keep.following, 3);
  assert.equal(r.ctl.list.active, 3);
  assert.deepEqual(r.chips(), ["3:146520000:nfm"]);
  assert.equal(r.ctl.list.get(3)!.bandwidthHz, 25_000);
  assert.deepEqual(r.toasts, []);
  assert.deepEqual(r.keep.removedSeen, []);
});

test("a refused replace lets a held removal through", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.setMode("am");
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "the source closed" });
  r.link.refuse("set_vrx_params: remote exception: no");
  await flush();
  assert.equal(r.audio.vrx, null);
  assert.deepEqual(r.chips(), []);
  assert.deepEqual(r.toasts, ["set_vrx_params: remote exception: no", "Receiver at 146.5200 MHz removed: the source closed"]);
});

test("the same mode again is a plain update", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.setMode("nfm");
  r.link.reply(status(1, 146_520_000));
  await flush();
  assert.equal(r.audio.vrx, 1);
  assert.deepEqual(r.chips(), ["1:146520000:nfm"]);
});

test("an engine restart remaps chips, the active receiver and audio, and reports what was lost", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.longPress(162_550_000);
  r.link.reply(status(2, 162_550_000));
  await flush();
  r.ctl.select(1);
  await flush();
  // The fresh engine numbers from 1: old 1 became 2, old 2 was refused.
  const e: LinkEvent = {
    event: "link",
    state: "up",
    detail: "connected",
    recreated: [{ old: 1, new: 2 }],
    lost: [{ vrx: 2, reason: "outside the source's band" }],
  };
  r.link.emit("link", e);
  // LiveAudio and KeepListening follow the map from the same event
  // themselves (live.test.ts, keep_listening.test.ts); the controller must
  // not retarget them a second time, which would undo a swap.
  assert.deepEqual(r.chips(), ["2:146520000:nfm"]);
  assert.equal(r.ctl.list.active, 2);
  assert.equal(r.audio.vrx, 1);
  assert.deepEqual(r.toasts, ["Receiver at 162.5500 MHz lost when the engine restarted: outside the source's band"]);
  // The lost id's own vrxRemoved names the old id, which is now the new
  // receiver's id; it must not take the recreated receiver with it.
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 2, reason: "outside the source's band" });
  assert.deepEqual(r.chips(), ["2:146520000:nfm"]);
});

test("a swapped map is applied all at once", async () => {
  const r = rig();
  r.ctl.longPress(100_000_000);
  r.link.reply(status(1, 100_000_000));
  await flush();
  r.ctl.longPress(101_000_000);
  r.link.reply(status(2, 101_000_000));
  await flush();
  r.link.emit("link", { event: "link", state: "up", detail: "", recreated: [{ old: 1, new: 2 }, { old: 2, new: 1 }], lost: [] });
  assert.deepEqual(r.chips(), ["1:101000000:nfm", "2:100000000:nfm"]);
  assert.equal(r.ctl.list.active, 1);
});

test("a live audio refusal is shown, not only logged", async () => {
  const r = rig();
  r.audio.refuse = "audio runs at 44100 Hz; live audio needs 48000";
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  assert.deepEqual(r.notes, ["Live audio unavailable: audio runs at 44100 Hz; live audio needs 48000"]);
  r.audio.refuse = null;
  r.audio.resumeOk = false;
  r.ctl.select(1);
  await flush();
  assert.equal(r.notes[r.notes.length - 1], "The phone has not started sound yet. Tap the waterfall or a receiver to allow it.");
  r.audio.resumeOk = true;
  r.ctl.select(1);
  await flush();
  assert.equal(r.notes[r.notes.length - 1], null);
});

test("removing the active receiver clears the selection", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.remove(1);
  assert.deepEqual(r.link.last().args, { vrx: 1 });
  r.link.reply({ vrx: 1 });
  await flush();
  assert.deepEqual(r.chips(), []);
  assert.equal(r.ctl.list.active, null);
  assert.equal(r.audio.vrx, null);
  // The removal event that follows is not news.
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "removed" });
  assert.deepEqual(r.toasts, []);
});

// ---- Status --------------------------------------------------------------------

test("source behind real time below 0.98, measured, and not paced slow on purpose", () => {
  assert.equal(BEHIND_BELOW, 0.98);
  assert.equal(sourceBehind({ event: "source", open: true, realtimeFactor: 0.97, pacedBy: 0 }), true);
  assert.equal(sourceBehind({ event: "source", open: true, realtimeFactor: 0.98, pacedBy: 0 }), false);
  assert.equal(sourceBehind({ event: "source", open: true, realtimeFactor: 0.5, pacedBy: 1 }), true);
  // Not measured yet.
  assert.equal(sourceBehind({ event: "source", open: true, realtimeFactor: 0, pacedBy: 0 }), false);
  // A replay paced at half speed is doing what it was told.
  assert.equal(sourceBehind({ event: "source", open: true, realtimeFactor: 0.5, pacedBy: 0.5 }), false);
  assert.equal(sourceBehind({ event: "source", open: false }), false);
  assert.equal(sourceBehind({ event: "source", open: true }), false);
});

test("the status pill names what is wrong and the waterfall freezes while not up", () => {
  const s = new StatusModel();
  assert.equal(s.pill(), "connecting");
  assert.equal(s.frozen(), true);
  s.socket(true);
  s.link("waiting");
  assert.equal(s.pill(), "waiting for engine");
  assert.equal(s.frozen(), true);
  s.link("up");
  s.source({ event: "source", open: false });
  assert.equal(s.pill(), "no radio open");
  assert.equal(s.frozen(), false);
  s.source({ event: "source", open: true, rateHz: 2_400_000, centerHz: 100_000_000, realtimeFactor: 1, pacedBy: 0 });
  assert.equal(s.pill(), null);
  s.link("waiting");
  assert.equal(s.pill(), "engine restarting");
  s.link("up");
  s.socket(false);
  assert.equal(s.pill(), "reconnecting");
  assert.equal(s.frozen(), true);
  s.socket(true);
  s.link("refused", "the engine refused the token");
  assert.equal(s.pill(), "engine refused: the engine refused the token");
});

test("status chips: behind real time, and receiver events ended", () => {
  const s = new StatusModel();
  s.socket(true);
  s.link("up");
  s.source({ event: "source", open: true, rateHz: 1, centerHz: 1, realtimeFactor: 0.9, pacedBy: 0 });
  assert.deepEqual(s.chips(), ["source behind real time"]);
  s.eventsEnded("subscriber fell behind");
  assert.deepEqual(s.chips(), ["source behind real time", "removals elsewhere may go unreported (subscriber fell behind)"]);
  s.source({ event: "source", open: true, rateHz: 1, centerHz: 1, realtimeFactor: 1, pacedBy: 0 });
  assert.deepEqual(s.chips(), ["removals elsewhere may go unreported (subscriber fell behind)"]);
});

test("opening and listing sources waits longer than an ordinary request", () => {
  assert.equal(OPEN_SOURCE_TIMEOUT_MS, 30_000);
});

// ---- S-meter -------------------------------------------------------------------

test("the S-meter reads the strongest bin inside the receiver's passband", () => {
  // 10 bins over 1000 Hz centred on 10 kHz: bin i covers 9500 + 100 i.
  const px = new Uint8Array([0, 10, 20, 30, 200, 40, 50, 60, 70, 80]);
  const f = { offsetDb: -120, scaleDb: 0.5, sourceStart: 0n, centerHz: 10_000, spanHz: 1000, px };
  assert.equal(levelInBand(f, 9_900, 10_000), -120 + 200 * 0.5);
  assert.equal(levelInBand(f, 9_500, 9_700), -120 + 10 * 0.5);
  assert.equal(levelInBand(f, 10_150, 10_250), -120 + 60 * 0.5);
  assert.equal(levelInBand(f, 20_000, 21_000), null);
  assert.equal(levelInBand(f, 1_000, 2_000), null);
});

// ---- Fix round 1 -----------------------------------------------------------------

test("frequency entry takes a comma and refuses a unitless value it would have to guess", () => {
  assert.equal(parseFrequency("146,52"), 146_520_000);
  assert.equal(parseFrequency("999"), 999_000_000);
  assert.deepEqual(readFrequency("14070"), { error: '"14070": add a unit (k, M, G)' });
  assert.deepEqual(readFrequency("1000"), { error: '"1000": add a unit (k, M, G)' });
  assert.equal(parseFrequency("100000"), 100_000);
  assert.equal(parseFrequency("14070k"), 14_070_000);
  assert.deepEqual(readFrequency("abc"), { error: '"abc" is not a frequency' });
});

test("a removal's id is forgotten once the reply lands, so a reused id's removal is reported", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.remove(1);
  r.link.reply({ vrx: 1 });
  await flush();
  // The engine's event for that removal never came. Later id 1 is reused.
  r.ctl.longPress(162_550_000);
  r.link.reply(status(1, 162_550_000));
  await flush();
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "retuned away" });
  assert.deepEqual(r.toasts, ["Receiver at 162.5500 MHz removed: retuned away"]);
});

test("a restart forgets removals in flight", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.remove(1);
  // The socket dropped before the reply; the session was rebuilt.
  r.link.refuse("connection lost");
  r.link.emit("link", { event: "link", state: "up", detail: "", recreated: [], lost: [] });
  r.ctl.longPress(100_000_000);
  r.link.reply(status(1, 100_000_000));
  await flush();
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "gone" });
  assert.equal(r.toasts[r.toasts.length - 1], "Receiver at 100.0000 MHz removed: gone");
});

test("a removal event before the reply is not news, and a restart in between clears it", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.remove(1);
  r.link.emit("link", { event: "link", state: "up", detail: "", recreated: [{ old: 1, new: 1 }], lost: [] });
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "x" });
  assert.deepEqual(r.toasts, ["Receiver at 146.5200 MHz removed: x"]);
});

test("taps are ignored while the active receiver is being replaced", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.setMode("am");
  const before = r.link.sent.length;
  r.ctl.tap(147_000_000);
  assert.equal(r.link.sent.length, before);
  r.link.reply(status(2, 146_520_000, "am", { replaced: 1 }));
  await flush();
  r.ctl.tap(147_000_000);
  assert.deepEqual(r.link.last().args, { vrx: 2, hz: 147_000_000 });
});

test("setCenter shows only a refusal; dropped receivers report themselves", async () => {
  const link = new FakeLink();
  const toasts: string[] = [];
  const ok = setCenter({ link: new ReplaceGate(link), toast: (t) => toasts.push(t) }, 100_000_000);
  link.reply({ grantedHz: 100_000_000, removed: [1, 2] });
  assert.equal(await ok, true);
  assert.deepEqual(toasts, []);
  const bad = setCenter({ link: new ReplaceGate(link), toast: (t) => toasts.push(t) }, 1);
  link.refuse("set_source_center: out of range");
  assert.equal(await bad, false);
  assert.deepEqual(toasts, ["set_source_center: out of range"]);
});

test("gain element ids keep to safe characters", () => {
  assert.equal(gainId("IF 1"), "rp-gain-IF_1");
  assert.equal(gainId('L"NA'), "rp-gain-L_NA");
});

test("the sheet hint points at the chips when receivers exist but none is active", () => {
  assert.equal(sheetHint(true), "Tap a receiver to listen");
  assert.match(sheetHint(false), /Tap the waterfall/);
});

test("the receiver-events chip clears on a rebuilt session or a fresh one", () => {
  const s = new StatusModel();
  s.ready(true);
  s.link("up");
  s.eventsEnded("fell behind");
  s.link("up", "", false);
  assert.equal(s.chips().length, 1);
  s.link("up", "", true);
  assert.deepEqual(s.chips(), []);
  s.eventsEnded("fell behind");
  s.ready(true);
  assert.equal(s.chips().length, 1);
  s.ready(false);
  assert.deepEqual(s.chips(), []);
});

// ---- Speech per receiver and id changes ----------------------------------------

test("the speech choice goes to setVrxTranscribe for the active receiver and the reply is kept", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000, "nfm", { transcribe: "auto", transcribing: false }));
  await flush();
  assert.equal(r.ctl.list.get(1)!.transcribe, "auto");
  r.ctl.setTranscribe("on");
  assert.deepEqual(r.link.last(), { op: "setVrxTranscribe", args: { vrx: 1, choice: "on" }, timeoutMs: undefined });
  r.link.reply(status(1, 146_520_000, "nfm", { transcribe: "on", transcribing: true }));
  await flush();
  assert.equal(r.ctl.list.get(1)!.transcribe, "on");
  assert.equal(r.ctl.list.get(1)!.transcribing, true);
});

test("a refused speech choice toasts the engine's sentence and keeps the old one", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  r.ctl.setTranscribe("off");
  r.link.refuse("transcription is not available");
  await flush();
  assert.deepEqual(r.toasts, ["transcription is not available"]);
  assert.equal(r.ctl.list.get(1)!.transcribe, "auto");
});

test("a replace carries the speech choice to the new id and tells remap listeners", async () => {
  const r = rig();
  const moves: { old: number; new: number }[][] = [];
  r.ctl.onRemap((p) => moves.push(p));
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000, "nfm", { transcribe: "on" }));
  await flush();
  r.ctl.setMode("am");
  r.link.reply(status(2, 146_520_000, "am", { replaced: 1 }));
  await flush();
  assert.deepEqual(moves, [[{ old: 1, new: 2 }]]);
  assert.equal(r.ctl.list.get(2)!.transcribe, "on");
});

test("an engine restart's id map reaches remap listeners once", async () => {
  const r = rig();
  const moves: { old: number; new: number }[][] = [];
  r.ctl.onRemap((p) => moves.push(p));
  r.link.emit("link", { event: "link", state: "up", detail: "", recreated: [{ old: 1, new: 5 }], lost: [] });
  r.link.emit("link", { event: "link", state: "up", detail: "", recreated: [], lost: [] });
  assert.deepEqual(moves, [[{ old: 1, new: 5 }]]);
});

test("gone listeners hear of own removals, engine removals and lost receivers", async () => {
  const r = rig();
  const gone: number[] = [];
  r.ctl.onGone((v) => gone.push(v));
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  r.ctl.longPress(162_550_000);
  r.link.reply(status(2, 162_550_000));
  await flush();
  r.ctl.remove(2);
  r.link.reply({ vrx: 2 });
  await flush();
  assert.deepEqual(gone, [2]);
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "retuned" });
  assert.deepEqual(gone, [2, 1]);
  r.link.emit("link", { event: "link", state: "up", detail: "", recreated: [], lost: [{ vrx: 7, reason: "gone" }] });
  assert.deepEqual(gone, [2, 1, 7]);
});

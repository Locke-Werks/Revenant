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
import type { Detection } from "../src/detections";
import {
  CONFIDENCE_MAX,
  CONFIDENCE_MIN,
  CONFIDENCE_SEND_MS,
  heardAgo,
  modeForDetection,
  pickSignal,
  tapWaterfall,
  receiverCovering,
  tuneForDetection,
  readConfidence,
  RowSet,
  RateLimitedSender,
  signalRows,
  type PickDeps,
} from "../src/ui/signals";
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

function rig(ranges: { lowHz: number; highHz: number }[] | null = null) {
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
    tuneRanges: () => ranges,
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
  assert.deepEqual(r.link.last(), { op: "addVrx", args: { hz: 146_520_000, mode: DEFAULT_MODE, bandwidthHz: 0 }, timeoutMs: undefined });
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
  // The removal event that follows is not news, and drops the lead without
  // turning the phone's audio off.
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "removed" });
  assert.deepEqual(r.toasts, []);
  assert.equal(r.audio.vrx, null);
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

test("problems are chips, the stopping one first and marked", () => {
  const s = new StatusModel();
  s.eventsEnded("restart");
  const p = s.problems();
  assert.deepEqual(p[0], { text: "connecting", stops: true });
  assert.equal(p.length, 2);
  assert.equal(p[1]!.stops, false);
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

test("a flushed drag tune reaches the receiver it names after the active one changed", async () => {
  const r = await dragRig();
  r.ctl.tap(162_550_000); // active still 1: tunes it
  r.link.reply(status(1, 162_550_000));
  await flush();
  r.ctl.tuneDragged(146_700_000, 1);
  assert.deepEqual(r.link.last(), { op: "tune", args: { vrx: 1, hz: 146_700_000 }, timeoutMs: undefined });
});

test("a drag tune for a receiver that is gone is not sent", async () => {
  const r = await dragRig();
  const n = r.link.sent.length;
  r.ctl.tuneDragged(146_700_000, 9);
  assert.equal(r.link.sent.length, n);
});

// ---- Signals tab ---------------------------------------------------------------

function det(id: string, snrDb: number, extra: Partial<Detection> = {}): Detection {
  return { id, centerHz: 146_520_000, bandwidthHz: 8_000, snrDb, confidence: 0.9, state: "live", label: "NFM", labelKind: "modulation", labelConfidence: 0.8, lastSeen: 0, ...extra };
}

test("signal rows list only detections with a modulation or protocol", () => {
  const rows = signalRows([
    det("1", 5),
    det("2", 5, { label: "", labelKind: "" }),
    det("3", 5, { label: "P25", labelKind: "protocol" }),
    det("4", 5, { label: "  " }),
  ]);
  assert.deepEqual(rows.map((d) => d.id), ["1", "3"]);
});

test("signal rows run lowest frequency first, ties by id", () => {
  const rows = signalRows([
    det("10", 5),
    det("9", 5),
    det("3", 20, { centerHz: 146_600_000 }),
    det("4", 1, { centerHz: 100_000_000 }),
  ]);
  assert.deepEqual(rows.map((d) => d.id), ["4", "9", "10", "3"]);
});

// A detection at a given MHz, so row order in the tests below reads off the
// frequencies.
const atMHz = (id: string, mhz: number, snrDb = 10) => det(id, snrDb, { centerHz: mhz * 1e6 });

test("time since heard is read from sample indices at the source rate", () => {
  assert.equal(heardAgo(1_000_000, 1_000_000, 2_400_000), "now");
  assert.equal(heardAgo(0, 2_400_000 * 5, 2_400_000), "5 s ago");
  assert.equal(heardAgo(0, 2_400_000 * 125, 2_400_000), "2 min ago");
  assert.equal(heardAgo(0, 2_400_000 * 7_300, 2_400_000), "2 h ago");
});

test("time since heard is blank without a clock, and never negative", () => {
  assert.equal(heardAgo(10, null, 2_400_000), "");
  assert.equal(heardAgo(10, 100, 0), "");
  assert.equal(heardAgo(5_000_000, 1_000_000, 2_400_000), "now");
});

type Rx = { vrx: number; hz: number; bandwidthHz: number | null };
function pickRig(rs: Rx[] = []) {
  const calls: string[] = [];
  const deps: PickDeps = {
    receivers: () => rs,
    select: (vrx) => calls.push(`select ${vrx}`),
    spawn: (hz, mode, bw, dec) => calls.push(`spawn ${hz} ${mode} ${bw}${dec ? " " + dec : ""}`),
    stopDial: () => calls.push("stop"),
    reveal: (hz) => calls.push(`reveal ${hz}`),
  };
  return { calls, deps };
}

test("picking a signal spawns a receiver in its mode at the fitted width and reveals it", () => {
  const { calls, deps } = pickRig([{ vrx: 1, hz: 100_000_000, bandwidthHz: 200_000 }]);
  pickSignal(det("1", 10, { centerHz: 162_550_000, bandwidthHz: 9_000, label: "P25", labelKind: "protocol" }), deps);
  assert.deepEqual(calls, ["stop", "spawn 162550000 p25p1 0", "reveal 162550000"]);
});

test("tapping a lost row spawns on its last centre, mode and width", () => {
  const { calls, deps } = pickRig();
  pickSignal(
    det("1", 10, { centerHz: 162_550_000, bandwidthHz: 9_000, label: "NFM", labelKind: "modulation", lostForMs: 42_000 } as Partial<Detection>),
    deps,
  );
  assert.deepEqual(calls, ["stop", "spawn 162550000 nfm 10000", "reveal 162550000"]);
});

test("tapping a lost row selects a receiver already parked on it", () => {
  const { calls, deps } = pickRig([{ vrx: 3, hz: 162_551_000, bandwidthHz: 12_500 }]);
  pickSignal(det("1", 10, { centerHz: 162_550_000, bandwidthHz: 9_000, lostForMs: 5_000 } as Partial<Detection>), deps);
  assert.deepEqual(calls, ["stop", "select 3", "reveal 162550000"]);
});

test("a waterfall tap in a detection box picks it, a miss retunes", () => {
  const { calls, deps } = pickRig();
  const d = det("1", 10, { centerHz: 146_520_000, bandwidthHz: 8_000, label: "NFM", labelKind: "modulation" });
  tapWaterfall({ detection: d }, deps, () => calls.push("retune"));
  assert.deepEqual(calls, ["stop", "spawn 146520000 nfm 10000", "reveal 146520000"]);
  calls.length = 0;
  tapWaterfall(null, deps, () => calls.push("retune"));
  tapWaterfall({}, deps, () => calls.push("retune"));
  assert.deepEqual(calls, ["retune", "retune"]);
});

test("an analogue FM pick is fitted to the shared width list", () => {
  const { calls, deps } = pickRig();
  pickSignal(det("1", 10, { bandwidthHz: 9_000, label: "NFM", labelKind: "modulation" }), deps);
  assert.equal(calls[1], "spawn 146520000 nfm 10000");
});

test("a pick names the desktop's decoder, except one a DV mode brings itself", () => {
  const t = (label: string, bandwidthHz = 12_000) => tuneForDetection({ label, labelKind: "protocol", bandwidthHz });
  assert.deepEqual(t("POCSAG"), { mode: "nfm", decoder: "pocsag", bandwidthHz: 12_500 });
  assert.deepEqual(t("AX.25"), { mode: "nfm", decoder: "ax25", bandwidthHz: 12_500 });
  assert.deepEqual(t("RTTY", 400), { mode: "usb", decoder: "rtty", bandwidthHz: 0 });
  assert.deepEqual(t("PSK31", 100), { mode: "usb", decoder: "psk31", bandwidthHz: 0 });
  assert.deepEqual(t("CW", 200), { mode: "cw", decoder: "cw", bandwidthHz: 0 });
  assert.deepEqual(t("M17"), { mode: "p25p1", decoder: "m17", bandwidthHz: 0 });
  assert.deepEqual(t("RDS", 150_000), { mode: "wfm", decoder: "", bandwidthHz: 200_000 });
  for (const dv of ["P25", "DMR", "D-STAR", "TETRA"]) assert.equal(t(dv).decoder, "", dv);
});

test("only nfm, am, wfm and raw are fitted; every other mode asks for its default", () => {
  const m = (label: string, bandwidthHz: number) => tuneForDetection({ label, labelKind: "modulation", bandwidthHz }).bandwidthHz;
  assert.equal(m("AM", 7_000), 10_000);
  assert.equal(m("WFM", 150_000), 200_000);
  assert.equal(m("USB", 2_800), 0);
  assert.equal(m("LSB", 2_800), 0);
  assert.equal(m("CW", 300), 0);
  assert.equal(m("BPSK", 2_000), 0);
});

test("a spawn adds in mode and width, then attaches the decoder", async () => {
  const r = rig();
  r.ctl.spawn(7_040_000, "usb", 0, "rtty");
  assert.deepEqual(r.link.last(), { op: "addVrx", args: { hz: 7_040_000, mode: "usb", bandwidthHz: 0 }, timeoutMs: undefined });
  r.link.reply(status(3, 7_040_000, "usb"));
  await flush();
  assert.equal(r.ctl.list.active, 3);
  assert.deepEqual(r.link.last(), { op: "setDecoders", args: { vrx: 3, names: ["rtty"] }, timeoutMs: undefined });
});

test("a refused spawn decoder says nothing", async () => {
  const r = rig();
  r.ctl.spawn(7_040_000, "usb", 0, "rtty");
  r.link.reply(status(3, 7_040_000, "usb"));
  await flush();
  r.link.refuse("unknown decoder");
  await flush();
  assert.deepEqual(r.toasts, []);
});

test("a spawn with no decoder, or one the receiver already has, sends no setDecoders", async () => {
  const r = rig();
  r.ctl.spawn(146_520_000, "nfm", 12_500);
  r.link.reply(status(1, 146_520_000));
  await flush();
  r.ctl.spawn(851_000_000, "p25p1", 0, "p25p1");
  r.link.reply(status(2, 851_000_000, "p25p1", { decoders: ["p25p1"] }));
  await flush();
  assert.deepEqual(r.link.sent.map((s) => s.op).filter((op) => op === "setDecoders"), []);
});

test("a signal wider than every choice gets the widest", () => {
  const { calls, deps } = pickRig();
  pickSignal(det("1", 10, { bandwidthHz: 900_000, label: "", labelKind: "" }), deps);
  assert.equal(calls[1], "spawn 146520000 wfm 200000");
});

test("picking a signal a receiver already covers selects that receiver", () => {
  const { calls, deps } = pickRig([{ vrx: 4, hz: 146_525_000, bandwidthHz: 12_500 }]);
  pickSignal(det("1", 10), deps);
  assert.deepEqual(calls, ["stop", "select 4", "reveal 146520000"]);
});

test("receiverCovering takes the nearest passband holding the centre", () => {
  const rs: Rx[] = [
    { vrx: 1, hz: 146_500_000, bandwidthHz: 50_000 },
    { vrx: 2, hz: 146_521_000, bandwidthHz: 12_500 },
    { vrx: 3, hz: 146_520_000, bandwidthHz: null },
  ];
  assert.equal(receiverCovering(rs, 146_520_000), 2);
  assert.equal(receiverCovering(rs, 146_510_000), 1);
  assert.equal(receiverCovering(rs, 147_000_000), null);
  assert.equal(receiverCovering([], 1), null);
});

test("modeForDetection follows the desktop's label table", () => {
  const m = (label: string, labelKind: string, bandwidthHz = 12_000) => modeForDetection({ label, labelKind, bandwidthHz });
  assert.equal(m("WFM", "modulation", 150_000), "wfm");
  assert.equal(m("NFM", "modulation"), "nfm");
  assert.equal(m("AM", "modulation"), "am");
  assert.equal(m("USB", "modulation"), "usb");
  assert.equal(m("LSB", "modulation"), "lsb");
  assert.equal(m("CW", "modulation"), "cw");
  assert.equal(m("P25", "protocol"), "p25p1");
  assert.equal(m("P25 control", "protocol"), "p25p1");
  assert.equal(m("DMR", "protocol"), "dmr");
  assert.equal(m("D-STAR", "protocol"), "dstar");
  assert.equal(m("TETRA", "protocol"), "tetra");
  assert.equal(m("POCSAG", "protocol"), "nfm");
  assert.equal(m("RTTY", "protocol"), "usb");
});

test("families and unlabelled signals fall to the width rule, narrow families to usb", () => {
  const m = (label: string, labelKind: string, bandwidthHz: number) => modeForDetection({ label, labelKind, bandwidthHz });
  assert.equal(m("2FSK", "modulation", 12_000), "nfm");
  assert.equal(m("8PSK", "modulation", 60_000), "wfm");
  assert.equal(m("BPSK", "modulation", 2_000), "usb");
  assert.equal(m("", "", 9_000), "nfm");
  assert.equal(m("", "", 180_000), "wfm");
  assert.equal(m("", "", 0), "nfm");
  assert.equal(m("Unheard", "protocol", 9_000), "nfm");
});


test("tuneFit tunes the active receiver, then fits the width when it differs", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  r.ctl.tuneFit(162_550_000, 25_000);
  assert.deepEqual(r.link.last(), { op: "tune", args: { vrx: 1, hz: 162_550_000 }, timeoutMs: undefined });
  const n = r.link.sent.length;
  r.link.reply(status(1, 162_550_000));
  await flush();
  assert.equal(r.link.sent.length, n + 1);
  assert.deepEqual(r.link.last(), { op: "setBandwidth", args: { vrx: 1, hz: 25_000 }, timeoutMs: undefined });
});

test("tuneFit leaves the width alone when the receiver already has it", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000));
  await flush();
  const n = r.link.sent.length;
  r.ctl.tuneFit(162_550_000, 12_500);
  r.link.reply(status(1, 162_550_000));
  await flush();
  assert.equal(r.link.sent.length, n + 1);
});

test("tuneFit with no receiver sends nothing", () => {
  const r = rig();
  r.ctl.tuneFit(162_550_000, 25_000);
  assert.equal(r.link.sent.length, 0);
});

function senderRig() {
  let t = 1000;
  const sent: number[] = [];
  let pending: { fn: () => void; at: number } | null = null;
  const s = new RateLimitedSender(
    (v) => sent.push(v),
    () => t,
    (fn, ms) => {
      const p = { fn, at: t + ms };
      pending = p;
      return () => {
        if (pending === p) pending = null;
      };
    },
  );
  const advance = (ms: number) => {
    t += ms;
    if (pending && pending.at <= t) {
      const p = pending;
      pending = null;
      p.fn();
    }
  };
  return { s, sent, advance };
}

test("the confidence slider sends once per 250 ms and ends on the last value", () => {
  assert.equal(CONFIDENCE_SEND_MS, 250);
  const { s, sent, advance } = senderRig();
  s.set(0.1);
  s.set(0.2);
  s.set(0.3);
  assert.deepEqual(sent, [0.1]);
  advance(249);
  assert.deepEqual(sent, [0.1]);
  advance(1);
  assert.deepEqual(sent, [0.1, 0.3]);
  advance(300);
  assert.deepEqual(sent, [0.1, 0.3]);
});

test("the slider range stays inside what the gateway accepts", () => {
  assert.equal(CONFIDENCE_MIN, 0);
  assert.equal(CONFIDENCE_MAX, 0.99);
});

test("a stored confidence is clamped and a bad one reads as zero", () => {
  assert.equal(readConfidence("0.35"), 0.35);
  assert.equal(readConfidence("5"), CONFIDENCE_MAX);
  assert.equal(readConfidence("-1"), 0);
  assert.equal(readConfidence("abc"), 0);
  assert.equal(readConfidence(null), 0);
});

// ---- Row reconciliation --------------------------------------------------------

const text = (d: Detection) => `${d.id}:${d.snrDb}`;

test("first apply adds every row in list order", () => {
  const s = new RowSet();
  const p = s.apply([atMHz("1", 146.6, 5), atMHz("2", 146.5, 20)], text);
  assert.deepEqual(p.added, ["2", "1"]);
  assert.deepEqual(p.order, ["2", "1"]);
  assert.deepEqual(p.removed, []);
  assert.deepEqual(p.updated, []);
});

test("a later apply reports added, removed and text-changed rows only", () => {
  const s = new RowSet();
  s.apply([atMHz("1", 146.5, 5), atMHz("2", 146.4, 20), atMHz("3", 146.7, 1)], text);
  const p = s.apply([atMHz("1", 146.5, 6), atMHz("2", 146.4, 20), atMHz("4", 146.6, 3)], text);
  assert.deepEqual(p.added, ["4"]);
  assert.deepEqual(p.removed, ["3"]);
  assert.deepEqual(p.updated, ["1"]);
  assert.deepEqual(p.order, ["2", "1", "4"]);
});

test("order is held while a pointer is down and applied on release", () => {
  const s = new RowSet();
  s.apply([atMHz("1", 146.3, 20), atMHz("2", 146.2, 10)], text);
  s.hold(true);
  const p = s.apply([atMHz("1", 146.3, 5), atMHz("2", 146.2, 10), atMHz("3", 146.1, 30)], text);
  assert.deepEqual(p.order, ["2", "1", "3"]);
  assert.deepEqual(p.added, ["3"]);
  assert.deepEqual(p.updated, ["1"]);
  assert.deepEqual(s.hold(false), ["3", "2", "1"]);
});

test("releasing with nothing pending reports no reorder", () => {
  const s = new RowSet();
  s.apply([det("1", 20)], text);
  s.hold(true);
  assert.equal(s.hold(false), null);
});

test("a row removed during a hold leaves at once", () => {
  const s = new RowSet();
  s.apply([det("1", 20), det("2", 10)], text);
  s.hold(true);
  const p = s.apply([det("2", 10)], text);
  assert.deepEqual(p.removed, ["1"]);
  assert.deepEqual(p.order, ["2"]);
});

// ---- The band edge -----------------------------------------------------------

const FM_BAND = { open: true, centerHz: 98_100_000, rateHz: 2_400_000, epoch: 1 };

async function edgeRig(ranges: { lowHz: number; highHz: number }[] | null = null) {
  const r = rig(ranges);
  r.ctl.setSource(FM_BAND);
  r.ctl.tap(99_200_000);
  r.link.reply(status(1, 99_200_000));
  await flush();
  return r;
}

test("a drag past the band's edge recentres the radio, holding tunes until it answers", async () => {
  const r = await edgeRig();
  r.ctl.preview(99_310_000);
  r.ctl.tuneDragged(99_310_000);
  // 25% in from the trailing edge: centre 0.6 MHz above the receiver.
  assert.deepEqual(r.link.last(), { op: "setCenter", args: { hz: 99_910_000 }, timeoutMs: undefined });
  r.ctl.tuneDragged(99_400_000);
  assert.equal(r.link.sent.length, 2);
  r.link.reply({ grantedHz: 99_910_000, removed: [] });
  await flush();
  assert.deepEqual(r.link.last(), { op: "tune", args: { vrx: 1, hz: 99_400_000 }, timeoutMs: undefined });
});

test("after a recentre the next drag steps have room and move no radio", async () => {
  const r = await edgeRig();
  r.ctl.tuneDragged(99_310_000);
  r.link.reply({ grantedHz: 99_910_000, removed: [] });
  await flush();
  r.link.reply(status(1, 99_310_000));
  await flush();
  r.ctl.tuneDragged(99_500_000);
  r.ctl.tuneDragged(100_500_000);
  assert.deepEqual(r.link.sent.map((s) => s.op), ["addVrx", "setCenter", "tune", "tune", "tune"]);
});

test("a drag whose receiver the recentre removed sends nothing more and ends cleanly", async () => {
  const r = await edgeRig();
  r.ctl.preview(99_310_000);
  r.ctl.tuneDragged(99_310_000);
  r.link.reply({ grantedHz: 99_910_000, removed: [1] });
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 1, reason: "outside the new span" });
  await flush();
  r.ctl.tuneDragged(99_400_000);
  r.ctl.endDrag();
  assert.deepEqual(r.link.sent.map((s) => s.op), ["addVrx", "setCenter"]);
});

test("picking a signal outside the band recentres before tuning", async () => {
  const r = await edgeRig();
  r.ctl.tuneFit(99_400_000, 12_500);
  assert.equal(r.link.last().op, "setCenter");
  r.link.reply({ grantedHz: 100_000_000, removed: [] });
  await flush();
  assert.deepEqual(r.link.last(), { op: "tune", args: { vrx: 1, hz: 99_400_000 }, timeoutMs: undefined });
});

test("a refused recentre clamps at the edge from then on", async () => {
  const r = await edgeRig();
  r.ctl.tuneDragged(99_310_000);
  r.link.refuse("set_source_center: no");
  await flush();
  assert.deepEqual(r.link.last(), { op: "tune", args: { vrx: 1, hz: 99_293_750 }, timeoutMs: undefined });
  assert.equal(r.ctl.preview(99_500_000)!.hz, 99_293_750);
});

test("a source that cannot be retuned clamps without asking", async () => {
  const r = await edgeRig([]);
  r.ctl.tuneDragged(99_310_000);
  assert.deepEqual(r.link.last(), { op: "tune", args: { vrx: 1, hz: 99_293_750 }, timeoutMs: undefined });
});

test("typed entry far outside the band recentres, and re-adds a receiver the move dropped", async () => {
  const r = await edgeRig();
  r.ctl.tap(150_000_000);
  assert.equal(r.link.last().op, "setCenter");
  r.link.reply({ grantedHz: Number(r.link.last().args!.hz), removed: [1] });
  await flush();
  assert.deepEqual(r.link.last(), { op: "addVrx", args: { hz: 150_000_000, mode: "nfm", bandwidthHz: 0 }, timeoutMs: undefined });
});

test("a refused tune puts the chip back where the engine last confirmed", async () => {
  const r = await edgeRig();
  r.ctl.tap(99_000_000);
  r.link.refuse("place: centre -1515153 Hz is outside the source's baseband span");
  await flush();
  assert.deepEqual(r.chips(), ["1:99200000:nfm"]);
  assert.equal(r.toasts.length, 1);
});

test("a refused drag tune rolls the preview back when the drag ends", async () => {
  const r = await edgeRig();
  r.ctl.preview(99_100_000);
  r.ctl.tuneDragged(99_100_000);
  r.link.refuse("nope");
  await flush();
  assert.deepEqual(r.chips(), ["1:99100000:nfm"]);
  r.ctl.endDrag();
  assert.deepEqual(r.chips(), ["1:99200000:nfm"]);
});

test("a receiver added at the mode's default width reads back as that default", async () => {
  const r = rig();
  r.ctl.tap(98_100_000);
  r.link.reply(status(1, 98_100_000, "wfm", { bandwidthHz: 0 }));
  await flush();
  assert.equal(r.ctl.list.get(1)!.bandwidthHz, null);
});

// ---- Decoders ------------------------------------------------------------------

test("a broadcast FM receiver asks for no decoder: the gateway reads its RDS", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.setMode("wfm");
  r.link.reply(status(2, 98_100_000, "wfm", { replaced: 1, decoders: [] }));
  await flush();
  assert.ok(r.link.last().op !== "setDecoders");
});

test("setDecoders sends the whole set and a refusal is a short toast", async () => {
  const r = rig();
  await listening(r, 1);
  r.ctl.setDecoders(["pocsag"]);
  assert.deepEqual(r.link.last().args, { vrx: 1, names: ["pocsag"] });
  r.link.refuse("names: p25p1 does not read a nfm receiver (it reads p25p1, raw)");
  await flush();
  assert.deepEqual(r.toasts, ["Decoder refused: p25p1 does not read a nfm receiver (it reads p25p1, raw)"]);
  assert.deepEqual(r.ctl.list.get(1)!.decoders, []);
});

test("a receiver reply carries its decoders onto the chip", async () => {
  const r = rig();
  r.ctl.tap(146_520_000);
  r.link.reply(status(1, 146_520_000, "p25p1", { decoders: ["p25p1"] }));
  await flush();
  assert.deepEqual(r.ctl.list.get(1)!.decoders, ["p25p1"]);
});

test("a decoder the engine ended is reported by name", async () => {
  const r = rig();
  await listening(r, 1);
  r.link.emit("vrxStatus", { event: "vrxStatus", vrx: 1, decoder: "pocsag", decoderError: "the decoder refused 8000 S/s. It needs 22050." });
  assert.deepEqual(r.toasts, ["pocsag: the decoder refused 8000 S/s"]);
});

test("every receiver the engine has is listed, whoever opened it", async () => {
  const r = rig();
  r.link.emit("vrx", { event: "vrx", vrx: 7, hz: 162_550_000, mode: "nfm", bandwidthHz: 12_500, squelchDbfs: -80, openedBy: "another client" });
  r.link.emit("vrx", { event: "vrx", vrx: 3, hz: 853_050_100, mode: "p25p1", bandwidthHz: 12_500, openedBy: "trunk follower" });
  assert.deepEqual(r.chips(), ["3:853050100:p25p1", "7:162550000:nfm"]);
  // A change the engine reports moves the chip.
  r.link.emit("vrx", { event: "vrx", vrx: 7, hz: 162_400_000, mode: "nfm", openedBy: "another client" });
  assert.deepEqual(r.chips(), ["3:853050100:p25p1", "7:162400000:nfm"]);
  r.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 7, reason: "removed by another client" });
  assert.deepEqual(r.chips(), ["3:853050100:p25p1"]);
});

test("a vrx event cannot move the previewed frequency mid-drag", async () => {
  const r = await dragRig();
  r.ctl.preview(146_700_000);
  r.link.emit("vrx", { event: "vrx", vrx: 1, hz: 146_520_000, mode: "nfm" });
  assert.deepEqual(r.chips(), ["1:146700000:nfm"]);
});
test("a fresh session's snapshot replaces the list, so a receiver removed meanwhile goes", async () => {
  const r = rig();
  r.link.emit("vrx", { event: "vrx", vrx: 5, hz: 146_520_000, mode: "nfm" });
  r.link.emit("vrx", { event: "vrx", vrx: 6, hz: 162_550_000, mode: "nfm" });
  r.ctl.list.select(6);
  // Taken over, then taken back: the new session lists only what exists now.
  r.link.emit("lease", { event: "lease", state: "lost", holder: "Revenant desktop on VALKYRIE" });
  r.link.emit("vrxList", { event: "vrxList", items: [{ vrx: 6, hz: 162_550_000, mode: "nfm" }] });
  assert.deepEqual(r.chips(), ["6:162550000:nfm"]);
  assert.equal(r.ctl.list.active, 6);
  r.link.emit("vrxList", { event: "vrxList", items: [] });
  assert.deepEqual(r.chips(), []);
  assert.equal(r.ctl.list.active, null);
});

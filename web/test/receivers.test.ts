import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DEFAULT_MODE,
  formatMHz,
  parseFrequency,
  ReceiverController,
  ReplaceGate,
  type ControllerAudio,
  type ControllerKeep,
  type GateLink,
} from "../src/ui/receivers";
import { BEHIND_BELOW, sourceBehind, StatusModel } from "../src/ui/status";
import { OPEN_SOURCE_TIMEOUT_MS } from "../src/ui/radio_panel";
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

import { test } from "node:test";
import assert from "node:assert/strict";
import { LiveAudio, type AudioContextLike, type AudioSink, type LiveAudioDeps, type LiveLink } from "../src/audio/live";
import type { PcmDecoder } from "../src/audio/opus_decoder";
import type { WorkletState } from "../src/audio/messages";
import type { JsonValue, OpusFrame } from "../src/protocol";

class FakeLink implements LiveLink {
  requests: { op: string; args: unknown }[] = [];
  fail: string | null = null;
  private fns = new Map<string, ((p: never) => void)[]>();
  request(op: string, args?: { [key: string]: JsonValue }): Promise<JsonValue> {
    this.requests.push({ op, args });
    if (this.fail) return Promise.reject(new Error(this.fail));
    return Promise.resolve({ vrx: args?.vrx ?? null, on: args?.on ?? true });
  }
  on(name: string, fn: (p: never) => void): () => void {
    const l = this.fns.get(name) ?? [];
    l.push(fn);
    this.fns.set(name, l);
    return () => {};
  }
  emit(name: string, payload: unknown): void {
    for (const fn of this.fns.get(name) ?? []) (fn as (p: unknown) => void)(payload);
  }
}

class FakeContext implements AudioContextLike {
  state = "suspended";
  sampleRate = 48_000;
  resumes = 0;
  // What a resume() call leaves the context in; iOS can refuse.
  resumeTo = "running";
  private fns: (() => void)[] = [];
  resume(): Promise<void> {
    this.resumes++;
    this.set(this.resumeTo);
    return Promise.resolve();
  }
  onStateChange(fn: () => void): void {
    this.fns.push(fn);
  }
  set(s: string): void {
    if (s === this.state) return;
    this.state = s;
    for (const fn of this.fns) fn();
  }
}

class FakeSink implements AudioSink {
  pushed: number[] = [];
  resets = 0;
  private fn: ((s: WorkletState) => void) | null = null;
  push(seq: number, _pcm: Float32Array): void {
    this.pushed.push(seq);
  }
  reset(): void {
    this.resets++;
  }
  onState(fn: (s: WorkletState) => void): void {
    this.fn = fn;
  }
  report(playing: boolean): void {
    this.fn?.({ t: "state", playing, targetMs: 80, bufferedMs: 80, underruns: 0, late: 0, concealed: 0, resets: 0, trimmed: 0 });
  }
}

function setup() {
  const link = new FakeLink();
  const ctx = new FakeContext();
  const sink = new FakeSink();
  let decoderResets = 0;
  let contexts = 0;
  const decoder: PcmDecoder = {
    // A packet whose first byte is 0 stands for one libopus refuses.
    decode: (p) => (p[0] === 0 ? null : new Float32Array(960)),
    reset: () => {
      decoderResets++;
      return Promise.resolve();
    },
    free: () => {},
  };
  const deps: LiveAudioDeps = {
    link,
    context: () => {
      contexts++;
      return ctx;
    },
    sink: () => Promise.resolve(sink),
    decoder: () => Promise.resolve(decoder),
  };
  const audio = new LiveAudio(deps);
  const frame = (vrx: number, seq: number, first = 1): OpusFrame => ({ vrx: BigInt(vrx), seq, packet: new Uint8Array([first, 2, 3]) });
  return { link, ctx, sink, audio, frame, decoderResets: () => decoderResets, contexts: () => contexts };
}

test("live: start resumes the context inside the gesture, before anything awaits", async () => {
  const { audio, ctx, link, contexts } = setup();
  const p = audio.start(7);
  // Synchronously: iOS only honours a resume made in the gesture's own turn.
  assert.equal(contexts(), 1);
  assert.equal(ctx.resumes, 1);
  await p;
  assert.deepEqual(link.requests, [{ op: "audio", args: { vrx: 7, on: true } }]);
  assert.equal(audio.vrx, 7);
});

test("live: frames for the active receiver are decoded and pushed, others ignored", async () => {
  const { audio, link, sink, frame } = setup();
  link.emit("opus", frame(7, 1));
  assert.deepEqual(sink.pushed, []);
  await audio.start(7);
  link.emit("opus", frame(7, 2));
  link.emit("opus", frame(9, 3));
  link.emit("opus", frame(7, 4));
  assert.deepEqual(sink.pushed, [2, 4]);
  assert.equal(audio.stats.ignored, 2);
});

test("live: a packet the decoder refuses is skipped and counted", async () => {
  const { audio, link, sink, frame } = setup();
  await audio.start(7);
  link.emit("opus", frame(7, 1, 0));
  link.emit("opus", frame(7, 2));
  assert.deepEqual(sink.pushed, [2]);
  assert.equal(audio.stats.undecodable, 1);
});

test("live: nothing is queued while the context is not running", async () => {
  const { audio, link, sink, ctx, frame } = setup();
  ctx.resumeTo = "suspended";
  await audio.start(7);
  link.emit("opus", frame(7, 1));
  assert.deepEqual(sink.pushed, []);
});

test("live: switching receivers resets the buffer and the decoder", async () => {
  const { audio, link, sink, frame, decoderResets } = setup();
  await audio.start(7);
  link.emit("opus", frame(7, 1));
  const before = sink.resets;
  await audio.start(8);
  assert.equal(sink.resets, before + 1);
  assert.equal(decoderResets(), 1);
  assert.deepEqual(link.requests.at(-1), { op: "audio", args: { vrx: 8, on: true } });
  link.emit("opus", frame(7, 2));
  link.emit("opus", frame(8, 3));
  assert.deepEqual(sink.pushed, [1, 3]);
});

test("live: stop turns the stream off and later frames are ignored", async () => {
  const { audio, link, sink, frame } = setup();
  await audio.start(7);
  audio.stop();
  assert.deepEqual(link.requests.at(-1), { op: "audio", args: { vrx: 7, on: false } });
  assert.equal(audio.vrx, null);
  link.emit("opus", frame(7, 1));
  assert.deepEqual(sink.pushed, []);
  assert.ok(sink.resets >= 1);
});

test("live: a refused audio op leaves nothing active and rejects", async () => {
  const { audio, link } = setup();
  link.fail = "vrx: receiver 7 is not one of yours";
  await assert.rejects(audio.start(7), /not one of yours/);
  assert.equal(audio.vrx, null);
});

test("live: resume reports whether the context is running", async () => {
  const { audio, ctx } = setup();
  assert.equal(await audio.resume(), false);
  await audio.start(7);
  assert.equal(await audio.resume(), true);
  // iOS: a call or Siri takes the audio session.
  ctx.resumeTo = "interrupted";
  ctx.set("interrupted");
  assert.equal(await audio.resume(), false);
  ctx.resumeTo = "running";
  assert.equal(await audio.resume(), true);
});

test("live: an interruption drops buffered audio and clears ready", async () => {
  const { audio, ctx, sink } = setup();
  await audio.start(7);
  sink.report(true);
  assert.equal(audio.ready, true);
  const before = sink.resets;
  ctx.set("interrupted");
  assert.equal(audio.ready, false);
  assert.equal(sink.resets, before + 1);
});

test("live: ready needs a running context and a playing worklet", async () => {
  const { audio, sink } = setup();
  assert.equal(audio.ready, false);
  await audio.start(7);
  assert.equal(audio.ready, false);
  sink.report(true);
  assert.equal(audio.ready, true);
  sink.report(false);
  assert.equal(audio.ready, false);
});

test("live: a context not at 48 kHz refuses to start", async () => {
  const { audio, ctx, link } = setup();
  ctx.sampleRate = 44_100;
  await assert.rejects(audio.start(7), /48000/);
  assert.equal(link.requests.length, 0);
  assert.equal(audio.vrx, null);
});

test("live: the audio op is sent again when the link comes back", async () => {
  const { audio, link, sink } = setup();
  await audio.start(7);
  const before = sink.resets;
  link.emit("ready", { resumed: false });
  assert.deepEqual(link.requests.at(-1), { op: "audio", args: { vrx: 7, on: true } });
  assert.equal(sink.resets, before + 1);
});

test("live: a recreated receiver keeps playing under its new id; a removed one stops", async () => {
  const { audio, link, sink, frame } = setup();
  await audio.start(7);
  link.emit("link", { event: "link", state: "up", detail: "", recreated: [{ old: 7, new: 12 }] });
  assert.equal(audio.vrx, 12);
  link.emit("opus", frame(12, 1));
  assert.deepEqual(sink.pushed, [1]);
  link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 12 });
  assert.equal(audio.vrx, null);
  link.emit("opus", frame(12, 2));
  assert.deepEqual(sink.pushed, [1]);
});

test("live: unlock creates and resumes the context synchronously, once", async () => {
  const { audio, ctx, contexts } = setup();
  void audio.unlock();
  assert.equal(contexts(), 1);
  assert.equal(ctx.resumes, 1);
  await audio.start(7);
  assert.equal(contexts(), 1);
  assert.equal(audio.vrx, 7);
});

test("live: a stop during start's audio op turns the stream off again", async () => {
  const { audio, link } = setup();
  const p = audio.start(7);
  // Wait until the on is in flight, then stop before its reply lands.
  while (link.requests.length === 0) await Promise.resolve();
  audio.stop();
  await p;
  assert.deepEqual(link.requests, [
    { op: "audio", args: { vrx: 7, on: true } },
    { op: "audio", args: { vrx: 7, on: false } },
  ]);
  assert.equal(audio.vrx, null);
});

test("live: a newer start during an older one's audio op sends no off", async () => {
  const { audio, link } = setup();
  const p = audio.start(7);
  while (link.requests.length === 0) await Promise.resolve();
  await audio.start(8);
  await p;
  assert.ok(!link.requests.some((r) => (r.args as { on?: boolean }).on === false));
  assert.equal(audio.vrx, 8);
});

test("live: retarget flushes the buffer and resets the decoder", async () => {
  const { audio, link, sink, frame, decoderResets } = setup();
  await audio.start(7);
  link.emit("opus", frame(7, 900));
  const before = sink.resets;
  audio.retarget(7, 12);
  assert.equal(sink.resets, before + 1);
  assert.equal(decoderResets(), 1);
  link.emit("opus", frame(12, 0));
  assert.deepEqual(sink.pushed, [900, 0]);
});

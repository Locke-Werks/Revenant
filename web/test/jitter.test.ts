import { test } from "node:test";
import assert from "node:assert/strict";
import { FRAME, JitterBuffer, MAX_TARGET_MS, SAMPLE_RATE, START_TARGET_MS, TRIM_SLACK_MS } from "../src/audio/jitter";

// A frame whose every sample is its seq + 1, so output says which packet it
// came from and silence (0) is distinguishable from frame 0.
const frame = (seq: number) => new Float32Array(FRAME).fill(seq + 1);

// Pulls in 128-sample render quanta, as an AudioWorklet does.
function pullFrames(jb: JitterBuffer, frames: number): Float32Array {
  const out = new Float32Array(frames * FRAME);
  for (let at = 0; at < out.length; at += 128) out.set(jb.pull(Math.min(128, out.length - at)), at);
  return out;
}

const frameValues = (pcm: Float32Array) => {
  const v: number[] = [];
  for (let i = 0; i < pcm.length; i += FRAME) v.push(pcm[i]!);
  return v;
};

test("jitter: silent until the 80 ms target is buffered, then in-order packets play through", () => {
  const jb = new JitterBuffer();
  assert.equal(jb.targetMs, START_TARGET_MS);
  assert.equal(START_TARGET_MS, 80);
  jb.push(0, frame(0));
  jb.push(1, frame(1));
  jb.push(2, frame(2));
  assert.deepEqual(frameValues(pullFrames(jb, 1)), [0]);
  assert.equal(jb.playing, false);
  jb.push(3, frame(3));
  jb.push(4, frame(4));
  const out = pullFrames(jb, 4);
  assert.deepEqual(frameValues(out), [1, 2, 3, 4]);
  // Whole frames, not just their first sample.
  assert.ok(out.subarray(0, FRAME).every((s) => s === 1));
  assert.equal(jb.stats.underruns, 0);
});

test("jitter: a missing seq is 20 ms of silence", () => {
  const jb = new JitterBuffer();
  for (const s of [0, 1, 3, 4]) jb.push(s, frame(s));
  const out = pullFrames(jb, 5);
  assert.deepEqual(frameValues(out), [1, 2, 0, 4, 5]);
  assert.ok(out.subarray(2 * FRAME, 3 * FRAME).every((s) => s === 0));
  assert.equal(jb.stats.concealed, 1);
});

test("jitter: an underrun raises the target to 100 ms, capped at 300", () => {
  const jb = new JitterBuffer();
  for (let s = 0; s < 4; ++s) jb.push(s, frame(s));
  pullFrames(jb, 4);
  pullFrames(jb, 1);
  assert.equal(jb.stats.underruns, 1);
  assert.equal(jb.targetMs, 100);
  assert.equal(jb.playing, false);

  let seq = 4;
  for (let i = 0; i < 20; ++i) {
    const need = jb.targetMs / 20;
    for (let k = 0; k < need; ++k, ++seq) jb.push(seq, frame(seq));
    pullFrames(jb, need + 1);
  }
  assert.equal(jb.targetMs, MAX_TARGET_MS);
  assert.equal(MAX_TARGET_MS, 300);
});

test("jitter: ten seconds without an underrun lowers the target by 10 ms", () => {
  const jb = new JitterBuffer();
  for (let s = 0; s < 4; ++s) jb.push(s, frame(s));
  pullFrames(jb, 5);
  assert.equal(jb.targetMs, 100);
  let seq = 4;
  for (; seq < 9; ++seq) jb.push(seq, frame(seq));
  // Steady state: one packet in, one frame out. 10 s is 500 frames.
  for (let i = 0; i < 499; ++i, ++seq) {
    jb.push(seq, frame(seq));
    pullFrames(jb, 1);
  }
  assert.equal(jb.targetMs, 100);
  jb.push(seq, frame(seq));
  ++seq;
  pullFrames(jb, 1);
  assert.equal(jb.targetMs, 90);
  for (let i = 0; i < 1500; ++i, ++seq) {
    jb.push(seq, frame(seq));
    pullFrames(jb, 1);
  }
  // Never below where it started.
  assert.equal(jb.targetMs, 80);
  assert.equal(jb.stats.underruns, 1);
});

test("jitter: seq wraps at 2^32", () => {
  const jb = new JitterBuffer();
  const seqs = [0xfffffffd, 0xfffffffe, 0xffffffff, 0, 1];
  for (const s of seqs) jb.push(s, frame(s % 1000));
  const out = pullFrames(jb, 5);
  assert.deepEqual(frameValues(out), seqs.map((s) => (s % 1000) + 1));
  assert.equal(jb.stats.concealed, 0);
});

test("jitter: a packet later than its play position is dropped and counted", () => {
  const jb = new JitterBuffer();
  for (const s of [0, 1, 3, 4]) jb.push(s, frame(s));
  pullFrames(jb, 3);
  jb.push(2, frame(2));
  assert.equal(jb.stats.late, 1);
  assert.deepEqual(frameValues(pullFrames(jb, 2)), [4, 5]);
});

test("jitter: packets reordered before playback starts are kept", () => {
  const jb = new JitterBuffer();
  for (const s of [11, 10, 12, 13]) jb.push(s, frame(s));
  assert.deepEqual(frameValues(pullFrames(jb, 4)), [11, 12, 13, 14]);
  assert.equal(jb.stats.late, 0);
});

test("jitter: a seq jump over a second resets instead of inserting silence", () => {
  const jb = new JitterBuffer();
  for (let s = 0; s < 4; ++s) jb.push(s, frame(s));
  pullFrames(jb, 2);
  for (let s = 1000; s < 1004; ++s) jb.push(s, frame(s));
  assert.equal(jb.stats.resets, 1);
  const out = pullFrames(jb, 4);
  assert.deepEqual(frameValues(out), [1001, 1002, 1003, 1004]);
  assert.equal(jb.stats.concealed, 0);
});

test("jitter: a jump backwards over a second (a new receiver from seq 0) resets and plays", () => {
  const jb = new JitterBuffer();
  let seq = 5000;
  for (let i = 0; i < 4; ++i) jb.push(seq++, frame(1));
  // Over a second of steady play.
  for (let i = 0; i < 60; ++i) {
    jb.push(seq++, frame(1));
    pullFrames(jb, 1);
  }
  for (let s = 0; s < 4; ++s) jb.push(s, frame(s));
  assert.equal(jb.stats.resets, 1);
  assert.equal(jb.stats.late, 0);
  assert.deepEqual(frameValues(pullFrames(jb, 4)), [1, 2, 3, 4]);
});

// A seeded generator, so the burst pattern is the same every run.
function lcg(seed: number) {
  let x = seed >>> 0;
  return () => {
    x = (Math.imul(x, 1664525) + 1013904223) >>> 0;
    return x / 2 ** 32;
  };
}

test("jitter: bursty delivery at the right average rate is never trimmed", () => {
  const jb = new JitterBuffer();
  const rnd = lcg(42);
  const total = 30 * SAMPLE_RATE;
  let produced = 0;
  let release = 0;
  let underrunsAt10s = 0;
  const out = new Float32Array(128);
  for (let t = 0; t < total; t += 128) {
    if (t >= release) {
      // Everything generated so far arrives at once, then nothing for
      // 100 to 200 ms.
      for (; (produced + 1) * FRAME <= t; ++produced) jb.push(produced, frame(produced % 1000));
      release = t + Math.round((0.1 + 0.1 * rnd()) * SAMPLE_RATE);
    }
    jb.pullInto(out);
    if (t < 10 * SAMPLE_RATE) underrunsAt10s = jb.stats.underruns;
  }
  assert.equal(jb.stats.trimmed, 0);
  assert.equal(jb.stats.resets, 0);
  // It adapts early on, then holds: at most the shrink probe every 10 s.
  assert.ok(jb.stats.underruns - underrunsAt10s <= 2, `underruns ${underrunsAt10s} then ${jb.stats.underruns}`);
});

test("jitter: a sustained backlog is trimmed a frame at a time, no sooner than 2 s", () => {
  const jb = new JitterBuffer();
  let seq = 0;
  for (; seq < 4; ++seq) jb.push(seq, frame(seq % 1000));
  pullFrames(jb, 1);
  // A stall that arrives all at once: 800 ms of backlog (under the 1 s
  // reset jump), then steady.
  for (let i = 0; i < 40; ++i, ++seq) jb.push(seq, frame(seq % 1000));
  for (let i = 0; i < 99; ++i, ++seq) {
    jb.push(seq, frame(seq % 1000));
    pullFrames(jb, 1);
  }
  assert.equal(jb.stats.trimmed, 0);
  for (let i = 0; i < 1500; ++i, ++seq) {
    jb.push(seq, frame(seq % 1000));
    pullFrames(jb, 1);
  }
  assert.ok(jb.stats.trimmed > 0);
  // One per 500 ms at most: 30 s of play allows 60.
  assert.ok(jb.stats.trimmed <= 60);
  const limit = Math.max(jb.targetMs + TRIM_SLACK_MS, 2 * jb.targetMs);
  assert.ok(jb.bufferedMs <= limit + 20, `buffered ${jb.bufferedMs} ms, limit ${limit}`);
  assert.equal(jb.stats.underruns, 0);
});

test("jitter: the splice fades from the dropped frame into the next", () => {
  const jb = new JitterBuffer();
  let seq = 0;
  // Every frame holds its own constant, so a splice without a fade would be
  // a step.
  for (; seq < 4; ++seq) jb.push(seq, frame(seq));
  pullFrames(jb, 1);
  for (let i = 0; i < 40; ++i, ++seq) jb.push(seq, frame(seq));
  let maxStep = 0;
  let prev: number | null = null;
  while (jb.stats.trimmed === 0 && seq < 400) {
    jb.push(seq, frame(seq));
    ++seq;
    const f = pullFrames(jb, 1);
    for (const v of f) {
      // Steps of 1 between neighbouring frames are the test signal itself.
      if (prev !== null) maxStep = Math.max(maxStep, Math.abs(v - prev));
      prev = v;
    }
  }
  assert.equal(jb.stats.trimmed, 1);
  // Skipping a frame is a step of 2 here; faded over SPLICE_FADE samples it
  // is far smaller per sample.
  assert.ok(maxStep <= 1.0001, `max step ${maxStep}`);
});

test("jitter: reset empties the buffer and waits to refill", () => {
  const jb = new JitterBuffer();
  for (let s = 0; s < 4; ++s) jb.push(s, frame(s));
  pullFrames(jb, 1);
  jb.reset();
  assert.equal(jb.playing, false);
  assert.equal(jb.bufferedMs, 0);
  assert.deepEqual(frameValues(pullFrames(jb, 1)), [0]);
});

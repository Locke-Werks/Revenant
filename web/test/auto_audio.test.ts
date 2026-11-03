import { test } from "node:test";
import assert from "node:assert/strict";
import { AutoAudio, type AutoAudioLive } from "../src/audio/auto_audio";

class FakeLive implements AutoAudioLive {
  on = false;
  starts: (number | null | undefined)[] = [];
  running = true;
  refuse: string | null = null;
  private runningFns: (() => void)[] = [];
  start(vrx?: number | null): Promise<void> {
    this.starts.push(vrx);
    if (this.refuse !== null) return Promise.reject(new Error(this.refuse));
    this.on = true;
    return Promise.resolve();
  }
  resume(): Promise<boolean> {
    return Promise.resolve(this.running);
  }
  onRunning(fn: () => void): void {
    this.runningFns.push(fn);
  }
  becomeRunning(): void {
    this.running = true;
    for (const fn of this.runningFns) fn();
  }
}

const flush = () => new Promise((r) => setTimeout(r, 0));

function rig() {
  const audio = new FakeLive();
  const notes: (string | null)[] = [];
  const auto = new AutoAudio({ audio, note: (t) => notes.push(t), notStarted: "tap to play" });
  return { audio, notes, auto };
}

test("auto audio: a DV receiver appearing turns the phone's audio on with no lead", async () => {
  const { audio, auto, notes } = rig();
  auto.update(false);
  assert.deepEqual(audio.starts, []);
  auto.update(true);
  auto.update(true);
  await flush();
  assert.deepEqual(audio.starts, [null]);
  assert.deepEqual(notes, [null]);
  // Already on: nothing more is asked.
  auto.update(true);
  assert.equal(audio.starts.length, 1);
});

test("auto audio: a context the browser will not start shows the tap note until it runs", async () => {
  const { audio, auto, notes } = rig();
  audio.running = false;
  auto.update(true);
  await flush();
  assert.deepEqual(notes, ["tap to play"]);
  audio.becomeRunning();
  assert.deepEqual(notes, ["tap to play", null]);
});

test("auto audio: after a lease loss and take back it asks again on the new session", async () => {
  const { audio, auto } = rig();
  auto.update(true);
  await flush();
  // Lease lost: audio stopped, and the old session refuses the next ask.
  audio.on = false;
  audio.refuse = "the lease is held elsewhere";
  auto.update(true);
  await flush();
  auto.update(true);
  await flush();
  assert.equal(audio.starts.length, 2);
  // Taken back.
  audio.refuse = null;
  auto.newSession();
  auto.update(true);
  await flush();
  assert.equal(audio.starts.length, 3);
  assert.equal(audio.on, true);
});

test("auto audio: a refusal is reported once and not asked again", async () => {
  const { audio, auto, notes } = rig();
  audio.refuse = "no engine";
  auto.update(true);
  await flush();
  auto.update(true);
  await flush();
  assert.equal(audio.starts.length, 1);
  assert.deepEqual(notes, ["Live audio unavailable: no engine"]);
});

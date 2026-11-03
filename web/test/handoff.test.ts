import { test } from "node:test";
import assert from "node:assert/strict";
import { Handoff, type HandoffCommand } from "../src/audio/handoff";

function machine(): { h: Handoff; out: HandoffCommand[]; take(): HandoffCommand[] } {
  const out: HandoffCommand[] = [];
  const h = new Handoff((c) => out.push(c));
  return {
    h,
    out,
    take() {
      return out.splice(0);
    },
  };
}

test("keep listening from Live stops live and plays HLS with the banner", () => {
  const m = machine();
  m.h.keepListeningOn();
  assert.deepEqual(m.take(), ["hlsOn", "stopLive", "playHls", "showBehindBanner"]);
  assert.equal(m.h.state, "Background");
});

test("hiding in Background does nothing", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.take();
  m.h.hidden();
  assert.deepEqual(m.take(), []);
  assert.equal(m.h.state, "Background");
});

test("visible in Background starts live and leaves HLS playing", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.take();
  m.h.visible();
  const out = m.take();
  assert.deepEqual(out, ["startLive"]);
  assert.ok(!out.includes("pauseHls"));
  assert.equal(m.h.state, "Priming");
});

test("live ready and resumed cuts to live", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.h.visible();
  m.take();
  m.h.liveReady();
  assert.deepEqual(m.take(), []);
  m.h.resumeResult(true);
  assert.deepEqual(m.take(), ["fadeCut", "pauseHls", "hideBehindBanner"]);
  assert.equal(m.h.state, "Live");
});

test("resume then ready cuts too", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.h.visible();
  m.h.resumeResult(true);
  m.take();
  m.h.liveReady();
  assert.deepEqual(m.take(), ["fadeCut", "pauseHls", "hideBehindBanner"]);
});

test("refused resume shows Go live; the tap cuts once live is ready", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.h.visible();
  m.take();
  m.h.resumeResult(false);
  assert.deepEqual(m.take(), ["showGoLive"]);
  assert.equal(m.h.state, "NeedsTap");
  m.h.liveReady();
  assert.deepEqual(m.take(), []);
  m.h.goLiveTap();
  assert.deepEqual(m.take(), ["hideGoLive", "fadeCut", "pauseHls", "hideBehindBanner"]);
  assert.equal(m.h.state, "Live");
});

test("a tap before live has filled waits for it", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.h.visible();
  m.h.resumeResult(false);
  m.take();
  m.h.goLiveTap();
  assert.deepEqual(m.take(), ["hideGoLive"]);
  assert.equal(m.h.state, "Priming");
  m.h.liveReady();
  assert.deepEqual(m.take(), ["fadeCut", "pauseHls", "hideBehindBanner"]);
});

test("keep listening off in the foreground cuts to live before HLS stops", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.take();
  m.h.keepListeningOff();
  assert.deepEqual(m.take(), ["startLive"]);
  m.h.resumeResult(true);
  m.h.liveReady();
  assert.deepEqual(m.take(), ["fadeCut", "pauseHls", "hideBehindBanner", "hlsOff"]);
});

test("keep listening off from the lock screen stops HLS", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.take();
  m.h.keepListeningOff();
  assert.deepEqual(m.take(), ["stopHls", "hideBehindBanner", "hlsOff"]);
  assert.equal(m.h.state, "Live");
});

test("hiding Live with keep listening on goes back to HLS", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.h.visible();
  m.h.resumeResult(true);
  m.h.liveReady();
  m.take();
  m.h.hidden();
  assert.deepEqual(m.take(), ["stopLive", "playHls", "showBehindBanner"]);
});

test("a lost receiver stops everything and says nothing is left", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.h.visible();
  m.h.resumeResult(false);
  m.take();
  m.h.receiverGone();
  assert.deepEqual(m.take(), ["hideGoLive", "stopLive", "stopHls", "hideBehindBanner", "hlsOff"]);
  assert.equal(m.h.state, "Live");
  assert.equal(m.h.keepListening, false);
});

// ---- Exhaustive walk -------------------------------------------------------

type Input =
  | "keepListeningOn"
  | "keepListeningOff"
  | "visible"
  | "hidden"
  | "liveReady"
  | "resumeOk"
  | "resumeRefused"
  | "goLiveTap"
  | "receiverGone";

const INPUTS: Input[] = [
  "keepListeningOn",
  "keepListeningOff",
  "visible",
  "hidden",
  "liveReady",
  "resumeOk",
  "resumeRefused",
  "goLiveTap",
  "receiverGone",
];

function apply(h: Handoff, i: Input): void {
  switch (i) {
    case "resumeOk":
      return h.resumeResult(true);
    case "resumeRefused":
      return h.resumeResult(false);
    default:
      return h[i]();
  }
}

function* sequences(n: number): Generator<Input[]> {
  if (n === 0) {
    yield [];
    return;
  }
  for (const head of sequences(n - 1)) for (const i of INPUTS) yield [...head, i];
}

test("no sequence of four inputs pauses HLS before live is ready, or goes quiet by itself", () => {
  let walked = 0;
  for (const seq of sequences(4)) {
    walked++;
    const cmds: HandoffCommand[] = [];
    const h = new Handoff((c) => cmds.push(c));
    let hls = false;
    let liveOn = true; // Live at rest: whatever LiveAudio is doing.
    // Since the last startLive: has liveReady arrived, has resume succeeded.
    let filled = false;
    let resumed = false;
    for (const input of seq) {
      cmds.length = 0;
      // A goLiveTap is itself a successful resume, if the machine was asking.
      const before = h.state;
      apply(h, input);
      if (input === "liveReady" && (before === "Priming" || before === "NeedsTap")) filled = true;
      if (input === "resumeOk" && before === "Priming") resumed = true;
      if (input === "goLiveTap" && before === "NeedsTap") resumed = true;
      for (const c of cmds) {
        const where = `${seq.join(",")} at ${input}: ${cmds.join(",")}`;
        switch (c) {
          case "startLive":
            filled = false;
            resumed = false;
            liveOn = false;
            break;
          case "stopLive":
            liveOn = false;
            break;
          case "playHls":
            hls = true;
            break;
          case "fadeCut":
            assert.ok(filled && resumed, `cut before live was ready: ${where}`);
            liveOn = true;
            break;
          case "pauseHls":
            assert.ok(filled && resumed && liveOn, `HLS paused before live was ready: ${where}`);
            hls = false;
            break;
          case "stopHls":
            assert.ok(
              input === "receiverGone" || (input === "keepListeningOff" && !h.isVisible),
              `HLS stopped with no reason the user can see: ${where}`,
            );
            hls = false;
            break;
          default:
            break;
        }
      }
      // Never both, never neither while keep listening is meant to play.
      assert.ok(!(hls && liveOn), `both paths playing: ${seq.join(",")} at ${input}`);
      if (h.keepListening && !h.isVisible) assert.ok(hls, `keep listening hidden but HLS silent: ${seq.join(",")}`);
      if (h.state !== "Live") assert.ok(hls, `state ${h.state} without HLS: ${seq.join(",")}`);
    }
  }
  assert.equal(walked, INPUTS.length ** 4);
});

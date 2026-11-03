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

test("refused resume shows Go live; the tap cuts once its resume succeeds", () => {
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
  assert.deepEqual(m.take(), ["hideGoLive"]);
  assert.equal(m.h.state, "Priming");
  m.h.resumeResult(true);
  assert.deepEqual(m.take(), ["fadeCut", "pauseHls", "hideBehindBanner"]);
  assert.equal(m.h.state, "Live");
});

test("a tap whose resume fails brings the pill back with HLS playing", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.h.visible();
  m.h.resumeResult(false);
  m.h.liveReady();
  m.h.goLiveTap();
  m.take();
  m.h.resumeResult(false);
  assert.deepEqual(m.take(), ["showGoLive"]);
  assert.equal(m.h.state, "NeedsTap");
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
  m.h.resumeResult(true);
  assert.deepEqual(m.take(), []);
  m.h.liveReady();
  assert.deepEqual(m.take(), ["fadeCut", "pauseHls", "hideBehindBanner"]);
});

test("HLS failing in the foreground makes live audible at once", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.take();
  m.h.hlsFailed();
  assert.deepEqual(m.take(), ["stopHls", "restoreLive", "hideBehindBanner", "hlsOff"]);
  assert.equal(m.h.state, "Live");
  assert.equal(m.h.keepListening, false);
});

test("HLS failing while hidden changes nothing until the page shows", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.take();
  m.h.hlsFailed();
  assert.deepEqual(m.take(), []);
  m.h.visible();
  assert.deepEqual(m.take(), ["startLive"]);
});

test("keep listening off in the foreground cuts to live before HLS stops", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.take();
  m.h.keepListeningOff();
  assert.deepEqual(m.take(), ["startLive"]);
  m.h.resumeResult(true);
  m.h.liveReady();
  // Stopped rather than paused: HLS is going off, so a later re-enable must
  // load fresh instead of seeking the old element.
  assert.deepEqual(m.take(), ["fadeCut", "stopHls", "hideBehindBanner", "hlsOff"]);
});

test("keep listening off from the lock screen stops HLS", () => {
  const m = machine();
  m.h.keepListeningOn();
  m.h.hidden();
  m.take();
  m.h.keepListeningOff();
  assert.deepEqual(m.take(), ["stopHls", "restoreLive", "hideBehindBanner", "hlsOff"]);
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
  assert.deepEqual(m.take(), ["hideGoLive", "stopHls", "restoreLive", "hideBehindBanner", "hlsOff"]);
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
  | "receiverGone"
  | "hlsFailed";

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
  "hlsFailed",
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

const WALK = 6;

test("no sequence of six inputs pauses HLS before live is ready, or goes quiet by itself", () => {
  let walked = 0;
  for (const seq of sequences(WALK)) {
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
      const before = h.state;
      apply(h, input);
      if (input === "liveReady" && (before === "Priming" || before === "NeedsTap")) filled = true;
      if (input === "resumeOk" && before === "Priming") resumed = true;
      // The tap's own resume is reported as resumeOk or resumeRefused, so
      // the tap itself proves nothing.
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
          case "restoreLive":
            liveOn = true;
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
              input === "receiverGone" ||
                input === "hlsFailed" ||
                (input === "keepListeningOff" && !h.isVisible) ||
                // A cut with keep listening off: live is already playing.
                (!h.keepListening && filled && resumed && liveOn),
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
      // Live is the resting state: whatever left HLS must have given live back.
      if (h.state === "Live") assert.ok(liveOn, `Live with live held: ${seq.join(",")}`);
    }
  }
  assert.equal(walked, INPUTS.length ** WALK);
});

import { test } from "node:test";
import assert from "node:assert/strict";
import { HlsPlayer, PROBE_EVERY_MS, RETRY_AFTER_MS, STALL_MS, type MediaLike } from "../src/audio/hls_player";
import { KeepListening, REPLACE_GRACE_MS, type KeepLink, type LivePath, type MediaSessionLike } from "../src/audio/keep_listening";
import type { JsonValue } from "../src/protocol";

class Clock {
  t = 0;
  private q: { at: number; fn: () => void; id: number }[] = [];
  private next = 1;
  setTimeout = (fn: () => void, ms: number): unknown => {
    const id = this.next++;
    this.q.push({ at: this.t + ms, fn, id });
    return id;
  };
  clearTimeout = (h: unknown): void => {
    this.q = this.q.filter((e) => e.id !== h);
  };
  async advance(ms: number): Promise<void> {
    const end = this.t + ms;
    for (;;) {
      await flush();
      this.q.sort((a, b) => a.at - b.at);
      const e = this.q[0];
      if (!e || e.at > end) break;
      this.q.shift();
      this.t = e.at;
      e.fn();
    }
    this.t = end;
    await flush();
  }
}

async function flush(): Promise<void> {
  for (let i = 0; i < 10; i++) await Promise.resolve();
}

class FakeMedia implements MediaLike {
  src = "";
  paused = true;
  currentTime = 0;
  seekEnd = 0;
  refuse = false;
  get seekable() {
    const end = this.seekEnd;
    return { length: end > 0 ? 1 : 0, end: () => end };
  }
  plays: string[] = [];
  unloads = 0;
  private fns = new Map<string, (() => void)[]>();
  play(): Promise<void> {
    if (this.refuse) return Promise.reject(new Error("NotAllowedError"));
    this.plays.push(this.src);
    this.paused = false;
    return Promise.resolve();
  }
  pause(): void {
    this.paused = true;
  }
  load(): void {
    this.unloads++;
  }
  removeAttribute(_name: string): void {
    this.src = "";
  }
  addEventListener(type: string, fn: () => void): void {
    const l = this.fns.get(type) ?? [];
    l.push(fn);
    this.fns.set(type, l);
  }
  fire(type: string): void {
    for (const fn of this.fns.get(type) ?? []) fn();
  }
}

function player(clock: Clock, served: () => boolean) {
  const media = new FakeMedia();
  const hls = new HlsPlayer({
    media,
    probe: () => Promise.resolve(served()),
    setTimeout: clock.setTimeout,
    clearTimeout: clock.clearTimeout,
    silence: "data:silence",
  });
  return { media, hls };
}

test("hls: waits for a URL and for the playlist to be served before playing", async () => {
  const clock = new Clock();
  let served = false;
  const { media, hls } = player(clock, () => served);
  hls.play();
  assert.equal(hls.status.kind, "starting");
  hls.setUrl("/hls/4/live.m3u8");
  await clock.advance(PROBE_EVERY_MS * 2);
  assert.deepEqual(media.plays, []);
  served = true;
  await clock.advance(PROBE_EVERY_MS);
  assert.deepEqual(media.plays, ["/hls/4/live.m3u8"]);
  media.fire("playing");
  assert.equal(hls.status.kind, "playing");
  hls.stop();
  assert.equal(media.src, "");
  assert.equal(media.unloads, 1);
  assert.equal(hls.status.kind, "idle");
});

test("hls: the unlocking play in the tap uses the silent clip", () => {
  const { media, hls } = player(new Clock(), () => true);
  hls.unlock();
  assert.deepEqual(media.plays, ["data:silence"]);
});

test("hls: no stream from the gateway is a failure with its reason", () => {
  const { hls } = player(new Clock(), () => true);
  hls.play();
  hls.setUrl(null, "encoder unavailable");
  assert.deepEqual(hls.status, { kind: "failed", error: "encoder unavailable" });
});

test("hls: a new URL while playing switches to it", async () => {
  const clock = new Clock();
  const { media, hls } = player(clock, () => true);
  hls.setUrl("/hls/4/live.m3u8");
  hls.play();
  await clock.advance(0);
  hls.setUrl("/hls/9/live.m3u8");
  await clock.advance(0);
  assert.deepEqual(media.plays, ["/hls/4/live.m3u8", "/hls/9/live.m3u8"]);
});

// ---- Glue -------------------------------------------------------------------

class FakeLink implements KeepLink {
  ready = true;
  requests: JsonValue[] = [];
  private fns = new Map<string, ((p: never) => void)[]>();
  request(_op: "keepListening", args: { [key: string]: JsonValue }): Promise<JsonValue> {
    this.requests.push(args);
    return Promise.resolve(args);
  }
  on(name: string, fn: (p: never) => void): () => void {
    const l = this.fns.get(name) ?? [];
    l.push(fn);
    this.fns.set(name, l);
    return () => {
      const i = l.indexOf(fn);
      if (i >= 0) l.splice(i, 1);
    };
  }
  emit(name: string, payload: unknown): void {
    for (const fn of [...(this.fns.get(name) ?? [])]) (fn as (p: unknown) => void)(payload);
  }
}

class FakeLive implements LivePath {
  vrx: number | null = 4;
  ready = false;
  resumeOk = true;
  log: string[] = [];
  hold(): void {
    this.log.push("hold");
  }
  release(): void {
    this.log.push("release");
  }
  fadeIn(ms: number): void {
    this.log.push(`fadeIn ${ms}`);
  }
  resume(): Promise<boolean> {
    this.log.push("resume");
    return Promise.resolve(this.resumeOk);
  }
}

function glue() {
  const clock = new Clock();
  const link = new FakeLink();
  const live = new FakeLive();
  const { media, hls } = player(clock, () => true);
  const ui = { behind: false, goLive: false, message: null as string | null, keep: false };
  const meta: string[] = [];
  const states: string[] = [];
  const session: MediaSessionLike = {
    setMetadata: (title) => meta.push(title),
    setPlaybackState: (s) => states.push(s),
    setActionHandler: () => {},
  };
  let hidden = false;
  const keep = new KeepListening({
    link,
    live,
    hls,
    ui: {
      behind: (s) => (ui.behind = s),
      goLive: (s) => (ui.goLive = s),
      message: (t) => (ui.message = t),
      keep: (on) => (ui.keep = on),
    },
    media: session,
    hidden: () => hidden,
    setTimeout: clock.setTimeout,
    clearTimeout: clock.clearTimeout,
  });
  const setHidden = (h: boolean) => {
    hidden = h;
    keep.visibility(h);
  };
  return { clock, link, live, media, hls, ui, keep, meta, states, setHidden };
}

test("glue: keep listening needs a receiver playing", () => {
  const g = glue();
  g.live.vrx = null;
  g.keep.on();
  assert.equal(g.keep.state, "Live");
  assert.match(g.ui.message ?? "", /Listen to a receiver first/);
});

test("glue: turning it on keeps live playing until HLS is heard", async () => {
  const g = glue();
  g.keep.on();
  assert.deepEqual(g.media.plays, ["data:silence"]);
  assert.deepEqual(g.link.requests, [{ on: true }]);
  assert.equal(g.ui.behind, true);
  assert.deepEqual(g.live.log, []);
  g.link.emit("hls", { event: "hls", vrx: 4, url: "/hls/4/live.m3u8" });
  await g.clock.advance(0);
  g.media.fire("playing");
  assert.deepEqual(g.live.log, ["hold"]);
});

test("glue: back in the foreground live primes muted, then cuts", async () => {
  const g = glue();
  g.keep.on();
  g.link.emit("hls", { event: "hls", vrx: 4, url: "/hls/4/live.m3u8" });
  await g.clock.advance(0);
  g.media.fire("playing");
  g.setHidden(true);
  g.live.log.length = 0;
  g.setHidden(false);
  await g.clock.advance(0);
  assert.deepEqual(g.live.log, ["release", "resume"]);
  assert.equal(g.keep.state, "Priming");
  assert.equal(g.media.paused, false);
  g.live.ready = true;
  await g.clock.advance(100);
  assert.deepEqual(g.live.log, ["release", "resume", "fadeIn 40"]);
  assert.equal(g.keep.state, "Live");
  assert.equal(g.media.paused, true);
  assert.equal(g.ui.behind, false);
});

test("glue: priming waits for the link to come back", async () => {
  const g = glue();
  g.keep.on();
  g.setHidden(true);
  g.link.ready = false;
  g.live.log.length = 0;
  g.setHidden(false);
  await g.clock.advance(1000);
  assert.deepEqual(g.live.log, []);
  g.link.ready = true;
  g.link.emit("ready", { resumed: true });
  await g.clock.advance(0);
  assert.deepEqual(g.live.log, ["release", "resume"]);
});

test("glue: a refused resume shows Go live and the tap resumes", async () => {
  const g = glue();
  g.keep.on();
  g.setHidden(true);
  g.live.resumeOk = false;
  g.setHidden(false);
  await g.clock.advance(0);
  assert.equal(g.keep.state, "NeedsTap");
  assert.equal(g.ui.goLive, true);
  g.live.resumeOk = true;
  g.live.ready = true;
  g.keep.goLive();
  await g.clock.advance(100);
  assert.equal(g.keep.state, "Live");
  assert.equal(g.ui.goLive, false);
});

test("glue: a replaced receiver is followed to its new id", async () => {
  const g = glue();
  g.keep.on();
  g.link.emit("hls", { event: "hls", vrx: 4, url: "/hls/4/live.m3u8" });
  await g.clock.advance(0);
  g.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 4, reason: "replaced" });
  g.link.emit("hls", { event: "hls", vrx: 7, url: "/hls/7/live.m3u8" });
  await g.clock.advance(REPLACE_GRACE_MS * 2);
  assert.equal(g.keep.state, "Background");
  assert.equal(g.ui.message, null);
  assert.deepEqual(g.media.plays.slice(-1), ["/hls/7/live.m3u8"]);
});

test("glue: a removed receiver stops keep listening and says why", async () => {
  const g = glue();
  g.keep.on();
  g.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 4, reason: "retuned out of band" });
  await g.clock.advance(REPLACE_GRACE_MS);
  assert.equal(g.keep.state, "Live");
  assert.equal(g.ui.keep, false);
  assert.match(g.ui.message ?? "", /retuned out of band/);
  assert.deepEqual(g.link.requests.slice(-1), [{ on: false }]);
});

test("glue: an engine restart maps the id, or reports it lost", async () => {
  const g = glue();
  g.keep.on();
  g.link.emit("link", { event: "link", state: "up", recreated: [{ old: 4, new: 11 }], lost: [] });
  g.link.emit("link", { event: "link", state: "up", recreated: [], lost: [{ vrx: 11, reason: "out of range" }] });
  assert.equal(g.keep.state, "Live");
  assert.match(g.ui.message ?? "", /out of range/);
});

test("glue: a restart map is applied once and lost names old ids", async () => {
  const g = glue();
  g.keep.on();
  g.link.emit("link", { event: "link", state: "up", detail: "", recreated: [{ old: 4, new: 5 }, { old: 5, new: 4 }], lost: [{ vrx: 5, reason: "x" }] });
  assert.equal(g.keep.state, "Background");
  assert.equal(g.ui.message, null);
  g.keep.retarget(5, 9);
  g.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 5, reason: "replaced" });
  await g.clock.advance(REPLACE_GRACE_MS * 2);
  assert.equal(g.keep.state, "Background");
});

test("glue: a reconnect that lost the session asks for keep listening again", () => {
  const g = glue();
  g.keep.on();
  g.link.requests.length = 0;
  g.link.emit("ready", { resumed: false });
  assert.deepEqual(g.link.requests, [{ on: true }]);
});

test("glue: the lock screen shows frequency and mode", async () => {
  const g = glue();
  g.keep.setNowPlaying({ hz: 146_520_000, mode: "nfm" });
  assert.deepEqual(g.meta.slice(-1), ["146.5200 MHz NFM"]);
  g.keep.on();
  g.link.emit("hls", { event: "hls", vrx: 4, url: "/hls/4/live.m3u8" });
  await g.clock.advance(0);
  g.media.fire("playing");
  assert.equal(g.states.at(-1), "playing");
});

// ---- Fix round 1 -------------------------------------------------------------

async function playingHls(g: ReturnType<typeof glue>): Promise<void> {
  g.keep.on();
  g.link.emit("hls", { event: "hls", vrx: 4, url: "/hls/4/live.m3u8" });
  await g.clock.advance(0);
  g.media.fire("playing");
}

test("glue: a lock-screen pause gives live back at full level", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  g.live.log.length = 0;
  g.keep.off();
  assert.deepEqual(g.live.log, ["release", "fadeIn 0", "resume"]);
  assert.equal(g.media.src, "");
});

test("glue: a removed receiver gives live back from Priming too", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  g.setHidden(false);
  await g.clock.advance(0);
  g.live.log.length = 0;
  g.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 4, reason: "gone" });
  await g.clock.advance(REPLACE_GRACE_MS);
  assert.deepEqual(g.live.log, ["release", "fadeIn 0", "resume"]);
  assert.equal(g.keep.state, "Live");
});

test("glue: removal in Live with keep listening on still stops it with a reason", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  g.setHidden(false);
  g.live.ready = true;
  await g.clock.advance(100);
  assert.equal(g.keep.state, "Live");
  g.link.emit("vrxRemoved", { event: "vrxRemoved", vrx: 4, reason: "gone" });
  await g.clock.advance(REPLACE_GRACE_MS);
  assert.equal(g.ui.keep, false);
  assert.match(g.ui.message ?? "", /gone/);
});

test("glue: a Go live tap whose resume fails brings the pill back", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  g.live.resumeOk = false;
  g.setHidden(false);
  await g.clock.advance(0);
  g.live.ready = true;
  g.keep.goLive();
  await g.clock.advance(100);
  assert.equal(g.keep.state, "NeedsTap");
  assert.equal(g.ui.goLive, true);
  assert.equal(g.media.paused, false);
});

test("hls: a clock that stops while playing is a failure and retries", async () => {
  const clock = new Clock();
  const { media, hls } = player(clock, () => true);
  hls.setUrl("/hls/4/live.m3u8");
  hls.play();
  await clock.advance(0);
  media.fire("playing");
  await clock.advance(STALL_MS);
  assert.equal(hls.status.kind, "starting");
  await clock.advance(RETRY_AFTER_MS);
  assert.equal(media.plays.length, 2);
});

test("hls: stalled counts only once the clock has also stopped", async () => {
  const clock = new Clock();
  const { media, hls } = player(clock, () => true);
  hls.setUrl("/hls/4/live.m3u8");
  hls.play();
  await clock.advance(0);
  media.fire("playing");
  media.fire("stalled");
  assert.equal(hls.status.kind, "playing");
  await clock.advance(STALL_MS / 2);
  media.fire("stalled");
  assert.equal(hls.status.kind, "starting");
});

test("glue: lock-screen play after HLS failed starts the stream again", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  for (let i = 0; i < 7; i++) {
    g.media.fire("error");
    await g.clock.advance(RETRY_AFTER_MS);
  }
  assert.equal(g.hls.status.kind, "failed");
  const plays = g.media.plays.length;
  g.keep.on();
  await g.clock.advance(0);
  assert.ok(g.media.plays.length > plays);
});

test("glue: the cut pauses the element and the next hide seeks it to the live edge", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  g.setHidden(false);
  g.live.ready = true;
  await g.clock.advance(100);
  assert.equal(g.media.src, "/hls/4/live.m3u8");
  assert.equal(g.media.paused, true);
  g.media.seekEnd = 42;
  g.setHidden(true);
  await g.clock.advance(0);
  assert.equal(g.media.currentTime, 42);
  assert.equal(g.media.paused, false);
});

test("glue: a refused play while hidden says why", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  g.setHidden(false);
  g.live.ready = true;
  await g.clock.advance(100);
  g.media.refuse = true;
  g.setHidden(true);
  await g.clock.advance(PROBE_EVERY_MS);
  assert.match(g.ui.message ?? "", /refused to play/);
});

test("glue: HLS failing while priming makes live audible at once", async () => {
  const g = glue();
  await playingHls(g);
  g.setHidden(true);
  g.setHidden(false);
  await g.clock.advance(0);
  g.live.log.length = 0;
  g.link.emit("hls", { event: "hls", vrx: 4, url: null, error: "encoder died" });
  assert.deepEqual(g.live.log, ["release", "fadeIn 0", "resume"]);
  assert.equal(g.keep.state, "Live");
});

test("glue: ready sends the wanted state, off included", () => {
  const g = glue();
  g.link.emit("ready", { resumed: false });
  assert.deepEqual(g.link.requests, [{ on: false }]);
});

test("hls: the same URL announced after a failure starts again", async () => {
  const clock = new Clock();
  const { media, hls } = player(clock, () => true);
  hls.play();
  hls.setUrl(null, "down");
  hls.setUrl("/hls/4/live.m3u8");
  await clock.advance(0);
  media.fire("error");
  for (let i = 0; i < 6; i++) {
    await clock.advance(RETRY_AFTER_MS);
    media.fire("error");
  }
  assert.equal(hls.status.kind, "failed");
  const plays = media.plays.length;
  hls.setUrl("/hls/4/live.m3u8");
  await clock.advance(0);
  assert.equal(media.plays.length, plays + 1);
});

test("glue: showing and hiding while the link is down leaves one wait", async () => {
  const g = glue();
  await playingHls(g);
  g.link.ready = false;
  for (let i = 0; i < 5; i++) {
    g.setHidden(true);
    g.setHidden(false);
  }
  g.live.log.length = 0;
  g.link.ready = true;
  g.link.emit("ready", { resumed: true });
  await g.clock.advance(0);
  assert.deepEqual(g.live.log.filter((x) => x === "release"), ["release"]);
});

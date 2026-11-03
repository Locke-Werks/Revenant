// Keep listening, wired: carries out the Handoff machine's commands on the
// live path (LiveAudio) and the HLS element, follows the receiver through id
// changes, and feeds the lock screen.

import type { GatewayEvent, JsonValue, LinkEvent } from "../protocol";
import { Handoff, type HandoffCommand, type HandoffState } from "./handoff";
import type { HlsPlayer, HlsStatus } from "./hls_player";

// The parts of LiveAudio this drives.
export interface LivePath {
  readonly vrx: number | null;
  readonly ready: boolean;
  hold(): void;
  release(): void;
  fadeIn(ms: number): void;
  resume(): Promise<boolean>;
}

export interface KeepLink {
  readonly ready: boolean;
  request(op: "keepListening", args: { [key: string]: JsonValue }): Promise<JsonValue>;
  on(name: "ready", fn: (e: { resumed: boolean }) => void): () => void;
  on(name: "link", fn: (e: LinkEvent) => void): () => void;
  on(name: "vrxRemoved", fn: (e: GatewayEvent) => void): () => void;
  on(name: "hls", fn: (e: GatewayEvent) => void): () => void;
}

export interface KeepUi {
  behind(show: boolean): void;
  goLive(show: boolean): void;
  // A sentence for the user, or null to clear it.
  message(text: string | null): void;
  // Keep listening on or off, for the toggle.
  keep(on: boolean): void;
}

export interface NowPlaying {
  hz: number;
  mode: string;
}

// The lock screen. Absent where the browser has no Media Session API.
export interface MediaSessionLike {
  setMetadata(title: string, artist: string): void;
  setPlaybackState(state: "playing" | "paused" | "none"): void;
  setActionHandler(action: "play" | "pause", fn: (() => void) | null): void;
}

export interface KeepListeningDeps {
  link: KeepLink;
  live: LivePath;
  hls: HlsPlayer;
  ui: KeepUi;
  media: MediaSessionLike | null;
  hidden(): boolean;
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(h: unknown): void;
}

// Long enough to hide the step, short enough to keep the cut a cut.
export const FADE_MS = 40;
export const READY_POLL_MS = 50;
// A setMode or setBandwidth replace removes the old id, then the gateway
// announces HLS for the new one. A removal not followed by that in this long
// is a receiver that is really gone.
export const REPLACE_GRACE_MS = 1_500;

export class KeepListening {
  readonly handoff: Handoff;
  private following: number | null = null;
  // Bumped whenever priming starts or stops, so a stale wait does nothing.
  private primeGen = 0;
  private poll: unknown = null;
  private grace: unknown = null;
  // stopLive arrived while HLS was still loading: live keeps playing until
  // HLS is heard, so turning keep listening on is never a gap.
  private holdWhenHlsPlays = false;
  private now: NowPlaying | null = null;

  constructor(private readonly deps: KeepListeningDeps) {
    this.handoff = new Handoff((c) => this.run(c));
    if (deps.hidden()) this.handoff.hidden();

    deps.hls.onStatus((s) => this.onHlsStatus(s));
    deps.link.on("hls", (e) => this.onHls(e));
    deps.link.on("ready", () => {
      // A refused resume lands on a fresh session with keep listening off;
      // a resumed one does not mind hearing it again.
      if (this.gatewayWants()) this.request(true);
    });
    deps.link.on("link", (e) => {
      for (const r of e.recreated ?? []) if (r.old === this.following) this.following = r.new;
      for (const l of e.lost ?? []) {
        if (l.vrx === this.following) this.gone(`the receiver was lost when the engine restarted: ${l.reason}`);
      }
    });
    deps.link.on("vrxRemoved", (e) => {
      if (typeof e.vrx !== "number" || e.vrx !== this.following || this.handoff.state === "Live") return;
      const reason = typeof e.reason === "string" && e.reason !== "" ? e.reason : "removed";
      this.clearGrace();
      this.grace = deps.setTimeout(() => {
        this.grace = null;
        this.gone(`the receiver was removed (${reason})`);
      }, REPLACE_GRACE_MS);
    });

    const media = deps.media;
    if (media) {
      // From the lock screen: play is keep listening on (the element was
      // unlocked by the first tap), pause is off.
      media.setActionHandler("play", () => this.on());
      media.setActionHandler("pause", () => this.off());
    }
  }

  get state(): HandoffState {
    return this.handoff.state;
  }

  // The keep-listening tap. Must run inside the gesture.
  on(): void {
    const vrx = this.deps.live.vrx;
    if (vrx === null) {
      this.deps.ui.message("Listen to a receiver first, then keep listening.");
      return;
    }
    this.following = vrx;
    this.deps.ui.message(null);
    this.deps.hls.unlock();
    this.handoff.keepListeningOn();
    this.deps.ui.keep(this.handoff.keepListening);
  }

  off(): void {
    this.handoff.keepListeningOff();
    this.deps.ui.keep(this.handoff.keepListening);
  }

  // The Go live pill. The resume is made inside the tap, which is what a
  // refused one needs.
  goLive(): void {
    const resumed = this.deps.live.resume();
    this.handoff.goLiveTap();
    void resumed.then((ok) => {
      if (!ok) this.handoff.resumeResult(false);
    });
  }

  visibility(hidden: boolean): void {
    if (hidden) this.handoff.hidden();
    else this.handoff.visible();
  }

  // What the lock screen shows: the receiver's frequency and mode.
  setNowPlaying(now: NowPlaying): void {
    this.now = now;
    this.updateMedia();
  }

  private run(c: HandoffCommand): void {
    const { live, hls, ui } = this.deps;
    switch (c) {
      case "hlsOn":
        return this.request(true);
      case "hlsOff":
        return this.request(false);
      case "playHls":
        hls.play();
        this.updateMedia();
        return;
      case "pauseHls":
      case "stopHls":
        this.holdWhenHlsPlays = false;
        hls.stop();
        this.updateMedia();
        return;
      case "startLive":
        return this.prime();
      case "stopLive":
        this.stopPriming();
        if (hls.playing || this.deps.hidden()) live.hold();
        else this.holdWhenHlsPlays = true;
        return;
      case "fadeCut":
        this.stopPriming();
        live.fadeIn(FADE_MS);
        return;
      case "showGoLive":
        return ui.goLive(true);
      case "hideGoLive":
        return ui.goLive(false);
      case "showBehindBanner":
        return ui.behind(true);
      case "hideBehindBanner":
        return ui.behind(false);
    }
  }

  private prime(): void {
    const gen = ++this.primeGen;
    this.holdWhenHlsPlays = false;
    const { link, live } = this.deps;
    const go = () => {
      if (gen !== this.primeGen) return;
      live.release();
      void live.resume().then((ok) => {
        if (gen === this.primeGen) this.handoff.resumeResult(ok);
      });
      const tick = () => {
        this.poll = null;
        if (gen !== this.primeGen) return;
        if (live.ready) {
          this.handoff.liveReady();
          if (this.handoff.state !== "NeedsTap") return;
        }
        this.poll = this.deps.setTimeout(tick, READY_POLL_MS);
      };
      tick();
    };
    // Back from the background the link may still be reconnecting or
    // resuming; live has nothing to fill from until it is ready, and HLS
    // carries on meanwhile.
    if (link.ready) {
      go();
    } else {
      const off = link.on("ready", () => {
        off();
        go();
      });
    }
  }

  private stopPriming(): void {
    this.primeGen++;
    if (this.poll !== null) this.deps.clearTimeout(this.poll);
    this.poll = null;
  }

  // HLS is needed while keep listening is on, and while it still plays
  // waiting for a cut.
  private gatewayWants(): boolean {
    return this.handoff.keepListening || this.handoff.state !== "Live";
  }

  private request(on: boolean): void {
    if (!this.deps.link.ready) return; // sent again on ready
    this.deps.link.request("keepListening", { on }).catch((e: unknown) => {
      if (!on) return;
      this.deps.ui.message(`Keep listening was refused: ${e instanceof Error ? e.message : String(e)}`);
      this.fallBack();
    });
  }

  private onHls(e: GatewayEvent): void {
    if (typeof e.vrx !== "number") return;
    // The gateway encodes HLS for the session's audio selection, so a URL
    // for another id means the selection moved (setMode, setBandwidth).
    if (e.vrx !== this.following) {
      if (typeof e.url !== "string") return;
      this.following = e.vrx;
    }
    this.clearGrace();
    if (typeof e.url === "string") this.deps.hls.setUrl(e.url);
    else this.deps.hls.setUrl(null, typeof e.error === "string" ? e.error : undefined);
  }

  private onHlsStatus(s: HlsStatus): void {
    if (s.kind === "playing" && this.holdWhenHlsPlays) {
      this.holdWhenHlsPlays = false;
      this.deps.live.hold();
    }
    if (s.kind === "failed" && this.handoff.state !== "Live") {
      this.deps.ui.message(`Background audio failed: ${s.error}`);
      this.fallBack();
    }
    this.updateMedia();
  }

  // HLS cannot play. In the foreground live takes over; with the phone
  // locked there is nothing to hand to, and the message says why.
  private fallBack(): void {
    if (this.deps.hidden()) return;
    this.handoff.keepListeningOff();
    this.deps.ui.keep(this.handoff.keepListening);
  }

  private gone(why: string): void {
    this.clearGrace();
    if (!this.handoff.keepListening && this.handoff.state === "Live") return;
    this.handoff.receiverGone();
    this.following = null;
    this.deps.ui.message(`Keep listening stopped: ${why}.`);
    this.deps.ui.keep(false);
  }

  private clearGrace(): void {
    if (this.grace !== null) this.deps.clearTimeout(this.grace);
    this.grace = null;
  }

  private updateMedia(): void {
    const media = this.deps.media;
    if (!media) return;
    if (this.now) {
      const mhz = (this.now.hz / 1e6).toFixed(4);
      media.setMetadata(`${mhz} MHz ${this.now.mode.toUpperCase()}`, "Revenant");
    }
    media.setPlaybackState(this.deps.hls.playing ? "playing" : this.handoff.state === "Live" ? "none" : "paused");
  }
}

export function browserMediaSession(): MediaSessionLike | null {
  if (typeof navigator === "undefined" || !("mediaSession" in navigator)) return null;
  const ms = navigator.mediaSession;
  return {
    setMetadata(title, artist) {
      ms.metadata = new MediaMetadata({ title, artist, album: "Keep listening", artwork: [{ src: "/icons/icon-512.png", sizes: "512x512", type: "image/png" }] });
    },
    setPlaybackState(state) {
      ms.playbackState = state;
    },
    setActionHandler(action, fn) {
      try {
        ms.setActionHandler(action, fn);
      } catch {
        // An action this browser does not know.
      }
    },
  };
}

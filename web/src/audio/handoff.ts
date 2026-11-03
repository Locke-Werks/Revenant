// Which audio path plays: live Opus (low latency, screen on) or the HLS
// <audio> element (Keep listening, survives a locked phone). A pure state
// machine: inputs are what happened, outputs are commands the glue carries
// out on real elements, so every sequence can be walked in a test.
//
// The rule it exists for: silence is never the result of a handoff. HLS is
// only paused by a cut, and a cut only happens once live is resumed and its
// jitter buffer has filled. The one way HLS stops without live taking over is
// the user asking for it (keep listening off while nothing can play live) or
// the receiver going away, and the glue says why in that case.
//
// The two paths never play together: iOS ignores volume on media elements,
// so there is no crossfade. Live stays muted while it primes; the cut pauses
// HLS and fades live in.

export type HandoffState = "Live" | "Background" | "Priming" | "NeedsTap";

export type HandoffCommand =
  // Tell the gateway to encode (or stop encoding) HLS for the receiver.
  | "hlsOn"
  | "hlsOff"
  | "playHls"
  // Only as part of a cut, when live is ready.
  | "pauseHls"
  // HLS stops with nothing taking over: the user's own request or a lost
  // receiver, never a handoff.
  | "stopHls"
  // Start feeding and resuming live, output muted.
  | "startLive"
  // Drop live frames; HLS carries the audio.
  | "stopLive"
  // Unmute live with a short ramp.
  | "fadeCut"
  | "showGoLive"
  | "hideGoLive"
  | "showBehindBanner"
  | "hideBehindBanner";

export class Handoff {
  private st: HandoffState = "Live";
  private keep = false;
  private shown = true;
  // Priming progress: the buffer has filled, and resume() has answered.
  private ready = false;
  private resumed: boolean | null = null;

  constructor(private readonly emit: (c: HandoffCommand) => void) {}

  get state(): HandoffState {
    return this.st;
  }

  get keepListening(): boolean {
    return this.keep;
  }

  get isVisible(): boolean {
    return this.shown;
  }

  // The user's tap (or the lock screen's play). The tap is the gesture iOS
  // wants before it lets the <audio> element play.
  keepListeningOn(): void {
    if (this.keep) return;
    this.keep = true;
    this.emit("hlsOn");
    if (this.st === "Live") this.toBackground();
  }

  keepListeningOff(): void {
    if (!this.keep) return;
    this.keep = false;
    switch (this.st) {
      case "Live":
        this.emit("hlsOff");
        return;
      case "Background":
        if (this.shown) {
          // Live has to take over before HLS may stop.
          this.prime();
        } else {
          // Off from the lock screen: the user asked for quiet.
          this.emit("stopHls");
          this.emit("hideBehindBanner");
          this.emit("hlsOff");
          this.st = "Live";
        }
        return;
      case "Priming":
      case "NeedsTap":
        // HLS plays on until the cut, which then turns the gateway's
        // encoder off. Asking now would end the stream 30 s later even if
        // the cut never comes.
        return;
    }
  }

  hidden(): void {
    this.shown = false;
    switch (this.st) {
      case "Live":
        // Live dies with the screen on iOS; HLS is what keeps going.
        if (this.keep) this.toBackground();
        return;
      case "Background":
        return;
      case "Priming":
        this.emit("stopLive");
        this.st = "Background";
        return;
      case "NeedsTap":
        this.emit("hideGoLive");
        this.emit("stopLive");
        this.st = "Background";
        return;
    }
  }

  visible(): void {
    this.shown = true;
    if (this.st === "Background") this.prime();
  }

  liveReady(): void {
    if (this.st !== "Priming" && this.st !== "NeedsTap") return;
    this.ready = true;
    if (this.st === "Priming" && this.resumed === true) this.cut();
  }

  resumeResult(ok: boolean): void {
    if (this.st !== "Priming") return;
    this.resumed = ok;
    if (!ok) {
      this.emit("showGoLive");
      this.st = "NeedsTap";
    } else if (this.ready) {
      this.cut();
    }
  }

  // The pill. The glue resumes live inside this tap, since that is the
  // gesture a refused resume was waiting for; if it still fails it reports
  // resumeResult(false) and the pill comes back.
  goLiveTap(): void {
    if (this.st !== "NeedsTap") return;
    this.resumed = true;
    if (this.ready) {
      this.cut();
      return;
    }
    this.emit("hideGoLive");
    this.st = "Priming";
  }

  // The receiver HLS was following is gone (removed, or lost in an engine
  // restart). Nothing is left to play on either path.
  receiverGone(): void {
    const wasHls = this.st !== "Live";
    if (this.st === "NeedsTap") this.emit("hideGoLive");
    if (this.st === "Priming" || this.st === "NeedsTap") this.emit("stopLive");
    if (wasHls) {
      this.emit("stopHls");
      this.emit("hideBehindBanner");
    }
    if (this.keep) this.emit("hlsOff");
    this.keep = false;
    this.st = "Live";
  }

  private toBackground(): void {
    this.emit("stopLive");
    this.emit("playHls");
    this.emit("showBehindBanner");
    this.st = "Background";
  }

  private prime(): void {
    this.ready = false;
    this.resumed = null;
    this.emit("startLive");
    this.st = "Priming";
  }

  private cut(): void {
    if (this.st === "NeedsTap") this.emit("hideGoLive");
    this.emit("fadeCut");
    this.emit("pauseHls");
    this.emit("hideBehindBanner");
    if (!this.keep) this.emit("hlsOff");
    this.st = "Live";
  }
}

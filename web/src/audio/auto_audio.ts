// Turns the phone's audio on by itself once there is something Auto DV made
// to hear: a DV receiver, or Auto DV switched on. Without it a call on a
// receiver the phone never selected is decoded and captioned in silence.
//
// It asks once while audio is off and stays out of the way once it is on.
// Outside a gesture iOS will not start the AudioContext, so the request can
// succeed at the gateway with nothing coming out: then the "tap to play" note
// goes up, and comes down when a later tap gets the context running.

export interface AutoAudioLive {
  readonly on: boolean;
  start(vrx?: number | null): Promise<void>;
  resume(): Promise<boolean>;
  onRunning(fn: () => void): void;
}

export interface AutoAudioDeps {
  audio: AutoAudioLive;
  // The persistent audio note; null clears it.
  note(text: string | null): void;
  notStarted: string;
}

export class AutoAudio {
  private noted = false;
  private asking = false;
  // A refusal is not asked again on every list change; a tap still starts it.
  private refused = false;

  constructor(private readonly deps: AutoAudioDeps) {
    deps.audio.onRunning(() => {
      if (!this.noted) return;
      this.noted = false;
      deps.note(null);
    });
  }

  // wants: a DV receiver exists or Auto DV is on.
  update(wants: boolean): void {
    const audio = this.deps.audio;
    if (!wants || audio.on || this.asking || this.refused) return;
    this.asking = true;
    audio.start(null).then(
      async () => {
        this.asking = false;
        const running = await audio.resume();
        this.noted = !running;
        this.deps.note(running ? null : this.deps.notStarted);
      },
      (e: unknown) => {
        this.asking = false;
        this.refused = true;
        this.deps.note(`Live audio unavailable: ${e instanceof Error ? e.message : String(e)}`);
      },
    );
  }
}

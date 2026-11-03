// Moving the radio's centre with the dial, a waterfall drag or a digit swipe
// while no receiver is active. Each setCenter is a USB retune that can take
// a while, so at most one is in flight; values that arrive meanwhile collapse
// to the newest, which goes out when the reply comes.

import { clampToRanges, type TuneRange } from "./band_edge";

export interface CentreDeps {
  // Resolves when the engine has answered, whatever the answer.
  send(hz: number): Promise<unknown>;
  ranges(): TuneRange[] | null;
  // The centre the readout should show, or null to go back to the view's.
  preview(hz: number | null): void;
}

export class CentreTuner {
  private inFlight = false;
  private pending: number | null = null;
  private shown: number | null = null;

  constructor(private readonly deps: CentreDeps) {}

  // Where the centre is being taken, or null when nothing is.
  get hz(): number | null {
    return this.shown;
  }

  get idle(): boolean {
    return !this.inFlight && this.pending === null;
  }

  set(hz: number): void {
    const to = Math.round(clampToRanges(hz, this.deps.ranges()));
    this.shown = to;
    this.deps.preview(to);
    this.pending = to;
    if (!this.inFlight) this.next();
  }

  // The source now reports a centre: once nothing is moving, the readout
  // goes back to following the view.
  settled(): void {
    if (!this.idle) return;
    this.shown = null;
    this.deps.preview(null);
  }

  private next(): void {
    const hz = this.pending;
    this.pending = null;
    if (hz === null) return;
    this.inFlight = true;
    const done = () => {
      this.inFlight = false;
      this.next();
    };
    this.deps.send(hz).then(done, done);
  }
}

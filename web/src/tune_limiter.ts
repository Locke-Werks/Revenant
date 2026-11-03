// Rate-limits a stream of values headed for the gateway. A drag produces a
// value per pointer event; the engine retunes no faster than this is worth,
// and every request is a round trip. Generic over the send so the dial strip
// can reuse it.

export const TUNE_INTERVAL_MS = 50;

export class TuneLimiter {
  private pending: number | null = null;
  private lastSent = Number.NEGATIVE_INFINITY;
  private cancelTimer: (() => void) | null = null;

  constructor(
    private readonly send: (hz: number) => void,
    private readonly now: () => number,
    private readonly schedule: (fn: () => void, ms: number) => () => void,
  ) {}

  set(hz: number): void {
    this.pending = hz;
    if (this.cancelTimer) return;
    const wait = this.lastSent + TUNE_INTERVAL_MS - this.now();
    if (wait <= 0) {
      this.emit();
      return;
    }
    this.cancelTimer = this.schedule(() => {
      this.cancelTimer = null;
      this.emit();
    }, wait);
  }

  // The release of a drag: the value the finger ended on must reach the
  // engine even if it arrived inside the interval.
  flush(): void {
    this.stopTimer();
    this.emit();
  }

  cancel(): void {
    this.stopTimer();
    this.pending = null;
  }

  private emit(): void {
    if (this.pending === null) return;
    const hz = this.pending;
    this.pending = null;
    this.lastSent = this.now();
    this.send(hz);
  }

  private stopTimer(): void {
    if (this.cancelTimer) {
      this.cancelTimer();
      this.cancelTimer = null;
    }
  }
}

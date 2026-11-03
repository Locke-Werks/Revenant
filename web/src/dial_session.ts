// One run of dial tuning, free of the DOM so it can be tested: the physics,
// the receiver it started on, the unrounded value and the hand-off to the tune
// limiter. The strip feeds it pointer events and animation frames.

import { DialPhysics, fineStep, roundToStep } from "./dial";
import { TuneLimiter } from "./tune_limiter";

export interface DialDeps {
  // The receiver the dial would move, or null when there is none.
  active(): { vrx: number; hz: number; mode: string } | null;
  // Shows hz on the active receiver at once; false when it cannot be dragged.
  preview(hz: number): boolean;
  // One rate-limited tune to receiver vrx, named so a flush after the active
  // receiver changed still reaches the one that was being tuned.
  tune(vrx: number, hz: number): void;
  // The session is over and its last tune has been sent.
  end(): void;
  now(): number;
  schedule(fn: () => void, ms: number): () => void;
}

export class DialSession {
  readonly physics = new DialPhysics("nfm");
  private readonly limiter: TuneLimiter;
  // Where the dial has taken the receiver, unrounded; null outside a session.
  private dialHz: number | null = null;
  // The receiver the session started on. Momentum never moves another one.
  private vrx = -1;
  private touching = false;

  constructor(private readonly deps: DialDeps) {
    this.limiter = new TuneLimiter((hz) => deps.tune(this.vrx, hz), deps.now, deps.schedule);
  }

  get moving(): boolean {
    return this.touching || this.physics.moving;
  }

  get tuning(): boolean {
    return this.dialHz !== null;
  }

  // The unrounded value while a session runs, for smooth drawing.
  get hz(): number | null {
    return this.dialHz;
  }

  down(): void {
    // A finger on a coasting ribbon catches it where it is.
    this.physics.stop();
    this.touching = true;
    // Before the first move, since the step of that move depends on it.
    const r = this.deps.active();
    if (r) this.physics.setMode(r.mode);
  }

  drag(dxPx: number, tMs: number): void {
    this.physics.drag(dxPx, tMs);
    this.pump(tMs);
  }

  release(tMs: number): void {
    this.touching = false;
    this.physics.release(tMs);
    this.pump(tMs);
    if (!this.physics.moving) this.settle();
  }

  // An animation frame while coasting.
  tick(tMs: number): void {
    this.pump(tMs);
    if (!this.moving) this.settle();
  }

  // The active receiver may have changed by any path (a chip, a replace, a
  // removal, a resume). Call after anything that could change it.
  check(): void {
    if (this.dialHz !== null && this.deps.active()?.vrx !== this.vrx) this.stop();
  }

  // Another source took over, or the receiver went: send what was already
  // shown to the receiver it was shown on, then let go.
  stop(): void {
    this.physics.stop();
    this.touching = false;
    if (this.dialHz === null) {
      this.limiter.cancel();
      return;
    }
    this.dialHz = null;
    this.limiter.flush();
    this.deps.end();
  }

  private pump(tMs: number): void {
    this.check();
    const d = this.physics.frame(tMs);
    if (d === 0) return;
    if (this.dialHz === null) {
      const r = this.deps.active();
      if (!r) {
        this.physics.stop();
        return;
      }
      this.dialHz = r.hz;
      this.vrx = r.vrx;
    }
    const next = Math.max(1, this.dialHz + d);
    if (!this.deps.preview(next)) {
      // Nothing to move, so the finger's travel must not pile up unseen.
      this.physics.stop();
      if (this.dialHz !== null) {
        this.dialHz = null;
        this.limiter.flush();
        this.deps.end();
      }
      return;
    }
    this.dialHz = next;
    this.limiter.set(next);
  }

  // The dial came to rest: land on a whole fine step, send it, and let the
  // receiver controller apply the engine's answer.
  private settle(): void {
    if (this.dialHz === null) return;
    const r = this.deps.active();
    const hz = roundToStep(this.dialHz, fineStep(r?.mode ?? "nfm"));
    this.dialHz = null;
    if (r && r.vrx === this.vrx && hz !== r.hz && this.deps.preview(hz)) this.limiter.set(hz);
    this.limiter.flush();
    this.deps.end();
  }
}

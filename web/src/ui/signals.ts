// The Signals tab: the engine's detections as a list, and the confidence bar.

import { fitBandwidth, sortForList, type Detection } from "../detections";
import { BANDWIDTHS_HZ, formatMHz } from "./receivers";

// The gateway refuses a negative bar or one at 1.0 or above.
export const CONFIDENCE_MIN = 0;
export const CONFIDENCE_MAX = 0.99;
export const CONFIDENCE_SEND_MS = 250;

export function signalRows(d: Detection[]): Detection[] {
  return sortForList(d);
}

// lastSeen is an absolute source sample index (docs/conventions.md), not a
// time, so the age is the gap to the newest index the phone has seen divided
// by the source rate. Without both there is nothing honest to show.
export function heardAgo(lastSeen: number, nowIndex: number | null, rateHz: number): string {
  if (nowIndex === null || !(rateHz > 0)) return "";
  const s = Math.max(0, (nowIndex - lastSeen) / rateHz);
  if (s < 1) return "now";
  if (s < 60) return `${Math.floor(s)} s ago`;
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  return `${Math.floor(s / 3600)} h ago`;
}

export interface PickDeps {
  hasActiveReceiver(): boolean;
  stopDial(): void;
  tuneFit(hz: number, bandwidthHz: number): void;
  // Pans the view when hz is off screen.
  reveal(hz: number): void;
  toast(text: string): void;
}

export function pickSignal(d: Detection, deps: PickDeps): void {
  if (!deps.hasActiveReceiver()) {
    deps.toast("Add a receiver first: tap the waterfall.");
    return;
  }
  deps.stopDial();
  deps.tuneFit(d.centerHz, fitBandwidth(d.bandwidthHz, BANDWIDTHS_HZ));
  deps.reveal(d.centerHz);
}

// The first value goes at once, later ones collapse to the newest and go when
// the interval is up, so a drag of the slider is a few requests and ends on
// the value the finger stopped at.
export class RateLimitedSender {
  private pending: number | null = null;
  private lastSent = Number.NEGATIVE_INFINITY;
  private cancelTimer: (() => void) | null = null;

  constructor(
    private readonly send: (v: number) => void,
    private readonly now: () => number,
    private readonly schedule: (fn: () => void, ms: number) => () => void,
    private readonly intervalMs = CONFIDENCE_SEND_MS,
  ) {}

  set(v: number): void {
    this.pending = v;
    if (this.cancelTimer) return;
    const wait = this.lastSent + this.intervalMs - this.now();
    if (wait <= 0) {
      this.emit();
      return;
    }
    this.cancelTimer = this.schedule(() => {
      this.cancelTimer = null;
      this.emit();
    }, wait);
  }

  private emit(): void {
    if (this.pending === null) return;
    const v = this.pending;
    this.pending = null;
    this.lastSent = this.now();
    this.send(v);
  }
}

export interface SignalsDeps extends PickDeps {
  setMinConfidence(v: number): void;
  // Newest source sample index and the source rate, for the age column.
  clock(): { index: number; rateHz: number } | null;
}

export class SignalsPanel {
  readonly el: HTMLElement;
  private readonly list: HTMLElement;
  private readonly empty: HTMLElement;
  private readonly out: HTMLOutputElement;
  private latest: Detection[] = [];

  constructor(private readonly deps: SignalsDeps) {
    this.el = document.createElement("section");
    this.el.className = "panel signals";
    this.el.innerHTML = `
<div class="row">
  <label for="sg-conf">Confidence</label>
  <input type="range" id="sg-conf" min="${CONFIDENCE_MIN}" max="${CONFIDENCE_MAX}" step="0.01" value="${CONFIDENCE_MIN}">
  <output id="sg-conf-out" for="sg-conf">0.00</output>
</div>
<p class="hint" id="sg-empty">No signals yet.</p>
<ul class="signal-list" id="sg-list"></ul>`;
    this.list = this.el.querySelector("#sg-list")!;
    this.empty = this.el.querySelector("#sg-empty")!;
    this.out = this.el.querySelector("#sg-conf-out")!;
    const slider = this.el.querySelector<HTMLInputElement>("#sg-conf")!;
    const limiter = new RateLimitedSender(
      (v) => deps.setMinConfidence(v),
      () => performance.now(),
      (fn, ms) => {
        const h = window.setTimeout(fn, ms);
        return () => window.clearTimeout(h);
      },
    );
    slider.addEventListener("input", () => {
      const v = Math.min(CONFIDENCE_MAX, Math.max(CONFIDENCE_MIN, Number(slider.value)));
      this.out.textContent = v.toFixed(2);
      limiter.set(v);
    });
  }

  update(d: Detection[]): void {
    this.latest = d;
    this.render();
  }

  render(): void {
    const rows = signalRows(this.latest);
    this.empty.hidden = rows.length > 0;
    this.list.textContent = "";
    const clock = this.deps.clock();
    for (const d of rows) {
      const li = document.createElement("li");
      const b = document.createElement("button");
      b.type = "button";
      b.className = "signal";
      b.classList.toggle("held", d.state === "held");
      const name = d.label !== "" ? d.label.toUpperCase() : "Signal";
      const ago = heardAgo(d.lastSeen, clock ? clock.index : null, clock ? clock.rateHz : 0);
      const bw = d.bandwidthHz >= 1000 ? `${(d.bandwidthHz / 1000).toFixed(1)} kHz` : `${Math.round(d.bandwidthHz)} Hz`;
      const parts = [`${formatMHz(d.centerHz)} MHz`, name, bw, `${Math.round(d.snrDb)} dB`];
      if (d.state === "held") parts.push("held");
      if (ago !== "") parts.push(ago);
      b.textContent = parts.join("  ");
      b.addEventListener("click", () => pickSignal(d, this.deps));
      li.append(b);
      this.list.append(li);
    }
  }
}

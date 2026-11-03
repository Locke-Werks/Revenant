// The top bar: the active receiver's frequency (tap to type one), its mode
// chip, and an S-meter.

import type { SpectrumFrame } from "../protocol";
import { formatMHz, readFrequency, type Receiver } from "./receivers";

// The meter's scale, dBFS. The bar is empty at the bottom and full at the top.
export const METER_FLOOR_DB = -120;
export const METER_TOP_DB = -20;

// The strongest bin of frame inside lowHz..highHz, in dB, or null when the
// band is outside the frame. The gateway sends no receiver level, and the
// spectrum row the phone already has answers the question the meter asks.
export function levelInBand(frame: SpectrumFrame, lowHz: number, highHz: number): number | null {
  const n = frame.px.length;
  if (n === 0 || !(frame.spanHz > 0)) return null;
  const left = frame.centerHz - frame.spanHz / 2;
  const binHz = frame.spanHz / n;
  let a = Math.floor((lowHz - left) / binHz);
  let b = Math.ceil((highHz - left) / binHz) - 1;
  if (b < 0 || a >= n) return null;
  a = Math.max(0, a);
  b = Math.min(n - 1, Math.max(a, b));
  let peak = 0;
  for (let i = a; i <= b; i++) peak = Math.max(peak, frame.px[i]!);
  return frame.offsetDb + peak * frame.scaleDb;
}

// Vertical travel that moves a digit by one.
export const SWIPE_STEP_PX = 16;

// One step of the digit at 10^place Hz, never below zero.
export function digitStep(hz: number, place: number, dir: 1 | -1): number {
  return Math.max(0, hz + dir * 10 ** place);
}

export interface ReadoutDigit {
  ch: string;
  // Power of ten, in Hz, this digit stands for; null for the point.
  index: number | null;
}

// formatMHz carries 100 Hz, so its last digit is the 10^2 place.
export function readoutDigits(hz: number): ReadoutDigit[] {
  const text = formatMHz(hz);
  let place = 2;
  const out: ReadoutDigit[] = [];
  for (let i = text.length - 1; i >= 0; i--) {
    const ch = text[i]!;
    out.unshift({ ch, index: ch === "." ? null : place++ });
  }
  return out;
}

export interface DigitSwipeDeps {
  // Start of a swipe: the receiver's current hz, or null when there is none.
  begin(): number | null;
  step(hz: number): void;
  end(): void;
}

// Turns vertical travel on one digit into tunes. Steps are counted from where
// the finger came down, so going back to the start restores the frequency.
export class DigitSwipe {
  private place: number | null = null;
  private startY = 0;
  private base: number | null = null;
  private started = false;
  private last = Number.NaN;

  constructor(private readonly deps: DigitSwipeDeps) {}

  down(place: number, y: number): void {
    this.place = place;
    this.startY = y;
    this.base = null;
    this.started = false;
    this.last = Number.NaN;
  }

  move(y: number): void {
    if (this.place === null) return;
    const steps = Math.trunc((this.startY - y) / SWIPE_STEP_PX);
    if (!this.started) {
      if (steps === 0) return;
      // Deferred to the first step so a plain tap never stops the dial.
      this.base = this.deps.begin();
      if (this.base === null) {
        this.place = null;
        return;
      }
      this.started = true;
    }
    const next = Math.max(0, this.base! + steps * 10 ** this.place);
    if (next === this.last) return;
    this.last = next;
    this.deps.step(next);
  }

  // True when the touch was a swipe, which the caller must not treat as a tap.
  up(): boolean {
    const was = this.started;
    if (was) this.deps.end();
    this.place = null;
    this.started = false;
    return was;
  }
}

export interface TopBarDeps {
  tune(hz: number): void;
  openSheet(): void;
  toast(text: string): void;
  // Digit swipes: the receiver's hz at the first step, then each previewed
  // value, then the release.
  swipeBegin(): number | null;
  swipeStep(hz: number): void;
  swipeEnd(): void;
}

export class TopBar {
  readonly el: HTMLElement;
  private readonly freq: HTMLButtonElement;
  private readonly entry: HTMLFormElement;
  private readonly input: HTMLInputElement;
  private readonly mode: HTMLButtonElement;
  private readonly meterFill: HTMLElement;
  private active: Receiver | null = null;
  private readonly swipe: DigitSwipe;
  private swiped = false;

  constructor(private readonly deps: TopBarDeps) {
    this.el = document.createElement("header");
    this.el.className = "topbar";
    this.el.innerHTML = `
<button type="button" class="freq" id="tb-freq" aria-label="Frequency, tap to type">--- MHz</button>
<form class="freq-entry" id="tb-entry" hidden>
  <input id="tb-input" inputmode="decimal" enterkeyhint="go" autocomplete="off" aria-label="Frequency, MHz or with k, M or G">
</form>
<button type="button" class="chip mode" id="tb-mode" aria-label="Mode, opens the controls">NFM</button>
<div class="meter" role="meter" aria-label="Signal, peak in view" aria-valuemin="${METER_FLOOR_DB}" aria-valuemax="${METER_TOP_DB}">
  <div class="meter-fill" id="tb-meter"></div>
</div>`;
    this.freq = this.el.querySelector("#tb-freq")!;
    this.entry = this.el.querySelector("#tb-entry")!;
    this.input = this.el.querySelector("#tb-input")!;
    this.mode = this.el.querySelector("#tb-mode")!;
    this.meterFill = this.el.querySelector("#tb-meter")!;

    this.swipe = new DigitSwipe({ begin: deps.swipeBegin, step: deps.swipeStep, end: deps.swipeEnd });
    this.freq.addEventListener("pointerdown", (ev) => {
      this.swiped = false;
      const digit = (ev.target as Element).closest<HTMLElement>("[data-place]");
      if (!digit) return;
      this.swipe.down(Number(digit.dataset.place), ev.clientY);
      // Keeps the moves coming when the finger leaves the small digit.
      this.freq.setPointerCapture(ev.pointerId);
    });
    this.freq.addEventListener("pointermove", (ev) => this.swipe.move(ev.clientY));
    this.freq.addEventListener("pointerup", () => {
      this.swiped = this.swipe.up();
    });
    this.freq.addEventListener("pointercancel", () => {
      this.swipe.up();
    });
    // The click that follows a swipe's release must not open typed entry.
    this.freq.addEventListener("click", () => {
      if (this.swiped) {
        this.swiped = false;
        return;
      }
      this.edit();
    });
    this.mode.addEventListener("click", () => deps.openSheet());
    this.entry.addEventListener("submit", (ev) => {
      ev.preventDefault();
      const text = this.input.value;
      const f = readFrequency(text);
      this.close();
      if ("error" in f) {
        if (text.trim() !== "") deps.toast(f.error);
        return;
      }
      deps.tune(f.hz);
    });
    this.input.addEventListener("blur", () => this.close());
  }

  show(r: Receiver | null): void {
    this.active = r;
    this.freq.textContent = "";
    if (r) {
      for (const d of readoutDigits(r.hz)) {
        const s = document.createElement("span");
        s.textContent = d.ch;
        if (d.index !== null) {
          s.className = "digit";
          s.dataset.place = String(d.index);
        }
        this.freq.append(s);
      }
      this.freq.append(" MHz");
    } else {
      this.freq.textContent = "--- MHz";
    }
    this.mode.textContent = (r?.mode ?? "nfm").toUpperCase();
    this.mode.hidden = r === null;
    if (!r) this.level(null);
  }

  level(db: number | null): void {
    const meter = this.el.querySelector<HTMLElement>(".meter")!;
    if (db === null || !this.active) {
      this.meterFill.style.transform = "scaleX(0)";
      meter.removeAttribute("aria-valuenow");
      return;
    }
    const f = Math.min(1, Math.max(0, (db - METER_FLOOR_DB) / (METER_TOP_DB - METER_FLOOR_DB)));
    this.meterFill.style.transform = `scaleX(${f})`;
    meter.setAttribute("aria-valuenow", String(Math.round(db)));
  }

  private edit(): void {
    this.input.value = this.active ? formatMHz(this.active.hz) : "";
    this.freq.hidden = true;
    this.entry.hidden = false;
    this.input.focus();
    this.input.select();
  }

  private close(): void {
    this.entry.hidden = true;
    this.freq.hidden = false;
  }
}

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

export interface TopBarDeps {
  tune(hz: number): void;
  openSheet(): void;
  toast(text: string): void;
}

export class TopBar {
  readonly el: HTMLElement;
  private readonly freq: HTMLButtonElement;
  private readonly entry: HTMLFormElement;
  private readonly input: HTMLInputElement;
  private readonly mode: HTMLButtonElement;
  private readonly meterFill: HTMLElement;
  private active: Receiver | null = null;

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

    this.freq.addEventListener("click", () => this.edit());
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
    this.freq.textContent = r ? `${formatMHz(r.hz)} MHz` : "--- MHz";
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

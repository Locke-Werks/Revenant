// The bottom sheet (a side panel in landscape): tuning for the active
// receiver, keep listening, and the radio panel.

import { BANDWIDTHS_HZ, formatMHz, MODES, type Receiver } from "./receivers";

export const SQUELCH_MIN_DBFS = -140;
export const SQUELCH_MAX_DBFS = 0;

export interface SheetDeps {
  setMode(mode: string): void;
  setBandwidth(hz: number): void;
  setSquelch(dbfs: number): void;
  remove(vrx: number): void;
  // Runs in the tap: the HLS element needs the gesture.
  toggleKeepListening(): void;
}

export function sheetHint(others: boolean): string {
  return others ? "Tap a receiver to listen" : "Tap the waterfall to listen there; press and hold to add another receiver.";
}

function bandwidthLabel(hz: number): string {
  return hz >= 1000 ? `${hz / 1000} kHz` : `${hz} Hz`;
}

export class Sheet {
  readonly el: HTMLElement;
  // Where the radio panel and, later, the decode feed go.
  readonly extra: HTMLElement;
  private readonly title: HTMLElement;
  private readonly tuning: HTMLElement;
  private readonly modes: HTMLElement;
  private readonly bandwidth: HTMLSelectElement;
  private readonly squelch: HTMLInputElement;
  private readonly squelchOut: HTMLOutputElement;
  private readonly keep: HTMLButtonElement;
  private readonly removeBtn: HTMLButtonElement;
  private active: Receiver | null = null;
  private openListeners = new Set<(open: boolean) => void>();

  constructor(deps: SheetDeps) {
    this.el = document.createElement("section");
    this.el.className = "sheet";
    this.el.id = "sheet";
    this.el.hidden = true;
    this.el.setAttribute("aria-label", "Controls");
    this.el.innerHTML = `
<div class="sheet-head">
  <h2 id="sh-title">No receiver</h2>
  <button type="button" class="icon-btn" id="sh-close" aria-label="Close controls">×</button>
</div>
<div class="sheet-body">
  <section class="panel" id="sh-tuning">
    <div class="modes" role="group" aria-label="Mode" id="sh-modes"></div>
    <div class="row">
      <label for="sh-bw">Bandwidth</label>
      <select id="sh-bw"></select>
    </div>
    <div class="row">
      <label for="sh-sq">Squelch</label>
      <input type="range" id="sh-sq" min="${SQUELCH_MIN_DBFS}" max="${SQUELCH_MAX_DBFS}" step="1">
      <output id="sh-sq-out" for="sh-sq"></output>
    </div>
    <div class="row">
      <button type="button" class="btn toggle" id="sh-keep" aria-pressed="false">Keep listening</button>
      <button type="button" class="btn danger" id="sh-remove">Remove receiver</button>
    </div>
  </section>
  <p class="hint" id="sh-hint">Tap the waterfall to listen there; press and hold to add another receiver.</p>
  <div id="sh-extra"></div>
</div>`;
    this.title = this.el.querySelector("#sh-title")!;
    this.tuning = this.el.querySelector("#sh-tuning")!;
    this.modes = this.el.querySelector("#sh-modes")!;
    this.bandwidth = this.el.querySelector("#sh-bw")!;
    this.squelch = this.el.querySelector("#sh-sq")!;
    this.squelchOut = this.el.querySelector("#sh-sq-out")!;
    this.keep = this.el.querySelector("#sh-keep")!;
    this.removeBtn = this.el.querySelector("#sh-remove")!;
    this.extra = this.el.querySelector("#sh-extra")!;

    for (const m of MODES) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "chip";
      b.dataset.mode = m;
      b.textContent = m.toUpperCase();
      b.setAttribute("aria-pressed", "false");
      b.addEventListener("click", () => {
        if (this.active && this.active.mode !== m) deps.setMode(m);
      });
      this.modes.append(b);
    }
    for (const hz of BANDWIDTHS_HZ) {
      const o = document.createElement("option");
      o.value = String(hz);
      o.textContent = bandwidthLabel(hz);
      this.bandwidth.append(o);
    }
    this.bandwidth.addEventListener("change", () => deps.setBandwidth(Number(this.bandwidth.value)));
    this.squelch.addEventListener("input", () => (this.squelchOut.textContent = `${this.squelch.value} dB`));
    this.squelch.addEventListener("change", () => deps.setSquelch(Number(this.squelch.value)));
    this.keep.addEventListener("click", () => deps.toggleKeepListening());
    this.removeBtn.addEventListener("click", () => {
      if (this.active) deps.remove(this.active.vrx);
    });
    this.el.querySelector("#sh-close")!.addEventListener("click", () => this.setOpen(false));
    this.show(null);
  }

  get open(): boolean {
    return !this.el.hidden;
  }

  setOpen(open: boolean): void {
    this.el.hidden = !open;
    for (const fn of this.openListeners) fn(open);
  }

  onOpen(fn: (open: boolean) => void): void {
    this.openListeners.add(fn);
  }

  keepListening(on: boolean): void {
    this.keep.setAttribute("aria-pressed", String(on));
  }

  // others: receivers exist though none is selected, so the hint points at
  // the chips rather than the waterfall.
  show(r: Receiver | null, others = false): void {
    this.active = r;
    this.tuning.hidden = r === null;
    const hint = this.el.querySelector<HTMLElement>("#sh-hint")!;
    hint.hidden = r !== null;
    hint.textContent = sheetHint(others);
    this.title.textContent = r ? `${formatMHz(r.hz)} MHz ${r.mode.toUpperCase()}` : "No receiver";
    if (!r) return;
    for (const b of Array.from(this.modes.querySelectorAll<HTMLButtonElement>("button"))) {
      b.setAttribute("aria-pressed", String(b.dataset.mode === r.mode));
    }
    if (r.bandwidthHz !== null) {
      // A width the engine granted that is not in the list is shown too.
      if (!Array.from(this.bandwidth.options).some((o) => Number(o.value) === r.bandwidthHz)) {
        const o = document.createElement("option");
        o.value = String(r.bandwidthHz);
        o.textContent = bandwidthLabel(r.bandwidthHz);
        this.bandwidth.append(o);
      }
      this.bandwidth.value = String(r.bandwidthHz);
    }
    if (r.squelchDbfs !== null && document.activeElement !== this.squelch) {
      this.squelch.value = String(Math.round(r.squelchDbfs));
      this.squelchOut.textContent = `${Math.round(r.squelchDbfs)} dB`;
    }
  }
}

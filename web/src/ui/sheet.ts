// The bottom sheet (a side panel in landscape): tuning for the active
// receiver, keep listening, and the radio panel.

import { BANDWIDTHS_HZ, MODES, TRANSCRIBE_CHOICES, type Receiver, type TranscribeChoice } from "./receivers";
import { tabAfterDvChange } from "../dv";
import { decodersFor, modeLabel, toggled, type DecoderInfo } from "../decoders";

export const SQUELCH_MIN_DBFS = -140;
export const SQUELCH_MAX_DBFS = 0;

export interface SheetDeps {
  setMode(mode: string): void;
  setBandwidth(hz: number): void;
  setSquelch(dbfs: number): void;
  setTranscribe(choice: TranscribeChoice): void;
  setDecoders(names: string[]): void;
  remove(vrx: number): void;
  // Runs in the tap: the HLS element needs the gesture.
  toggleKeepListening(): void;
}

export const SHEET_TABS = ["tune", "signals", "dv", "radio"] as const;
export type SheetTab = (typeof SHEET_TABS)[number];
export const TAB_LABELS: Record<SheetTab, string> = { tune: "Tune", signals: "Signals", dv: "DV", radio: "Radio" };

export const TAB_KEY = "revenant.tab";

// The tab the bottom pane opens on: the last one shown, else Tune.
export function readTab(stored: string | null): SheetTab {
  return SHEET_TABS.find((t) => t === stored) ?? "tune";
}

export function sheetHint(others: boolean): string {
  return others ? "Tap a receiver to listen" : "Tap the waterfall to listen there; press and hold to add another receiver.";
}

function bandwidthLabel(hz: number): string {
  return hz >= 1000 ? `${hz / 1000} kHz` : `${hz} Hz`;
}

export class Sheet {
  readonly el: HTMLElement;
  // Where the decode feed goes, at the foot of the Tune tab.
  readonly extra: HTMLElement;
  readonly signalsHost: HTMLElement;
  readonly radioHost: HTMLElement;
  readonly dvHost: HTMLElement;
  private dvShown = false;
  private readonly panels: Record<SheetTab, HTMLElement>;
  private tab: SheetTab = "tune";
  private tabListeners = new Set<(tab: SheetTab) => void>();
  private readonly tuning: HTMLElement;
  private readonly modes: HTMLElement;
  private readonly bandwidth: HTMLSelectElement;
  private readonly squelch: HTMLInputElement;
  private readonly squelchOut: HTMLOutputElement;
  private readonly speech: HTMLSelectElement;
  private readonly speechLineEl: HTMLElement;
  private readonly keep: HTMLButtonElement;
  private readonly removeBtn: HTMLButtonElement;
  private readonly decoderRow: HTMLElement;
  private readonly decoderChips: HTMLElement;
  private catalogue: readonly DecoderInfo[] = [];
  private readonly setDecoders: (names: string[]) => void;
  private active: Receiver | null = null;

  constructor(deps: SheetDeps) {
    this.el = document.createElement("section");
    this.el.className = "sheet";
    this.el.id = "sheet";
    this.el.setAttribute("aria-label", "Controls");
    this.el.innerHTML = `
<div class="tabs" role="tablist" aria-label="Controls" id="sh-tabs"></div>
<div class="sheet-body">
  <div id="sh-tab-tune" role="tabpanel" aria-label="Tune">
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
      <label for="sh-speech">Speech</label>
      <select id="sh-speech" aria-label="Transcribe this receiver"></select>
    </div>
    <div class="row decoders-row" id="sh-dec-row">
      <span class="row-label" id="sh-dec-label">Decoders</span>
      <div class="modes" role="group" aria-labelledby="sh-dec-label" id="sh-decoders"></div>
    </div>
    <div class="row">
      <button type="button" class="btn toggle" id="sh-keep" aria-pressed="false">Keep listening</button>
      <button type="button" class="btn danger" id="sh-remove">Remove receiver</button>
    </div>
  </section>
  <p class="hint" id="sh-hint">Tap the waterfall to listen there; press and hold to add another receiver.</p>
  <p class="panel-status" id="sh-speech-line"></p>
  <div id="sh-extra"></div>
  </div>
  <div id="sh-tab-signals" role="tabpanel" aria-label="Signals" hidden></div>
  <div id="sh-tab-dv" role="tabpanel" aria-label="DV" hidden></div>
  <div id="sh-tab-radio" role="tabpanel" aria-label="Radio" hidden></div>
</div>`;
    this.tuning = this.el.querySelector("#sh-tuning")!;
    this.modes = this.el.querySelector("#sh-modes")!;
    this.bandwidth = this.el.querySelector("#sh-bw")!;
    this.squelch = this.el.querySelector("#sh-sq")!;
    this.squelchOut = this.el.querySelector("#sh-sq-out")!;
    this.speech = this.el.querySelector("#sh-speech")!;
    this.speechLineEl = this.el.querySelector("#sh-speech-line")!;
    this.keep = this.el.querySelector("#sh-keep")!;
    this.removeBtn = this.el.querySelector("#sh-remove")!;
    this.decoderRow = this.el.querySelector("#sh-dec-row")!;
    this.decoderChips = this.el.querySelector("#sh-decoders")!;
    this.setDecoders = (names) => deps.setDecoders(names);
    this.extra = this.el.querySelector("#sh-extra")!;
    this.signalsHost = this.el.querySelector("#sh-tab-signals")!;
    this.radioHost = this.el.querySelector("#sh-tab-radio")!;
    this.dvHost = this.el.querySelector("#sh-tab-dv")!;
    const panels: Record<SheetTab, HTMLElement> = {
      tune: this.el.querySelector("#sh-tab-tune")!,
      signals: this.signalsHost,
      dv: this.dvHost,
      radio: this.radioHost,
    };
    const tabs = this.el.querySelector("#sh-tabs")!;
    for (const t of SHEET_TABS) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "chip tab";
      b.dataset.tab = t;
      b.textContent = TAB_LABELS[t];
      b.setAttribute("role", "tab");
      b.addEventListener("click", () => this.selectTab(t));
      tabs.append(b);
    }
    this.panels = panels;
    this.selectTab("tune");
    this.setDvShown(false);

    for (const m of MODES) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "chip";
      b.dataset.mode = m;
      b.textContent = modeLabel(m);
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
    const labels: Record<TranscribeChoice, string> = { auto: "Auto", on: "On", off: "Off" };
    for (const c of TRANSCRIBE_CHOICES) {
      const o = document.createElement("option");
      o.value = c;
      o.textContent = labels[c];
      this.speech.append(o);
    }
    this.speech.addEventListener("change", () => deps.setTranscribe(this.speech.value as TranscribeChoice));
    this.keep.addEventListener("click", () => deps.toggleKeepListening());
    this.removeBtn.addEventListener("click", () => {
      if (this.active) deps.remove(this.active.vrx);
    });
    this.show(null);
  }

  // The DV tab exists only while a digital voice receiver does. Hiding it
  // while it is open falls back to Tune.
  setDvShown(shown: boolean): void {
    this.dvShown = shown;
    const b = this.el.querySelector<HTMLButtonElement>('#sh-tabs button[data-tab="dv"]');
    if (b) b.hidden = !shown;
    const next = tabAfterDvChange(this.tab, shown);
    if (next !== this.tab) this.selectTab(next);
  }

  get currentTab(): SheetTab {
    return this.tab;
  }

  selectTab(tab: SheetTab): void {
    if (tab === "dv" && !this.dvShown) tab = "tune";
    this.tab = tab;
    for (const t of SHEET_TABS) this.panels[t].hidden = t !== tab;
    for (const b of Array.from(this.el.querySelectorAll<HTMLButtonElement>("#sh-tabs button"))) {
      b.setAttribute("aria-selected", String(b.dataset.tab === tab));
    }
    for (const fn of this.tabListeners) fn(tab);
  }

  onTab(fn: (tab: SheetTab) => void): void {
    this.tabListeners.add(fn);
  }

  // The engine-wide speech state, as a sentence.
  speechLine(text: string): void {
    this.speechLineEl.textContent = text;
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
    this.speech.value = r.transcribe;
    this.speech.title = r.transcribing ? "Being transcribed now" : "";
    if (r.squelchDbfs !== null && document.activeElement !== this.squelch) {
      this.squelch.value = String(Math.round(r.squelchDbfs));
      this.squelchOut.textContent = `${Math.round(r.squelchDbfs)} dB`;
    }
    this.renderDecoders(r);
  }

  // The engine's decoders, from listDecoders.
  setCatalogue(all: readonly DecoderInfo[]): void {
    this.catalogue = all;
    this.renderDecoders(this.active);
  }

  // One toggle per decoder that reads the receiver's mode. Rebuilt only when
  // the offered names change, so a tap's pressed state is not lost mid-render.
  private renderDecoders(r: Receiver | null): void {
    const offered = r ? decodersFor(r.mode, this.catalogue) : [];
    this.decoderRow.hidden = offered.length === 0;
    const key = offered.map((d) => d.name).join(",");
    if (this.decoderChips.dataset.names !== key) {
      this.decoderChips.dataset.names = key;
      this.decoderChips.textContent = "";
      for (const d of offered) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "chip";
        b.dataset.decoder = d.name;
        b.textContent = d.name.toUpperCase();
        b.title = d.description;
        b.addEventListener("click", () => {
          if (this.active) this.setDecoders(toggled(this.active.decoders, d.name));
        });
        this.decoderChips.append(b);
      }
    }
    for (const b of Array.from(this.decoderChips.querySelectorAll<HTMLButtonElement>("button"))) {
      b.setAttribute("aria-pressed", String(r !== null && r.decoders.includes(b.dataset.decoder ?? "")));
    }
  }
}

// The radio panel in the sheet: which source is open, opening another, the
// centre frequency, and gain.

import type { JsonValue, SourceEvent } from "../protocol";
import type { GateLink } from "./receivers";
import { formatMHz, parseFrequency } from "./receivers";

// Opening a device or enumerating them waits on USB and driver start-up,
// which can run well past an ordinary request's 10 s.
export const OPEN_SOURCE_TIMEOUT_MS = 30_000;

interface GainStage {
  name: string;
  minDb: number;
  maxDb: number;
  hasAuto: boolean;
  inForceDb: number | null;
  inForceAuto: boolean;
}

interface SourceEntry {
  uri: string;
  name: string;
  available: boolean;
  unavailable?: string;
  gainStages: GainStage[];
}

function entries(v: JsonValue): SourceEntry[] {
  const list = (v as { sources?: unknown } | null)?.sources;
  return Array.isArray(list) ? (list as unknown as SourceEntry[]) : [];
}

export interface RadioPanelDeps {
  link: GateLink;
  toast(text: string): void;
}

export class RadioPanel {
  readonly el: HTMLElement;
  private readonly status: HTMLElement;
  private readonly list: HTMLElement;
  private readonly gains: HTMLElement;
  private readonly center: HTMLInputElement;
  private current: SourceEntry | null = null;

  constructor(private readonly deps: RadioPanelDeps) {
    this.el = document.createElement("section");
    this.el.className = "panel";
    this.el.innerHTML = `
<h2>Radio</h2>
<p class="panel-status" id="rp-status">No radio open</p>
<form class="row" id="rp-center-form">
  <label for="rp-center">Centre</label>
  <input id="rp-center" inputmode="decimal" autocomplete="off" placeholder="MHz" aria-label="Centre frequency">
  <button type="submit" class="btn">Set</button>
</form>
<div id="rp-gains"></div>
<div class="row"><button type="button" class="btn" id="rp-list">Find radios</button></div>
<ul class="sources" id="rp-sources"></ul>`;
    this.status = this.el.querySelector("#rp-status")!;
    this.list = this.el.querySelector("#rp-sources")!;
    this.gains = this.el.querySelector("#rp-gains")!;
    this.center = this.el.querySelector("#rp-center")!;
    this.el.querySelector("#rp-list")!.addEventListener("click", () => this.findSources());
    this.el.querySelector("#rp-center-form")!.addEventListener("submit", (ev) => {
      ev.preventDefault();
      const hz = parseFrequency(this.center.value);
      if (hz === null) {
        deps.toast(`"${this.center.value}" is not a frequency`);
        return;
      }
      deps.link.request("setCenter", { hz }).then(
        (r) => {
          const removed = (r as { removed?: unknown } | null)?.removed;
          if (Array.isArray(removed) && removed.length > 0) deps.toast(`The retune removed ${removed.length} receiver(s)`);
          this.center.blur();
        },
        (e: unknown) => deps.toast(e instanceof Error ? e.message : String(e)),
      );
    });
  }

  source(e: SourceEvent): void {
    if (!e.open || typeof e.centerHz !== "number" || typeof e.rateHz !== "number") {
      this.status.textContent = "No radio open";
      return;
    }
    const name = this.current?.name ? `${this.current.name}, ` : "";
    this.status.textContent = `${name}${formatMHz(e.centerHz)} MHz, ${(e.rateHz / 1e6).toFixed(3)} MS/s`;
    if (document.activeElement !== this.center) this.center.value = formatMHz(e.centerHz);
  }

  private findSources(): void {
    this.list.textContent = "Looking...";
    this.deps.link.request("listSources", {}, { timeoutMs: OPEN_SOURCE_TIMEOUT_MS }).then(
      (r) => this.showSources(entries(r)),
      (e: unknown) => {
        this.list.textContent = "";
        this.deps.toast(e instanceof Error ? e.message : String(e));
      },
    );
  }

  private showSources(sources: SourceEntry[]): void {
    this.list.textContent = "";
    if (sources.length === 0) {
      this.list.textContent = "No radios found";
      return;
    }
    for (const s of sources) {
      const li = document.createElement("li");
      const b = document.createElement("button");
      b.type = "button";
      b.className = "btn source";
      b.disabled = !s.available;
      b.textContent = s.name || s.uri;
      b.setAttribute("aria-label", `Open ${s.name || s.uri}`);
      if (!s.available && s.unavailable) b.title = s.unavailable;
      b.addEventListener("click", () => this.open(s));
      li.append(b);
      if (!s.available && s.unavailable) {
        const why = document.createElement("span");
        why.className = "why";
        why.textContent = s.unavailable;
        li.append(why);
      }
      this.list.append(li);
    }
  }

  private open(s: SourceEntry): void {
    this.status.textContent = `Opening ${s.name || s.uri}...`;
    this.deps.link.request("openSource", { uri: s.uri }, { timeoutMs: OPEN_SOURCE_TIMEOUT_MS }).then(
      (r) => {
        const opened = (r as { source?: unknown } | null)?.source;
        this.current = opened && typeof opened === "object" ? (opened as SourceEntry) : null;
        this.showGains();
        this.list.textContent = "";
      },
      (e: unknown) => {
        this.status.textContent = "Open failed";
        this.deps.toast(e instanceof Error ? e.message : String(e));
      },
    );
  }

  private showGains(): void {
    this.gains.textContent = "";
    for (const g of this.current?.gainStages ?? []) {
      const row = document.createElement("div");
      row.className = "row gain";
      const label = document.createElement("label");
      const id = `rp-gain-${g.name}`;
      label.htmlFor = id;
      label.textContent = `${g.name} gain`;
      const slider = document.createElement("input");
      slider.type = "range";
      slider.id = id;
      slider.min = String(g.minDb);
      slider.max = String(g.maxDb);
      slider.step = "any";
      slider.value = String(g.inForceDb ?? g.minDb);
      const value = document.createElement("output");
      value.textContent = `${Number(slider.value).toFixed(1)} dB`;
      slider.addEventListener("input", () => (value.textContent = `${Number(slider.value).toFixed(1)} dB`));
      slider.addEventListener("change", () => {
        this.deps.link.request("setGain", { stage: g.name, db: Number(slider.value) }).then(
          (r) => {
            const granted = (r as { grantedDb?: unknown } | null)?.grantedDb;
            if (typeof granted === "number") {
              slider.value = String(granted);
              value.textContent = `${granted.toFixed(1)} dB`;
            }
          },
          (e: unknown) => this.deps.toast(e instanceof Error ? e.message : String(e)),
        );
      });
      row.append(label, slider, value);
      if (g.hasAuto) {
        const auto = document.createElement("button");
        auto.type = "button";
        auto.className = "btn toggle";
        auto.textContent = "Auto";
        auto.setAttribute("aria-pressed", String(g.inForceAuto));
        auto.setAttribute("aria-label", `${g.name} automatic gain`);
        auto.addEventListener("click", () => {
          const on = auto.getAttribute("aria-pressed") !== "true";
          this.deps.link.request("setGainAuto", { stage: g.name, on }).then(
            () => {
              auto.setAttribute("aria-pressed", String(on));
              slider.disabled = on;
            },
            (e: unknown) => this.deps.toast(e instanceof Error ? e.message : String(e)),
          );
        });
        slider.disabled = g.inForceAuto;
        row.append(auto);
      }
      this.gains.append(row);
    }
  }
}

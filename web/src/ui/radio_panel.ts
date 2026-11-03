// The radio panel in the sheet: which source is open, opening another, the
// centre frequency, and gain.

import type { JsonValue, SourceEvent } from "../protocol";
import type { GateLink } from "./receivers";
import { formatMHz, readFrequency } from "./receivers";
import type { TuneRange } from "../band_edge";

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
  backend: string;
  name: string;
  available: boolean;
  unavailable?: string;
  tuneRanges: TuneRange[];
  sampleRates: number[];
  minRate: number;
  maxRate: number;
  gainStages: GainStage[];
}

// The desktop's defaults when it opens a radio with nothing remembered.
export const DEFAULT_RATE_HZ = 2_400_000;
export const DEFAULT_CENTRE_HZ = 98_100_000;

type OpenFields = Pick<SourceEntry, "uri" | "backend" | "tuneRanges" | "sampleRates" | "minRate" | "maxRate">;

// The engine refuses a tuner opened with no freq=: an R820T left where opening
// put it fails to lock and then cannot tune at all. The synthetic scene
// defaults to 20 MS/s, which this machine renders at a fraction of real time.
export function openUri(s: OpenFields, centreHz: number | null): string {
  const params: string[] = [];
  const rateFits =
    s.minRate <= DEFAULT_RATE_HZ &&
    DEFAULT_RATE_HZ <= s.maxRate &&
    (s.sampleRates.length === 0 || s.sampleRates.includes(DEFAULT_RATE_HZ));
  if (rateFits || s.backend === "synthetic") params.push(`rate=${DEFAULT_RATE_HZ}`);
  const ranges = s.tuneRanges ?? [];
  if (ranges.length > 0) {
    const reaches = (hz: number) => ranges.some((r) => r.lowHz <= hz && hz <= r.highHz);
    let hz: number;
    if (centreHz !== null && reaches(centreHz)) hz = centreHz;
    else if (reaches(DEFAULT_CENTRE_HZ)) hz = DEFAULT_CENTRE_HZ;
    else {
      const first = ranges[0]!;
      hz = Math.min(Math.max(DEFAULT_CENTRE_HZ, first.lowHz), first.highHz);
    }
    params.push(`freq=${Math.round(hz)}`);
  }
  if (params.length === 0) return s.uri;
  return s.uri + (s.uri.includes("?") ? "&" : "?") + params.join("&");
}

function entries(v: JsonValue): SourceEntry[] {
  const list = (v as { sources?: unknown } | null)?.sources;
  return Array.isArray(list) ? (list as unknown as SourceEntry[]) : [];
}

// Stage names come from the driver ("LNA", "IF 1", ...); an id is kept to
// characters a selector and a label's for= can carry.
export function gainId(stage: string): string {
  return `rp-gain-${stage.replace(/[^A-Za-z0-9_-]/g, "_")}`;
}

// Retunes the front end. A receiver the retune dropped arrives as its own
// vrxRemoved with its frequency and reason, so the reply adds nothing to say.
export function setCenter(deps: RadioPanelDeps, hz: number): Promise<boolean> {
  return deps.link.request("setCenter", { hz }).then(
    () => true,
    (e: unknown) => {
      deps.toast(e instanceof Error ? e.message : String(e));
      return false;
    },
  );
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
  private epoch: number | undefined = undefined;
  // openSource answered; the source event for its new stream comes after
  // the reply and must not forget what was just opened.
  private justOpened = false;
  // Where the last open source was tuned, so switching radios keeps the band.
  private lastCentreHz: number | null = null;

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
      const f = readFrequency(this.center.value);
      if ("error" in f) {
        deps.toast(f.error);
        return;
      }
      void setCenter(deps, f.hz).then((ok) => {
        if (ok) this.center.blur();
      });
    });
  }

  // What the open tuner can reach, known only for a source this phone opened.
  tuneRanges(): TuneRange[] | null {
    return this.current ? this.current.tuneRanges ?? [] : null;
  }

  source(e: SourceEvent): void {
    if (!e.open || typeof e.centerHz !== "number" || typeof e.rateHz !== "number") {
      this.status.textContent = "No radio open";
      this.forget();
      return;
    }
    // A new epoch is a different stream: whatever this phone opened before is
    // not what is open now (another client, or an engine restart, changed it).
    if (e.epoch !== this.epoch) {
      if (!this.justOpened) this.forget();
      this.justOpened = false;
      this.epoch = e.epoch;
    }
    if (e.centerHz > 0) this.lastCentreHz = e.centerHz;
    const name = this.current?.name ? `${this.current.name}, ` : "";
    this.status.textContent = `${name}${formatMHz(e.centerHz)} MHz, ${(e.rateHz / 1e6).toFixed(3)} MS/s`;
    if (document.activeElement !== this.center) this.center.value = formatMHz(e.centerHz);
  }

  private forget(): void {
    if (this.current === null) return;
    this.current = null;
    this.showGains();
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
    const uri = openUri(s, this.lastCentreHz);
    this.deps.link.request("openSource", { uri }, { timeoutMs: OPEN_SOURCE_TIMEOUT_MS }).then(
      (r) => {
        const opened = (r as { source?: unknown } | null)?.source;
        this.current = opened && typeof opened === "object" ? (opened as SourceEntry) : null;
        this.justOpened = true;
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
      const id = gainId(g.name);
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

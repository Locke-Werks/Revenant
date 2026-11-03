// The Signals tab: the engine's detections as a list, and the confidence bar.

import { fitBandwidth, labelText, sortForList, type Detection } from "../detections";
import { BANDWIDTHS_HZ, formatMHz, type Receiver } from "./receivers";

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

export function readConfidence(stored: string | null): number {
  const v = stored === null ? Number.NaN : Number(stored);
  if (!Number.isFinite(v)) return CONFIDENCE_MIN;
  return Math.min(CONFIDENCE_MAX, Math.max(CONFIDENCE_MIN, v));
}

export interface RowPlan {
  added: string[];
  removed: string[];
  // Present before and after, with different text.
  updated: string[];
  // Ids top to bottom as they should sit now.
  order: string[];
}

// Keyed diff of the list between events, free of the DOM. The list refreshes
// at about 2 Hz; rebuilding buttons under a finger swallows the tap and
// reordering moves the wrong row under it, so rows are reused by id and the
// order holds still while a pointer is down.
export class RowSet {
  private texts = new Map<string, string>();
  private order: string[] = [];
  private held = false;
  private pending: string[] | null = null;

  apply(rows: Detection[], textOf: (d: Detection) => string): RowPlan {
    const sorted = signalRows(rows);
    const next = new Map<string, string>();
    for (const d of sorted) next.set(d.id, textOf(d));
    const removed = this.order.filter((id) => !next.has(id));
    const added = sorted.filter((d) => !this.texts.has(d.id)).map((d) => d.id);
    const updated = sorted.filter((d) => this.texts.has(d.id) && this.texts.get(d.id) !== next.get(d.id)).map((d) => d.id);
    const sortedIds = sorted.map((d) => d.id);
    if (this.held) {
      const kept = this.order.filter((id) => next.has(id));
      this.order = [...kept, ...added];
      this.pending = this.order.some((id, i) => id !== sortedIds[i]) ? sortedIds : null;
    } else {
      this.order = sortedIds;
      this.pending = null;
    }
    this.texts = next;
    return { added, removed, updated, order: [...this.order] };
  }

  // Returns the order to apply on release, or null when none is waiting.
  hold(on: boolean): string[] | null {
    this.held = on;
    if (on) return null;
    const p = this.pending;
    this.pending = null;
    if (p) this.order = p;
    return p ? [...p] : null;
  }
}

// The desktop's table, ui/models/label_tune.h, so a detection opens in the
// same mode on both. Protocols by core/identify's names, analogue by
// core/detect/label.cpp's. [mode, decoder], decoder "" for none.
const PROTOCOL_TUNES: Record<string, readonly [string, string]> = {
  P25: ["p25p1", "p25p1"],
  "D-STAR": ["dstar", "dstar"],
  TETRA: ["tetra", "tetra"],
  M17: ["p25p1", "m17"],
  DMR: ["dmr", "dmr"],
  POCSAG: ["nfm", "pocsag"],
  "AX.25": ["nfm", "ax25"],
  RTTY: ["usb", "rtty"],
  "SITOR-B": ["usb", "sitor_b"],
  PSK31: ["usb", "psk31"],
  CW: ["cw", "cw"],
  RDS: ["wfm", ""],
};
const ANALOGUE_MODES: Record<string, string> = { AM: "am", NFM: "nfm", WFM: "wfm", CW: "cw", USB: "usb", LSB: "lsb" };
// ui::kFamilySidebandMaxHz: a digital family this narrow fits a USB voice
// channel, where its tones reach the audio.
export const FAMILY_SIDEBAND_MAX_HZ = 3_000;
// ui::kNarrowbandChannelHz, the width rule's line between NFM and WFM.
export const NARROWBAND_CHANNEL_HZ = 25_000;
// The modes whose passband the shared width list describes. Every other mode
// gets the engine's own default, which the list cannot say: USB's is 300 to
// 2700 Hz, offset from the carrier.
export const FITTED_MODES: readonly string[] = ["nfm", "am", "wfm", "raw"];

// Modes the gateway opens with their own decoder already on.
const DV_MODES: readonly string[] = ["p25p1", "dstar", "tetra", "dmr"];

export interface DetectionTune {
  mode: string;
  // Attached after the add; "" for none. Never the decoder a DV mode
  // already brings with it.
  decoder: string;
  // 0 asks the engine for the mode's default passband.
  bandwidthHz: number;
}

// What a receiver spawned on d opens with. The gateway folds digital
// families into "modulation", so a modulation name the analogue table does
// not know is taken as a family, as the desktop would.
export function tuneForDetection(d: Pick<Detection, "label" | "labelKind" | "bandwidthHz">): DetectionTune {
  const name = d.label.toUpperCase();
  let mode = "";
  let decoder = "";
  if (d.labelKind === "protocol") {
    // A refined protocol such as "P25 control" tunes as its base protocol.
    const row = PROTOCOL_TUNES[name] ?? PROTOCOL_TUNES[name.split(" ")[0] ?? ""];
    if (row) [mode, decoder] = row;
  } else if (d.labelKind === "modulation") {
    mode = ANALOGUE_MODES[name] ?? "";
    if (mode === "" && d.bandwidthHz > 0 && d.bandwidthHz <= FAMILY_SIDEBAND_MAX_HZ) mode = "usb";
  }
  if (mode === "") mode = d.bandwidthHz > NARROWBAND_CHANNEL_HZ ? "wfm" : "nfm";
  // A DV receiver is opened with its mode's decoder by the gateway.
  if (DV_MODES.includes(mode) && decoder === mode) decoder = "";
  const bandwidthHz = FITTED_MODES.includes(mode) ? fitBandwidth(d.bandwidthHz, BANDWIDTHS_HZ) : 0;
  return { mode, decoder, bandwidthHz };
}

export function modeForDetection(d: Pick<Detection, "label" | "labelKind" | "bandwidthHz">): string {
  return tuneForDetection(d).mode;
}

// The receiver already sitting on hz: its passband covers it. The nearest
// wins when passbands overlap.
export function receiverCovering(rs: readonly Pick<Receiver, "vrx" | "hz" | "bandwidthHz">[], hz: number): number | null {
  let best: number | null = null;
  let bestGap = Infinity;
  for (const r of rs) {
    if (r.bandwidthHz === null || !(r.bandwidthHz > 0)) continue;
    const gap = Math.abs(r.hz - hz);
    if (gap <= r.bandwidthHz / 2 && gap < bestGap) {
      best = r.vrx;
      bestGap = gap;
    }
  }
  return best;
}

export interface PickDeps {
  receivers(): readonly Pick<Receiver, "vrx" | "hz" | "bandwidthHz">[];
  select(vrx: number): void;
  spawn(hz: number, mode: string, bandwidthHz: number, decoder: string): void;
  stopDial(): void;
  // Pans the view when hz is off screen.
  reveal(hz: number): void;
}

// A row tap opens a receiver of its own on the signal, so whatever the
// active one was listening to keeps going.
export function pickSignal(d: Detection, deps: PickDeps): void {
  deps.stopDial();
  const there = receiverCovering(deps.receivers(), d.centerHz);
  if (there !== null) deps.select(there);
  else {
    const t = tuneForDetection(d);
    deps.spawn(d.centerHz, t.mode, t.bandwidthHz, t.decoder);
  }
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
  // Where the slider starts, and told whenever the user moves it.
  initialConfidence: number;
  saveConfidence(v: number): void;
  // Newest source sample index and the source rate, for the age column.
  clock(): { index: number; rateHz: number } | null;
}

interface RowEls {
  li: HTMLLIElement;
  btn: HTMLButtonElement;
}

export class SignalsPanel {
  readonly el: HTMLElement;
  private readonly list: HTMLElement;
  private readonly empty: HTMLElement;
  private readonly out: HTMLOutputElement;
  private readonly slider: HTMLInputElement;
  private latest: Detection[] = [];
  private visible = false;
  private readonly set = new RowSet();
  private readonly rows = new Map<string, RowEls>();
  // Latest detection per id, read at tap time so a row that moved still
  // tunes the signal it shows.
  private readonly byId = new Map<string, Detection>();

  constructor(private readonly deps: SignalsDeps) {
    this.el = document.createElement("section");
    this.el.className = "panel signals";
    this.el.innerHTML = `
<div class="row">
  <label for="sg-conf">Confidence</label>
  <input type="range" id="sg-conf" min="${CONFIDENCE_MIN}" max="${CONFIDENCE_MAX}" step="0.01" value="${deps.initialConfidence}">
  <output id="sg-conf-out" for="sg-conf">${deps.initialConfidence.toFixed(2)}</output>
</div>
<p class="hint" id="sg-empty">No signals yet.</p>
<ul class="signal-list" id="sg-list"></ul>`;
    this.list = this.el.querySelector("#sg-list")!;
    this.empty = this.el.querySelector("#sg-empty")!;
    this.out = this.el.querySelector("#sg-conf-out")!;
    this.slider = this.el.querySelector<HTMLInputElement>("#sg-conf")!;
    const limiter = new RateLimitedSender(
      (v) => deps.setMinConfidence(v),
      () => performance.now(),
      (fn, ms) => {
        const h = window.setTimeout(fn, ms);
        return () => window.clearTimeout(h);
      },
    );
    this.slider.addEventListener("input", () => {
      const v = readConfidence(this.slider.value);
      this.out.textContent = v.toFixed(2);
      deps.saveConfidence(v);
      limiter.set(v);
    });
    // A press on the list freezes the order; the release comes after the
    // click has been delivered, so the moved row still receives it.
    this.list.addEventListener("pointerdown", () => this.set.hold(true));
    const release = () => {
      window.setTimeout(() => {
        const order = this.set.hold(false);
        if (order) this.place(order);
      }, 0);
    };
    document.addEventListener("pointerup", release);
    document.addEventListener("pointercancel", release);
  }

  value(): number {
    return readConfidence(this.slider.value);
  }

  update(d: Detection[]): void {
    this.latest = d;
    if (this.visible) this.render();
  }

  // Rows are only built while the tab can be seen; showing it catches up.
  setVisible(visible: boolean): void {
    this.visible = visible;
    if (visible) this.render();
  }

  render(): void {
    const clock = this.deps.clock();
    const textOf = (d: Detection) => {
      const name = labelText(d);
      const ago = heardAgo(d.lastSeen, clock ? clock.index : null, clock ? clock.rateHz : 0);
      const bw = d.bandwidthHz >= 1000 ? `${(d.bandwidthHz / 1000).toFixed(1)} kHz` : `${Math.round(d.bandwidthHz)} Hz`;
      const parts = [`${formatMHz(d.centerHz)} MHz`, name, bw, `${Math.round(d.snrDb)} dB`];
      if (d.state === "held") parts.push("held");
      if (ago !== "") parts.push(ago);
      return parts.join("  ");
    };
    const plan = this.set.apply(this.latest, textOf);
    this.byId.clear();
    for (const d of this.latest) this.byId.set(d.id, d);
    for (const id of plan.removed) {
      this.rows.get(id)?.li.remove();
      this.rows.delete(id);
    }
    for (const id of plan.added) {
      const li = document.createElement("li");
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "signal";
      btn.addEventListener("click", () => {
        const cur = this.byId.get(id);
        if (cur) pickSignal(cur, this.deps);
      });
      li.append(btn);
      this.rows.set(id, { li, btn });
    }
    for (const d of this.latest) {
      const r = this.rows.get(d.id);
      if (!r) continue;
      const t = textOf(d);
      if (r.btn.textContent !== t) r.btn.textContent = t;
      r.btn.classList.toggle("held", d.state === "held");
    }
    this.place(plan.order);
    this.empty.hidden = this.rows.size > 0;
  }

  // Touches the DOM only where the order differs from what is there.
  private place(order: string[]): void {
    const have = Array.from(this.list.children);
    let same = have.length === order.length;
    for (let i = 0; same && i < order.length; i++) same = have[i] === this.rows.get(order[i]!)?.li;
    if (same) return;
    for (const id of order) {
      const r = this.rows.get(id);
      if (r) this.list.append(r.li);
    }
  }
}


// The speech switch: whether the engine transcribes at all, and the state of
// the model it needs. The model is a download, so turning speech on for the
// first time is a visible process: absent, downloading with a percent,
// verifying, loading, ready, or failed with the engine's own sentence.
//
// The gateway pushes a `transcriptionStatus` event on connect, on resume and
// whenever the state changes (a download moving a whole percent counts), and
// the reply to setTranscription carries the same object.

import type { GatewayEvent, JsonValue } from "../protocol";
import type { GateLink } from "./receivers";

export type ModelState = "absent" | "downloading" | "verifying" | "loading" | "ready" | "failed";

const STATES: readonly string[] = ["absent", "downloading", "verifying", "loading", "ready", "failed"];

export interface SpeechStatus {
  enabled: boolean;
  modelState: ModelState;
  modelName: string;
  bytesDone: number;
  bytesTotal: number;
  detail: string;
}

export function parseSpeechStatus(v: JsonValue | GatewayEvent): SpeechStatus | null {
  if (v === null || typeof v !== "object" || Array.isArray(v)) return null;
  if (typeof v.enabled !== "boolean" || typeof v.modelState !== "string" || !STATES.includes(v.modelState)) return null;
  const num = (x: JsonValue | undefined) => (typeof x === "number" && Number.isFinite(x) ? x : 0);
  return {
    enabled: v.enabled,
    modelState: v.modelState as ModelState,
    modelName: typeof v.modelName === "string" ? v.modelName : "",
    bytesDone: num(v.bytesDone),
    bytesTotal: num(v.bytesTotal),
    detail: typeof v.detail === "string" ? v.detail : "",
  };
}

// Null when the size is not known yet.
export function downloadPercent(s: SpeechStatus): number | null {
  if (s.modelState !== "downloading" || !(s.bytesTotal > 0)) return null;
  return Math.max(0, Math.min(100, Math.floor((s.bytesDone * 100) / s.bytesTotal)));
}

// Short enough for the top bar.
export function speechLabel(s: SpeechStatus | null): string {
  if (s === null) return "Speech";
  if (!s.enabled) return "Speech off";
  switch (s.modelState) {
    case "absent":
      return "Speech: waiting for model";
    case "downloading": {
      const pct = downloadPercent(s);
      return pct === null ? "Speech: downloading" : `Speech: downloading ${pct}%`;
    }
    case "verifying":
      return "Speech: verifying";
    case "loading":
      return "Speech: loading";
    case "ready":
      return "Speech on";
    case "failed":
      return "Speech failed";
  }
}

// The full sentence, for the sheet.
export function speechLine(s: SpeechStatus | null): string {
  if (s === null) return "Speech: tap the switch to turn transcription on.";
  if (!s.enabled) return "Speech is off";
  const name = s.modelName === "" ? "model" : `model ${s.modelName}`;
  switch (s.modelState) {
    case "absent":
      return "Speech is on, waiting for the model";
    case "downloading": {
      const pct = downloadPercent(s);
      return pct === null ? `Downloading the speech ${name}` : `Downloading the speech ${name}, ${pct}%`;
    }
    case "verifying":
      return `Verifying the speech ${name}`;
    case "loading":
      return `Loading the speech ${name}`;
    case "ready":
      return "Speech is on";
    case "failed":
      return s.detail === "" ? "Speech failed" : `Speech failed: ${s.detail}`;
  }
}

export interface SpeechDeps {
  link: GateLink;
}

export class SpeechModel {
  status: SpeechStatus | null = null;
  // The engine's sentence for the last refusal; cleared when the next
  // request starts.
  error: string | null = null;
  private listeners = new Set<() => void>();
  private refusals = new Set<(text: string) => void>();

  // The gateway pushes the state on connect, on resume and whenever it
  // changes, so nothing here asks for it. The replies to the switch carry it
  // too and are applied the same way.
  constructor(private readonly deps: SpeechDeps) {
    deps.link.on("transcriptionStatus", (e) => this.apply(e));
  }

  onChange(fn: () => void): void {
    this.listeners.add(fn);
  }

  // Once per refused request, however alike the sentences are.
  onRefused(fn: (text: string) => void): void {
    this.refusals.add(fn);
  }

  // On when it is off, off when it is on, and on again when the model failed:
  // a tap on a failed switch is a retry, not a turn-off.
  toggle(): void {
    const s = this.status;
    this.ask(!(s?.enabled === true && s.modelState !== "failed"));
  }

  private ask(on: boolean): void {
    this.error = null;
    this.deps.link.request("setTranscription", { on }).then(
      (reply) => {
        this.apply(reply);
      },
      (e: unknown) => {
        // A dropped link is the status pill's news, not a refusal.
        if (!this.deps.link.ready) return;
        this.error = e instanceof Error ? e.message : String(e);
        this.changed();
        for (const fn of this.refusals) fn(this.error);
      },
    );
  }

  private apply(v: JsonValue | GatewayEvent): void {
    const s = parseSpeechStatus(v);
    if (!s) return;
    this.status = s;
    this.changed();
  }

  private changed(): void {
    for (const fn of this.listeners) fn();
  }
}

// The button for the top bar.
export class SpeechSwitch {
  readonly el: HTMLButtonElement;

  constructor(private readonly model: SpeechModel, toast: (text: string) => void) {
    this.el = document.createElement("button");
    this.el.type = "button";
    this.el.className = "chip speech";
    this.el.addEventListener("click", () => model.toggle());
    model.onChange(() => this.render());
    // Every refusal gets its sentence, including the same one twice.
    model.onRefused(toast);
    this.render();
  }

  private render(): void {
    const s = this.model.status;
    this.el.textContent = speechLabel(s);
    this.el.setAttribute("aria-pressed", String(s?.enabled === true));
    this.el.title = speechLine(s);
    this.el.classList.toggle("failed", s?.enabled === true && s.modelState === "failed");
  }
}

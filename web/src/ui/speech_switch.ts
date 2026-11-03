// The speech switch: whether the engine transcribes at all, and the state of
// the model it needs. The model is a download, so turning speech on for the
// first time is a visible process: absent, downloading with a percent,
// verifying, loading, ready, or failed with the engine's own sentence.
//
// The gateway has no read-only status op and never pushes
// `transcriptionStatus`; setTranscription is the only way to see the state.
// Asking again with the value already in force changes nothing, so the model
// polls with it while the model is on its way, and applies the event too
// should the gateway ever send one.

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

export const POLL_MS = 1_000;
// A model the engine never starts fetching is not worth asking about forever.
export const MAX_ABSENT_POLLS = 10;

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

function pending(s: SpeechStatus): boolean {
  return s.enabled && s.modelState !== "ready" && s.modelState !== "failed";
}

export interface SpeechDeps {
  link: GateLink;
  timers: { setTimeout(fn: () => void, ms: number): unknown; clearTimeout(h: unknown): void };
}

export class SpeechModel {
  status: SpeechStatus | null = null;
  // The engine's sentence when the last request was refused; cleared by the
  // next answer.
  error: string | null = null;
  private listeners = new Set<() => void>();
  private timer: unknown = null;
  private absentPolls = 0;

  constructor(private readonly deps: SpeechDeps) {
    deps.link.on("transcriptionStatus", (e) => this.apply(e));
    deps.link.on("ready", () => this.schedule());
  }

  onChange(fn: () => void): void {
    this.listeners.add(fn);
  }

  toggle(): void {
    this.absentPolls = 0;
    this.ask(!(this.status?.enabled ?? false));
  }

  // A transcript can only come from an engine that is transcribing, so a page
  // that opened onto a running session learns that without asking.
  heardTranscript(): void {
    if (this.status?.enabled && this.status.modelState === "ready") return;
    this.status = { enabled: true, modelState: "ready", modelName: this.status?.modelName ?? "", bytesDone: 0, bytesTotal: 0, detail: "" };
    this.changed();
  }

  private ask(on: boolean): void {
    this.cancel();
    this.deps.link.request("setTranscription", { on }).then(
      (reply) => {
        this.error = null;
        const s = parseSpeechStatus(reply);
        if (s) this.status = s;
        this.changed();
        this.schedule();
      },
      (e: unknown) => {
        this.error = e instanceof Error ? e.message : String(e);
        this.changed();
      },
    );
  }

  private apply(e: GatewayEvent): void {
    const s = parseSpeechStatus(e);
    if (!s) return;
    this.status = s;
    this.changed();
    this.schedule();
  }

  private schedule(): void {
    if (this.timer !== null || !this.status || !pending(this.status)) return;
    if (this.status.modelState === "absent" && ++this.absentPolls > MAX_ABSENT_POLLS) return;
    this.timer = this.deps.timers.setTimeout(() => {
      this.timer = null;
      // Down: the reconnect's ready event restarts the poll.
      if (!this.deps.link.ready || !this.status || !pending(this.status)) return;
      this.ask(true);
    }, POLL_MS);
  }

  private cancel(): void {
    if (this.timer !== null) this.deps.timers.clearTimeout(this.timer);
    this.timer = null;
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
    let shown: string | null = null;
    model.onChange(() => {
      this.render();
      // The refusal, or a failed model, once; the line in the sheet stays.
      if (model.error !== null && model.error !== shown) toast(model.error);
      shown = model.error;
    });
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

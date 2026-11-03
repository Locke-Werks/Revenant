// What the phone says about the link, the engine and the source: the pill
// over the waterfall when something stops it, and the smaller chips for
// conditions that leave it running.

import type { SourceEvent } from "../protocol";

// Below this many capture seconds per wall second the source is behind.
export const BEHIND_BELOW = 0.98;

// Zero is the engine's "not measured yet", and a pace below real time is a
// replay doing what it was asked; neither is the source falling behind.
export function sourceBehind(e: SourceEvent): boolean {
  if (!e.open) return false;
  const f = e.realtimeFactor;
  if (typeof f !== "number" || !(f > 0) || f >= BEHIND_BELOW) return false;
  const paced = e.pacedBy;
  if (typeof paced === "number" && paced > 0 && paced < BEHIND_BELOW) return false;
  return true;
}

export type EngineState = "starting" | "waiting" | "up" | "refused";

export class StatusModel {
  private socketUp = false;
  private engine: EngineState | null = null;
  private engineDetail = "";
  private everUp = false;
  private sourceOpen: boolean | null = null;
  private behind = false;
  private ended: string | null = null;

  socket(up: boolean): void {
    this.socketUp = up;
  }

  link(state: EngineState, detail = ""): void {
    this.engine = state;
    this.engineDetail = detail;
    if (state === "up") this.everUp = true;
  }

  source(e: SourceEvent): void {
    this.sourceOpen = e.open === true;
    this.behind = sourceBehind(e);
  }

  eventsEnded(reason: string): void {
    this.ended = reason;
  }

  // One line for whatever stops the waterfall, or null when nothing does.
  pill(): string | null {
    if (!this.socketUp) return this.everUp ? "reconnecting" : "connecting";
    switch (this.engine) {
      case "starting":
      case "waiting":
        return this.everUp ? "engine restarting" : "waiting for engine";
      case "refused":
        return this.engineDetail ? `engine refused: ${this.engineDetail}` : "engine refused";
      default:
        break;
    }
    if (this.sourceOpen === false) return "no radio open";
    return null;
  }

  // The waterfall shows old rows, dimmed, while there is no engine to send
  // new ones. Before the gateway has said anything about the engine the
  // socket being up is taken as enough.
  frozen(): boolean {
    if (!this.socketUp) return true;
    return this.engine !== null && this.engine !== "up";
  }

  chips(): string[] {
    const out: string[] = [];
    if (this.behind && this.sourceOpen) out.push("source behind real time");
    if (this.ended !== null) out.push(`removals elsewhere may go unreported (${this.ended})`);
    return out;
  }
}

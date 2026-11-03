// The feed in the sheet: what the active receiver has decoded and what was
// said on it, newest first. Everything in it came off the air or out of a
// recogniser, so it only ever reaches the page as text.

import type { GatewayEvent, JsonValue } from "../protocol";

export const MAX_FEED = 200;
// Engine and over-the-air text can be any length; one line of feed is not
// worth more than this.
export const MAX_FEED_CHARS = 500;

function clip(text: string): string {
  return text.length > MAX_FEED_CHARS ? `${text.slice(0, MAX_FEED_CHARS - 1)}…` : text;
}

export interface FeedEntry {
  id: number;
  kind: "decoded" | "transcript";
  vrx: number;
  label: string;
  text: string;
}

export type NewFeedEntry = Omit<FeedEntry, "id">;

export class FeedModel {
  private items: FeedEntry[] = [];
  private nextId = 1;
  private listeners = new Set<() => void>();

  onChange(fn: () => void): void {
    this.listeners.add(fn);
  }

  add(e: NewFeedEntry): void {
    this.items.push({ ...e, id: this.nextId++ });
    while (this.items.length > MAX_FEED) this.items.shift();
    this.changed();
  }

  // The receiver's entries, newest first. Nothing when no receiver is active.
  forReceiver(vrx: number | null): FeedEntry[] {
    if (vrx === null) return [];
    const out: FeedEntry[] = [];
    for (let i = this.items.length - 1; i >= 0; i--) {
      if (this.items[i]!.vrx === vrx) out.push(this.items[i]!);
    }
    return out;
  }

  dropReceiver(vrx: number): void {
    const before = this.items.length;
    this.items = this.items.filter((e) => e.vrx !== vrx);
    if (this.items.length !== before) this.changed();
  }

  remap(pairs: { old: number; new: number }[]): void {
    if (pairs.length === 0) return;
    const to = new Map(pairs.map((p) => [p.old, p.new]));
    this.items = this.items.map((e) => ({ ...e, vrx: to.get(e.vrx) ?? e.vrx }));
    this.changed();
  }

  clear(): void {
    this.items = [];
    this.changed();
  }

  private changed(): void {
    for (const fn of this.listeners) fn();
  }
}

function vrxOf(e: GatewayEvent): number | null {
  return typeof e.vrx === "number" && Number.isFinite(e.vrx) ? e.vrx : null;
}

export function transcriptEntry(e: GatewayEvent): NewFeedEntry | null {
  const vrx = vrxOf(e);
  const text = typeof e.text === "string" ? e.text.trim() : "";
  if (vrx === null || text === "") return null;
  return { kind: "transcript", vrx, label: "Speech", text: clip(text) };
}

function fieldValue(v: JsonValue): string {
  return typeof v === "object" && v !== null ? JSON.stringify(v) : String(v);
}

// A digital decoder's message: its own text when it has one, otherwise the
// fields it published (talkgroup, source and the like) as a line.
export function decodedEntry(e: GatewayEvent): NewFeedEntry | null {
  const vrx = vrxOf(e);
  if (vrx === null) return null;
  let text = typeof e.text === "string" ? e.text.trim() : "";
  if (text === "" && typeof e.fields === "object" && e.fields !== null && !Array.isArray(e.fields)) {
    text = Object.entries(e.fields)
      .map(([k, v]) => `${k} ${fieldValue(v)}`)
      .join(", ");
  }
  if (text === "") return null;
  const decoder = typeof e.decoder === "string" ? e.decoder : "";
  const kind = typeof e.kind === "string" ? e.kind : "";
  const label = [decoder, kind].filter((s) => s !== "").join(" ") || "Decoded";
  return { kind: "decoded", vrx, label, text: clip(text) };
}

// The few things the view needs from an element, so a test can stand in.
export interface HostEl {
  className: string;
  textContent: string | null;
  append(...nodes: never[]): void;
  replaceChildren(...nodes: never[]): void;
}

export class FeedView<E extends HostEl> {
  constructor(
    private readonly host: E,
    private readonly model: FeedModel,
    private readonly create: () => E,
  ) {}

  render(active: number | null): void {
    const entries = this.model.forReceiver(active);
    if (entries.length === 0) {
      const empty = this.create();
      empty.className = "feed-empty";
      empty.textContent = active === null ? "No receiver selected." : "Nothing decoded yet.";
      this.host.replaceChildren(empty as never);
      return;
    }
    const rows = entries.map((e) => {
      const row = this.create();
      row.className = `feed-row ${e.kind}`;
      const label = this.create();
      label.className = "feed-label";
      label.textContent = e.label;
      const text = this.create();
      text.className = "feed-text";
      text.textContent = e.text;
      row.append(label as never, text as never);
      return row as never;
    });
    this.host.replaceChildren(...rows);
  }
}

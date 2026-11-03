// The phone's model of the gateway's "detections" event.
import type { GatewayEvent } from "./protocol";

export interface Detection {
  // Decimal string: the engine's id is a UInt64 and a JSON number loses it
  // past 2^53. Never convert it to a number.
  id: string;
  centerHz: number;
  bandwidthHz: number;
  snrDb: number;
  confidence: number;
  state: "live" | "held";
  label: string;
  // "modulation", "protocol", or "" for none.
  labelKind: string;
  labelConfidence: number;
  lastSeen: number;
}

const finite = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);

function parseOne(v: unknown): Detection | null {
  if (typeof v !== "object" || v === null) return null;
  const o = v as Record<string, unknown>;
  if (typeof o.id !== "string" || o.id === "") return null;
  if (o.state !== "live" && o.state !== "held") return null;
  if (
    !finite(o.centerHz) ||
    !finite(o.bandwidthHz) ||
    !finite(o.snrDb) ||
    !finite(o.confidence) ||
    !finite(o.labelConfidence) ||
    !finite(o.lastSeen)
  ) {
    return null;
  }
  return {
    id: o.id,
    centerHz: o.centerHz,
    bandwidthHz: o.bandwidthHz,
    snrDb: o.snrDb,
    confidence: o.confidence,
    state: o.state,
    label: typeof o.label === "string" ? o.label : "",
    labelKind: typeof o.labelKind === "string" ? o.labelKind : "",
    labelConfidence: o.labelConfidence,
    lastSeen: o.lastSeen,
  };
}

// A bad item is dropped on its own so one stray entry does not blank the list.
export function parseDetections(e: GatewayEvent): Detection[] {
  const items = (e as { items?: unknown }).items;
  if (!Array.isArray(items)) return [];
  const out: Detection[] = [];
  for (const it of items) {
    const d = parseOne(it);
    if (d) out.push(d);
  }
  return out;
}

// Ids compare as BigInt because they can exceed 2^53 and a string compare
// would put "10" before "9". Digits-only is guaranteed by the gateway; a
// non-numeric id falls back to a string compare rather than throwing.
export function compareId(a: string, b: string): number {
  try {
    const x = BigInt(a);
    const y = BigInt(b);
    return x < y ? -1 : x > y ? 1 : 0;
  } catch {
    return a < b ? -1 : a > b ? 1 : 0;
  }
}

// Lowest frequency first: a row keeps its place as signals come and go,
// where an SNR order shuffles the whole list every time a level wobbles.
export function sortForList<T extends Detection>(d: readonly T[]): T[] {
  return [...d].sort((a, b) => a.centerHz - b.centerHz || compareId(a.id, b.id));
}

// How long a signal the gateway has stopped reporting stays in the phone's
// list and on its spectrum, so the user can park a receiver on it while
// waiting for it to come back.
export const LOST_LINGER_MS = 120_000;
// Where the fade ends up just before the entry is removed.
export const LOST_MIN_OPACITY = 0.35;

// A detection as the phone shows it. lostForMs is the time since the gateway
// last listed it, absent while it is still listed.
export interface Listed extends Detection {
  lostForMs?: number;
}

// 1 while listed, falling linearly to LOST_MIN_OPACITY at the end of the
// linger.
export function lostOpacity(lostForMs: number | undefined): number {
  if (lostForMs === undefined) return 1;
  const k = Math.min(1, Math.max(0, lostForMs / LOST_LINGER_MS));
  return 1 - (1 - LOST_MIN_OPACITY) * k;
}

// "0:42", for the lost rows' age.
export function lostClock(lostForMs: number): string {
  const s = Math.max(0, Math.floor(lostForMs / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

// Remembers detections that dropped out of the gateway's list. A lost entry
// is revived by the same id coming back, or by a new id appearing with its
// centre inside the lost band: the tracker re-issues ids when a signal
// returns after its track expired.
export class LingerTracker {
  private live = new Map<string, Detection>();
  private lost = new Map<string, { det: Detection; heardMs: number }>();
  private lastUpdateMs = 0;

  update(items: readonly Detection[], nowMs: number): void {
    const next = new Map<string, Detection>();
    for (const d of items) next.set(d.id, d);
    // The last update that listed it is when it was last heard.
    for (const [id, det] of this.live) {
      if (!next.has(id)) this.lost.set(id, { det, heardMs: this.lastUpdateMs });
    }
    for (const id of next.keys()) this.lost.delete(id);
    const fresh = items.filter((d) => !this.live.has(d.id));
    for (const [id, l] of this.lost) {
      const half = Math.max(0, l.det.bandwidthHz) / 2;
      if (fresh.some((d) => Math.abs(d.centerHz - l.det.centerHz) <= half)) this.lost.delete(id);
    }
    this.live = next;
    this.lastUpdateMs = nowMs;
  }

  // Listed detections followed by the lost ones still within the linger.
  view(nowMs: number): Listed[] {
    const out: Listed[] = [...this.live.values()];
    for (const [id, l] of this.lost) {
      const gone = nowMs - l.heardMs;
      if (gone >= LOST_LINGER_MS) {
        this.lost.delete(id);
        continue;
      }
      out.push({ ...l.det, lostForMs: Math.max(0, gone) });
    }
    return out;
  }

  hasLost(): boolean {
    return this.lost.size > 0;
  }
}

// Narrowest wins where bands overlap: the tap is more likely aimed at the
// small signal sitting inside a wide one than the reverse.
export function snapTap(hz: number, d: Detection[]): number {
  let best: Detection | null = null;
  for (const x of d) {
    if (Math.abs(hz - x.centerHz) > x.bandwidthHz / 2) continue;
    if (!best || x.bandwidthHz < best.bandwidthHz) best = x;
  }
  return best ? best.centerHz : hz;
}

// The labelled detection a receiver at hz sits on, narrowest first as in
// snapTap; null when none covers it or the one that does has no label.
export function labelAt(hz: number, d: readonly Detection[]): Detection | null {
  let best: Detection | null = null;
  for (const x of d) {
    if (x.label === "" || Math.abs(hz - x.centerHz) > x.bandwidthHz / 2) continue;
    if (!best || x.bandwidthHz < best.bandwidthHz) best = x;
  }
  return best;
}

// A label with its confidence, "?" when the classifier has not settled.
export function labelText(d: Pick<Detection, "label" | "labelConfidence">): string {
  if (d.label === "") return "?";
  const pct = Math.round(Math.min(1, Math.max(0, d.labelConfidence)) * 100);
  return pct > 0 ? `${d.label.toUpperCase()} ${pct}%` : d.label.toUpperCase();
}

export function fitBandwidth(widthHz: number, choices: number[]): number {
  let fit = Infinity;
  let largest = -Infinity;
  for (const c of choices) {
    if (c >= widthHz && c < fit) fit = c;
    if (c > largest) largest = c;
  }
  return Number.isFinite(fit) ? fit : largest;
}

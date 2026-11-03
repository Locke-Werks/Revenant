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
function compareId(a: string, b: string): number {
  try {
    const x = BigInt(a);
    const y = BigInt(b);
    return x < y ? -1 : x > y ? 1 : 0;
  } catch {
    return a < b ? -1 : a > b ? 1 : 0;
  }
}

export function sortForList(d: Detection[]): Detection[] {
  return [...d].sort((a, b) => b.snrDb - a.snrDb || compareId(a.id, b.id));
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

export function fitBandwidth(widthHz: number, choices: number[]): number {
  let fit = Infinity;
  let largest = -Infinity;
  for (const c of choices) {
    if (c >= widthHz && c < fit) fit = c;
    if (c > largest) largest = c;
  }
  return Number.isFinite(fit) ? fit : largest;
}

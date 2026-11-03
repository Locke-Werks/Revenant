// The DV tab's data, free of the DOM: the gateway's calls, talkgroups, trunk
// and dv events (tools/webgate/dv_feed.h has the shapes), when the tab is
// shown, and what a row says in each column. The columns are the desktop's,
// ui/qml/CallsPane.qml.

import type { GatewayEvent, JsonValue } from "./protocol";

// Modes that carry digital voice. A trunk follower is a p25p1 receiver like
// any other, so it counts by its mode.
export const DV_RECEIVER_MODES: readonly string[] = ["p25p1", "dmr", "dstar", "tetra"];

export function hasDvReceiver(receivers: readonly { mode: string }[]): boolean {
  return receivers.some((r) => DV_RECEIVER_MODES.includes(r.mode));
}

// The tab to show once DV availability is known: the one open, unless that is
// the DV tab and it has nothing to show, which falls back to Tune.
export function tabAfterDvChange<T extends string>(open: T | "dv", dvShown: boolean): T | "dv" | "tune" {
  return open === "dv" && !dvShown ? "tune" : open;
}

export type CallState = "live" | "ended" | "granted";

export interface CallRow {
  id: number;
  protocol: string;
  system: string;
  target: string;
  alias: string | null;
  source: string;
  group: boolean;
  encrypted: boolean;
  emergency: boolean;
  frequencyHz: number;
  vrx: number;
  slot: number;
  owner: string;
  firstMs: number;
  lastMs: number;
  state: CallState;
  count: number;
}

const str = (v: JsonValue | undefined): string => (typeof v === "string" ? v : "");
const num = (v: JsonValue | undefined): number => (typeof v === "number" && Number.isFinite(v) ? v : 0);
const bool = (v: JsonValue | undefined): boolean => v === true;

function rowOf(v: JsonValue): CallRow | null {
  if (v === null || typeof v !== "object" || Array.isArray(v)) return null;
  const state = v.state === "live" || v.state === "granted" ? v.state : "ended";
  return {
    id: num(v.id),
    protocol: str(v.protocol),
    system: str(v.system),
    target: str(v.target),
    alias: typeof v.alias === "string" && v.alias !== "" ? v.alias : null,
    source: str(v.source),
    group: v.group !== false,
    encrypted: bool(v.encrypted),
    emergency: bool(v.emergency),
    frequencyHz: num(v.frequencyHz),
    vrx: num(v.vrx),
    slot: num(v.slot),
    owner: str(v.owner),
    firstMs: num(v.firstMs),
    lastMs: num(v.lastMs),
    state,
    count: num(v.count),
  };
}

export function parseRows(e: GatewayEvent): CallRow[] {
  const items = Array.isArray(e.items) ? e.items : [];
  const out: CallRow[] = [];
  for (const item of items) {
    const r = rowOf(item);
    if (r) out.push(r);
  }
  return out;
}

// Newest first, by when the row was first heard and then by id, so two rows
// opened in one millisecond keep the gateway's order.
export function newestFirst(rows: readonly CallRow[]): CallRow[] {
  return [...rows].sort((a, b) => b.firstMs - a.firstMs || b.id - a.id);
}

function clock(ms: number): string {
  if (!(ms > 0)) return "";
  const d = new Date(ms);
  const p = (n: number) => String(n).padStart(2, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

// "TG" only where the target is a talkgroup, as CallsPane.qml writes it.
export function talkgroupText(r: Pick<CallRow, "target" | "group" | "protocol" | "alias">): string {
  if (r.alias) return r.alias;
  if (r.target === "") return "";
  return r.group && (r.protocol === "P25" || r.protocol === "DMR") ? `TG ${r.target}` : r.target;
}

export const STATE_TEXT: Record<CallState, string> = { live: "live", ended: "ended", granted: "not followed" };

export interface CallCells {
  time: string;
  talkgroup: string;
  radio: string;
  receiver: string;
  state: string;
  encrypted: boolean;
  emergency: boolean;
}

// One calls-table row. receiver names the receiver as the chips do: its id,
// with the owner when it is a plugin's, such as a trunk follower.
export function callCells(r: CallRow): CallCells {
  const owner = r.owner !== "" && r.owner !== "user" ? ` ${r.owner}` : "";
  return {
    time: clock(r.lastMs),
    talkgroup: talkgroupText(r),
    radio: r.source,
    receiver: r.state === "granted" ? "" : `#${r.vrx}${owner}`,
    state: STATE_TEXT[r.state],
    encrypted: r.encrypted,
    emergency: r.emergency,
  };
}

export interface GroupCells {
  time: string;
  talkgroup: string;
  radio: string;
  calls: string;
  state: string;
  encrypted: boolean;
}

export function groupCells(r: CallRow): GroupCells {
  // A station row is filed by its source; the talkgroup column then says who.
  const who = r.group ? talkgroupText(r) : r.source;
  return {
    time: clock(r.lastMs),
    talkgroup: who,
    radio: r.group ? r.source : r.target,
    calls: `${r.count} ${r.count === 1 ? "call" : "calls"}`,
    state: STATE_TEXT[r.state],
    encrypted: r.encrypted,
  };
}

// A tap on a call row selects the receiver that carried it, when it is still
// there. A grant nobody followed has no receiver to select.
export function pickCall(r: CallRow, receivers: readonly { vrx: number }[], select: (vrx: number) => void): boolean {
  if (r.state === "granted" || !receivers.some((x) => x.vrx === r.vrx)) return false;
  select(r.vrx);
  return true;
}

export interface DvState {
  autoDv: boolean;
  follow: { vrx: number; maxFollowers: number } | null;
  maxFollowers: number;
}

export const NO_DV: DvState = { autoDv: false, follow: null, maxFollowers: 2 };

export function parseDv(e: GatewayEvent): DvState {
  const f = e.follow;
  const follow =
    f !== null && typeof f === "object" && !Array.isArray(f) ? { vrx: num(f.vrx), maxFollowers: num(f.maxFollowers) } : null;
  return { autoDv: bool(e.autoDv), follow, maxFollowers: num(e.maxFollowers) || 2 };
}

export interface TrunkSummary {
  enabled: boolean;
  controlVrx: number;
  text: string;
}

export function parseTrunk(e: GatewayEvent): TrunkSummary {
  const fault = str(e.fault);
  const enabled = bool(e.enabled);
  const calls = Array.isArray(e.calls) ? e.calls : [];
  const active = calls.filter((c) => c !== null && typeof c === "object" && !Array.isArray(c) && c.active === true).length;
  let text: string;
  if (fault !== "") text = `This engine does not follow trunks: ${fault}`;
  else if (!enabled) text = str(e.note) === "" ? "Not following" : `Not following: ${str(e.note)}`;
  else text = `Control channel #${num(e.controlVrx)}, ${num(e.tsbks)} blocks, ${num(e.grants)} grants, ${active} on the air`;
  return { enabled, controlVrx: num(e.controlVrx), text };
}

// ---- What the phone remembers ----------------------------------------------
// The gateway's session dies with the resident linger, and receiver ids change
// across sessions, so the phone keeps its own switches and sends them on every
// link ready, as it does the confidence bar. Follow is kept by its control
// channel's frequency and found again on whatever receiver sits there.

export const DV_KEY = "revenant.dv";

// Half a P25 channel: a receiver this close is the same control channel.
export const FOLLOW_MATCH_HZ = 6_250;

export interface TrunkRuleTexts {
  allow: string;
  deny: string;
  priority: string;
}

export interface DvMemory {
  autoDv: boolean;
  follow: { hz: number; maxFollowers: number } | null;
  maxFollowers: number | null;
  rules: TrunkRuleTexts | null;
}

export const NO_MEMORY: DvMemory = { autoDv: false, follow: null, maxFollowers: null, rules: null };

export function readDvMemory(text: string | null): DvMemory {
  if (text === null) return NO_MEMORY;
  let v: unknown;
  try {
    v = JSON.parse(text);
  } catch {
    return NO_MEMORY;
  }
  if (v === null || typeof v !== "object") return NO_MEMORY;
  const o = v as Record<string, unknown>;
  const f = o.follow as Record<string, unknown> | null | undefined;
  const r = o.rules as Record<string, unknown> | null | undefined;
  const s = (x: unknown) => (typeof x === "string" ? x : "");
  return {
    autoDv: o.autoDv === true,
    follow:
      f && typeof f.hz === "number" && Number.isFinite(f.hz)
        ? { hz: f.hz, maxFollowers: typeof f.maxFollowers === "number" ? f.maxFollowers : 2 }
        : null,
    maxFollowers: typeof o.maxFollowers === "number" ? o.maxFollowers : null,
    rules: r && typeof r === "object" ? { allow: s(r.allow), deny: s(r.deny), priority: s(r.priority) } : null,
  };
}

export function controlAt(hz: number, receivers: readonly { vrx: number; hz: number; mode: string }[]): number | null {
  let best: number | null = null;
  let gap = Infinity;
  for (const r of receivers) {
    const d = Math.abs(r.hz - hz);
    if (r.mode === "p25p1" && d <= FOLLOW_MATCH_HZ && d < gap) {
      best = r.vrx;
      gap = d;
    }
  }
  return best;
}

export interface DvSend {
  autoDv(on: boolean): void;
  follow(vrx: number, on: boolean, maxFollowers: number): void;
  // null goes back to the desktop's saved rules.
  rules(r: TrunkRuleTexts | null): void;
}

// The phone's switches, saved on every change and replayed on every ready.
// Follow waits for its control channel's receiver to be listed, since a fresh
// session hears of receivers after ready.
export class DvReplay {
  private memory: DvMemory;
  private followPending = false;

  constructor(
    private readonly send: DvSend,
    private readonly load: () => string | null,
    private readonly save: (text: string) => void,
  ) {
    this.memory = readDvMemory(load());
  }

  get state(): DvMemory {
    return this.memory;
  }

  ready(receivers: readonly { vrx: number; hz: number; mode: string }[]): void {
    this.memory = readDvMemory(this.load());
    if (this.memory.autoDv) this.send.autoDv(true);
    if (this.memory.rules) this.send.rules(this.memory.rules);
    this.followPending = this.memory.follow !== null;
    this.receivers(receivers);
  }

  // The gateway's dv event: following already on means nothing to replay.
  gateway(dv: DvState): void {
    if (dv.follow) this.followPending = false;
  }

  receivers(list: readonly { vrx: number; hz: number; mode: string }[]): void {
    const f = this.memory.follow;
    if (!this.followPending || !f) return;
    const vrx = controlAt(f.hz, list);
    if (vrx === null) return;
    this.followPending = false;
    this.send.follow(vrx, true, f.maxFollowers);
  }

  setAutoDv(on: boolean): void {
    this.store({ ...this.memory, autoDv: on });
    this.send.autoDv(on);
  }

  setFollow(vrx: number, hz: number, on: boolean, maxFollowers: number): void {
    this.followPending = false;
    this.store({ ...this.memory, follow: on ? { hz, maxFollowers } : null, maxFollowers });
    this.send.follow(vrx, on, maxFollowers);
  }

  setRules(r: TrunkRuleTexts | null): void {
    this.store({ ...this.memory, rules: r });
    this.send.rules(r);
  }

  private store(m: DvMemory): void {
    this.memory = m;
    try {
      this.save(JSON.stringify(m));
    } catch {
      // A full disk costs the preference, not the session.
    }
  }
}

// The Follow switch belongs to a P25 receiver the phone is on, or to the
// control channel already being followed.
export function followTarget(active: { vrx: number; mode: string } | null, dv: DvState): number | null {
  if (dv.follow) return dv.follow.vrx;
  return active && active.mode === "p25p1" ? active.vrx : null;
}

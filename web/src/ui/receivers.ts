// The phone's receivers: frequency entry, the chip list, and the controller
// that turns taps and sheet controls into gateway ops and keeps the list,
// live audio and keep listening on the same ids as they change.
//
// Ids change under the phone in two ways. setMode and setBandwidth make a new
// receiver and remove the old one (the engine will not change either in
// place), so the reply carries the new vrx and `replaced`, and a vrxRemoved
// for the old id arrives in either order with it. An engine restart rebuilds
// every receiver under new ids, sent as a map on the link event. ReplaceGate
// holds the old id's removal until the reply has moved everything across, so
// live audio never sees its receiver vanish on a mode change.

import type { GatewayEvent, JsonValue, LinkEvent, Op } from "../protocol";
import type { LinkEvents, RequestOptions } from "../link";

export const MODES = ["nfm", "am", "wfm", "usb", "lsb", "cw", "dsb", "sam", "raw"] as const;
export const DEFAULT_MODE = "nfm";

// Widths offered in the sheet; the engine clamps anything its channel cannot
// carry and says so.
export const BANDWIDTHS_HZ = [2_400, 3_000, 6_000, 10_000, 12_500, 25_000, 50_000, 200_000];

// Below this a bare number is MHz; from it up to UNITLESS_HZ_FROM it could
// be kHz or MHz, so it needs a unit.
export const UNITLESS_MHZ_BELOW = 1_000;
export const UNITLESS_HZ_FROM = 100_000;

export type FrequencyParse = { hz: number } | { error: string };

// "146.52", "146,52", "146.52M", "146520k" and "146520000" are the same
// frequency. A bare number below 1000 is MHz and from 100000 up is Hz; in
// between (14070: kHz? MHz?) a guess would tune somewhere surprising, so it
// is refused with a sentence asking for a unit.
export function readFrequency(text: string): FrequencyParse {
  const t = text.replace(",", ".");
  const m = /^\s*(\d+(?:\.\d*)?|\.\d+)\s*([kKmMgG]?)([hH][zZ])?\s*$/.exec(t);
  const bad = { error: `"${text.trim()}" is not a frequency` };
  if (!m) return bad;
  const value = Number(m[1]);
  if (!Number.isFinite(value) || value <= 0) return bad;
  const unit = (m[2] ?? "").toLowerCase();
  let scale: number;
  if (unit === "k") scale = 1e3;
  else if (unit === "m") scale = 1e6;
  else if (unit === "g") scale = 1e9;
  else if (m[3]) scale = 1;
  else if (value < UNITLESS_MHZ_BELOW) scale = 1e6;
  else if (value >= UNITLESS_HZ_FROM) scale = 1;
  else return { error: `"${text.trim()}": add a unit (k, M, G)` };
  const hz = Math.round(value * scale);
  return hz > 0 ? { hz } : bad;
}

export function parseFrequency(text: string): number | null {
  const r = readFrequency(text);
  return "hz" in r ? r.hz : null;
}

export function formatMHz(hz: number): string {
  return (hz / 1e6).toFixed(4);
}

export interface Receiver {
  vrx: number;
  hz: number;
  mode: string;
  bandwidthHz: number | null;
  squelchDbfs: number | null;
  // The gateway could not read the receiver back and answered with what was
  // sent; shown as such, not as an error.
  statusUnavailable: boolean;
  audioError: string | null;
  // Whether speech on this receiver is left to the engine ("auto"), forced on
  // or off, and whether it is being transcribed right now.
  transcribe: TranscribeChoice;
  transcribing: boolean;
}

export const TRANSCRIBE_CHOICES = ["auto", "on", "off"] as const;
export type TranscribeChoice = (typeof TRANSCRIBE_CHOICES)[number];

function choiceOf(v: JsonValue | undefined): TranscribeChoice | undefined {
  return TRANSCRIBE_CHOICES.find((c) => c === v);
}

export class ReceiverList {
  private items = new Map<number, Receiver>();
  private activeId: number | null = null;
  private listeners = new Set<() => void>();

  get active(): number | null {
    return this.activeId;
  }

  get(vrx: number): Receiver | undefined {
    return this.items.get(vrx);
  }

  has(vrx: number): boolean {
    return this.items.has(vrx);
  }

  list(): Receiver[] {
    return [...this.items.values()].sort((a, b) => a.vrx - b.vrx);
  }

  onChange(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => void this.listeners.delete(fn);
  }

  select(vrx: number | null): void {
    if (vrx !== null && !this.items.has(vrx)) return;
    this.activeId = vrx;
    this.changed();
  }

  // A reply from addVrx, tune, setSquelch, setMode or setBandwidth. Fields the
  // reply leaves out keep what was there.
  upsert(r: { vrx: number } & Partial<Omit<Receiver, "vrx">>): Receiver {
    const was = this.items.get(r.vrx);
    const next: Receiver = {
      vrx: r.vrx,
      hz: r.hz ?? was?.hz ?? 0,
      mode: r.mode ?? was?.mode ?? DEFAULT_MODE,
      bandwidthHz: r.bandwidthHz ?? was?.bandwidthHz ?? null,
      squelchDbfs: r.squelchDbfs ?? was?.squelchDbfs ?? null,
      statusUnavailable: r.statusUnavailable ?? false,
      audioError: r.audioError ?? was?.audioError ?? null,
      transcribe: r.transcribe ?? was?.transcribe ?? "auto",
      transcribing: r.transcribing ?? was?.transcribing ?? false,
    };
    this.items.set(r.vrx, next);
    this.changed();
    return next;
  }

  remove(vrx: number): Receiver | undefined {
    const was = this.items.get(vrx);
    if (!was) return undefined;
    this.items.delete(vrx);
    if (this.activeId === vrx) this.activeId = null;
    this.changed();
    return was;
  }

  // Old ids to new, applied as one step: a fresh engine numbers from 1, so
  // a map can swap two ids.
  remap(pairs: { old: number; new: number }[]): void {
    if (pairs.length === 0) return;
    const to = new Map(pairs.map((p) => [p.old, p.new]));
    const next = new Map<number, Receiver>();
    for (const r of this.items.values()) {
      const id = to.get(r.vrx) ?? r.vrx;
      next.set(id, { ...r, vrx: id });
    }
    this.items = next;
    if (this.activeId !== null) this.activeId = to.get(this.activeId) ?? this.activeId;
    this.changed();
  }

  private changed(): void {
    for (const fn of this.listeners) fn();
  }
}

// ---- The gate ----------------------------------------------------------------

// The parts of Link the controller and the audio paths use.
export interface GateLink {
  readonly ready: boolean;
  request(op: Op, args?: { [key: string]: JsonValue }, opts?: RequestOptions): Promise<JsonValue>;
  on<K extends keyof LinkEvents>(name: K, fn: (payload: LinkEvents[K]) => void): () => void;
}

// Stands between Link and everything that reacts to vrxRemoved. A removal of
// an id with a replace in flight waits until the replace settles; the lost
// ids of an engine restart, whose removals name OLD ids a recreated receiver
// may now be using, are dropped.
export class ReplaceGate implements GateLink {
  private removedFns = new Set<(e: GatewayEvent) => void>();
  private holding = new Map<number, GatewayEvent[]>();
  private staleLost = new Set<number>();

  constructor(private readonly link: GateLink) {
    link.on("vrxRemoved", (e) => this.removed(e));
    link.on("link", (e) => {
      this.staleLost.clear();
      for (const l of e.lost ?? []) this.staleLost.add(l.vrx);
    });
  }

  get ready(): boolean {
    return this.link.ready;
  }

  request(op: Op, args?: { [key: string]: JsonValue }, opts?: RequestOptions): Promise<JsonValue> {
    return this.link.request(op, args, opts);
  }

  on<K extends keyof LinkEvents>(name: K, fn: (payload: LinkEvents[K]) => void): () => void {
    if (name !== "vrxRemoved") return this.link.on(name, fn);
    const f = fn as (e: GatewayEvent) => void;
    this.removedFns.add(f);
    return () => void this.removedFns.delete(f);
  }

  hold(vrx: number): void {
    if (!this.holding.has(vrx)) this.holding.set(vrx, []);
  }

  // Delivers whatever was held for vrx. Call after the replace has moved
  // everything to the new id, so listeners find nothing under the old one.
  release(vrx: number): void {
    const held = this.holding.get(vrx);
    this.holding.delete(vrx);
    for (const e of held ?? []) this.deliver(e);
  }

  private removed(e: GatewayEvent): void {
    if (typeof e.vrx === "number") {
      if (this.staleLost.delete(e.vrx)) return;
      const held = this.holding.get(e.vrx);
      if (held) {
        held.push(e);
        return;
      }
    }
    this.deliver(e);
  }

  private deliver(e: GatewayEvent): void {
    for (const fn of [...this.removedFns]) fn(e);
  }
}

// ---- The controller ----------------------------------------------------------

export interface ControllerAudio {
  readonly vrx: number | null;
  unlock(): Promise<void>;
  start(vrx: number): Promise<void>;
  stop(): void;
  resume(): Promise<boolean>;
  retarget(oldVrx: number, newVrx: number): void;
}

export interface ControllerKeep {
  retarget(oldVrx: number, newVrx: number): void;
  setNowPlaying(now: { hz: number; mode: string }): void;
}

export interface ControllerDeps {
  link: ReplaceGate;
  audio: ControllerAudio;
  keep: ControllerKeep;
  // A sentence that comes and goes.
  toast(text: string): void;
  // A sentence about live audio that stays until it no longer holds; null
  // clears it.
  audioNote(text: string | null): void;
}

const NOT_STARTED = "The phone has not started sound yet. Tap the waterfall or a receiver to allow it.";

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

function num(v: JsonValue | undefined): number | undefined {
  return typeof v === "number" && Number.isFinite(v) ? v : undefined;
}

export class ReceiverController {
  readonly list = new ReceiverList();
  // Removals this phone asked for, so their events are not reported as news.
  private removing = new Set<number>();
  // Receivers with a setMode or setBandwidth in flight. A tap tuning one
  // would land on an id about to be removed.
  private replacing = new Set<number>();
  private tuneSeq = 0;
  private dragging = false;
  private heldReply: { seq: number; reply: JsonValue } | null = null;
  private goneFns = new Set<(vrx: number) => void>();
  private remapFns = new Set<(pairs: { old: number; new: number }[]) => void>();

  // Everything else that keys on a receiver id (captions, the feed) follows
  // the same moves the list makes, from the one place that knows them: a
  // replace's single pair and an engine restart's map.
  onRemap(fn: (pairs: { old: number; new: number }[]) => void): void {
    this.remapFns.add(fn);
  }

  // Called when a receiver is gone for good (removed here, removed by the
  // engine, or lost in a restart), so what was kept for it can go too.
  onGone(fn: (vrx: number) => void): void {
    this.goneFns.add(fn);
  }

  private gone(vrx: number): void {
    for (const fn of this.goneFns) fn(vrx);
  }

  private remapped(pairs: { old: number; new: number }[]): void {
    if (pairs.length === 0) return;
    for (const fn of this.remapFns) fn(pairs);
  }

  constructor(private readonly deps: ControllerDeps) {
    const link = deps.link;
    link.on("vrxStatus", (e) => this.onStatus(e));
    link.on("vrxRemoved", (e) => this.onRemoved(e));
    link.on("link", (e) => this.onLink(e));
  }

  // Tap on the waterfall: tune the active receiver, or add one there. Runs
  // inside the gesture, which is where iOS lets audio start.
  tap(hz: number): void {
    void this.deps.audio.unlock();
    const active = this.list.active;
    if (active === null) {
      this.add(hz);
      return;
    }
    if (this.replacing.has(active)) return;
    // Supersedes any drag reply still in flight.
    this.tuneSeq++;
    this.call("tune", { vrx: active, hz: Math.round(hz) }).then((r) => {
      if (r) this.nowPlaying(r);
    });
  }

  // A drag shows the active receiver at hz at once; the engine's answer comes
  // later and replaces it. Returns the receiver, or null when there is none to
  // drag or it is mid-replace.
  preview(hz: number): Receiver | null {
    const vrx = this.list.active;
    if (vrx === null || this.replacing.has(vrx)) return null;
    this.dragging = true;
    return this.list.upsert({ vrx, hz: Math.round(hz) });
  }

  // Call after the final flush has been sent. Replies were being held back;
  // the one answering the newest request (if it already came) lands now, and
  // one still in flight lands on arrival.
  endDrag(): void {
    this.dragging = false;
    const held = this.heldReply;
    this.heldReply = null;
    if (held && held.seq === this.tuneSeq) this.applyTuneReply(held.reply);
  }

  private applyTuneReply(reply: JsonValue): void {
    const r = this.fromReply(reply);
    if (r) this.nowPlaying(r);
  }

  // One tune of a drag. Replies arrive out of step with the finger, so only
  // the newest request's reply is applied; an older one would pull the marker
  // back behind the finger.
  // vrx names the receiver being dragged, which a final flush must still
  // reach after the active one has changed.
  tuneDragged(hz: number, target: number | null = this.list.active): void {
    const vrx = target;
    if (vrx === null || !this.list.has(vrx) || this.replacing.has(vrx)) return;
    const seq = ++this.tuneSeq;
    this.deps.link.request("tune", { vrx, hz: Math.round(hz) }).then(
      (reply) => {
        if (seq !== this.tuneSeq) return;
        // The limiter may hold a newer value than this reply answers, so
        // applying it would snap the preview back behind the finger.
        if (this.dragging) {
          this.heldReply = { seq, reply };
          return;
        }
        this.applyTuneReply(reply);
      },
      (e: unknown) => {
        if (seq === this.tuneSeq) this.deps.toast(errorText(e));
      },
    );
  }

  // Long press: another receiver there, which becomes the one heard.
  longPress(hz: number): void {
    void this.deps.audio.unlock();
    this.add(hz);
  }

  // A chip tap. Also in a gesture.
  select(vrx: number): void {
    if (!this.list.has(vrx)) return;
    const was = this.deps.audio.vrx;
    this.list.select(vrx);
    if (was !== null && was !== vrx) this.deps.keep.retarget(was, vrx);
    const r = this.list.get(vrx)!;
    this.deps.keep.setNowPlaying({ hz: r.hz, mode: r.mode });
    this.listen(vrx);
  }

  setMode(mode: string): void {
    const vrx = this.list.active;
    if (vrx === null) return;
    this.replace(vrx, "setMode", { vrx, mode });
  }

  setBandwidth(hz: number): void {
    const vrx = this.list.active;
    if (vrx === null) return;
    this.replace(vrx, "setBandwidth", { vrx, hz: Math.round(hz) });
  }

  setSquelch(dbfs: number): void {
    const vrx = this.list.active;
    if (vrx === null) return;
    void this.call("setSquelch", { vrx, dbfs });
  }

  setTranscribe(choice: TranscribeChoice): void {
    const vrx = this.list.active;
    if (vrx === null) return;
    void this.call("setVrxTranscribe", { vrx, choice });
  }

  remove(vrx: number): void {
    this.removing.add(vrx);
    this.deps.link.request("removeVrx", { vrx }).then(
      () => {
        // The reply settles it: an event still to come finds no chip, and an
        // id the engine reuses later is reported as anyone else's would be.
        this.removing.delete(vrx);
        if (this.deps.audio.vrx === vrx) this.deps.audio.stop();
        this.list.remove(vrx);
        this.gone(vrx);
      },
      (e: unknown) => {
        this.removing.delete(vrx);
        this.deps.toast(errorText(e));
      },
    );
  }

  private add(hz: number): void {
    void this.call("addVrx", { hz: Math.round(hz), mode: DEFAULT_MODE }).then((r) => {
      if (!r) return;
      this.list.select(r.vrx);
      this.nowPlaying(r);
      this.listen(r.vrx);
    });
  }

  private listen(vrx: number): void {
    const audio = this.deps.audio;
    audio.start(vrx).then(
      async () => {
        this.deps.audioNote((await audio.resume()) ? null : NOT_STARTED);
      },
      (e: unknown) => this.deps.audioNote(`Live audio unavailable: ${errorText(e)}`),
    );
  }

  private replace(vrx: number, op: "setMode" | "setBandwidth", args: { [key: string]: JsonValue }): void {
    const gate = this.deps.link;
    gate.hold(vrx);
    this.replacing.add(vrx);
    this.deps.link.request(op, args).then(
      (reply) => {
        this.replacing.delete(vrx);
        const replaced = num((reply as { replaced?: JsonValue } | null)?.replaced);
        const r = this.fromReply(reply, replaced);
        if (r && replaced !== undefined && r.vrx !== replaced) {
          // Everything moves before the old id's removal is let through.
          this.deps.audio.retarget(replaced, r.vrx);
          this.deps.keep.retarget(replaced, r.vrx);
          this.remapped([{ old: replaced, new: r.vrx }]);
          const active = this.list.active === replaced;
          this.list.remove(replaced);
          if (active) this.list.select(r.vrx);
          this.nowPlaying(r);
        }
        gate.release(vrx);
      },
      (e: unknown) => {
        this.replacing.delete(vrx);
        this.deps.toast(errorText(e));
        gate.release(vrx);
      },
    );
  }

  // Sends op and applies a receiver-shaped reply. Undefined after a refusal,
  // which has been shown.
  private call(op: Op, args: { [key: string]: JsonValue }): Promise<Receiver | undefined> {
    return this.deps.link.request(op, args).then(
      (reply) => this.fromReply(reply),
      (e: unknown) => {
        this.deps.toast(errorText(e));
        return undefined;
      },
    );
  }

  private fromReply(reply: JsonValue, replaced?: number): Receiver | undefined {
    if (reply === null || typeof reply !== "object" || Array.isArray(reply)) return undefined;
    const vrx = num(reply.vrx);
    if (vrx === undefined) return undefined;
    // A replace's new receiver inherits the old one's place in the list.
    if (replaced !== undefined && replaced !== vrx && this.list.has(replaced) && !this.list.has(vrx)) {
      const old = this.list.get(replaced)!;
      this.list.upsert({ ...old, vrx });
    }
    const r = this.list.upsert({
      vrx,
      hz: num(reply.hz),
      mode: typeof reply.mode === "string" ? reply.mode : undefined,
      bandwidthHz: num(reply.bandwidthHz),
      squelchDbfs: num(reply.squelchDbfs),
      statusUnavailable: reply.statusUnavailable === true,
      transcribe: choiceOf(reply.transcribe),
      transcribing: typeof reply.transcribing === "boolean" ? reply.transcribing : undefined,
    });
    if (reply.clamped === true) {
      const why = typeof reply.clampReason === "string" && reply.clampReason !== "" ? reply.clampReason : "the engine clamped this receiver";
      this.deps.toast(why);
    }
    return r;
  }

  private nowPlaying(r: Receiver): void {
    if (this.list.active === r.vrx) this.deps.keep.setNowPlaying({ hz: r.hz, mode: r.mode });
  }

  private onStatus(e: GatewayEvent): void {
    const vrx = num(e.vrx);
    if (vrx === undefined || !this.list.has(vrx)) return;
    if (typeof e.audioError === "string") {
      this.list.upsert({ vrx, audioError: e.audioError, statusUnavailable: this.list.get(vrx)!.statusUnavailable });
      this.deps.toast(e.audioError);
    }
  }

  private onRemoved(e: GatewayEvent): void {
    const vrx = num(e.vrx);
    if (vrx === undefined) return;
    this.gone(vrx);
    if (this.removing.delete(vrx)) {
      this.list.remove(vrx);
      return;
    }
    const was = this.list.remove(vrx);
    if (!was) return;
    const reason = typeof e.reason === "string" && e.reason !== "" ? e.reason : "removed by the engine";
    this.deps.toast(`Receiver at ${formatMHz(was.hz)} MHz removed: ${reason}`);
  }

  private onLink(e: LinkEvent): void {
    // A rebuilt session has new ids; a removal asked for before it names an
    // id that may now belong to another receiver.
    if (e.recreated || e.lost) this.removing.clear();
    const lost = e.lost ?? [];
    // Lost ids are old ids: take them out before the map reuses them.
    for (const l of lost) {
      this.gone(l.vrx);
      const was = this.list.remove(l.vrx);
      const where = was ? `at ${formatMHz(was.hz)} MHz` : String(l.vrx);
      this.deps.toast(`Receiver ${where} lost when the engine restarted: ${l.reason}`);
    }
    this.list.remap(e.recreated ?? []);
    this.remapped(e.recreated ?? []);
  }
}

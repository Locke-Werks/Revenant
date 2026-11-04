// Captions pinned to the waterfall. A transcript names where the speech was in
// the engine's source sample clock (sourceStart..sourceEnd) and at what
// frequency, and every spectrum row carries the same clock (its source_start),
// so the text can be put on the rows the speech was on and scrolls with them.
//
// The row bookkeeping and the placement arithmetic are pure and tested; the
// DOM layer below only moves elements to where they say.

import type { GatewayEvent } from "../protocol";
import { hzToPx, type View } from "../view";
import { RING_ROWS } from "../waterfall";

// Bounds the elements the layer moves on every frame.
export const MAX_CAPTIONS = 20;
export const MAX_CAPTION_CHARS = 160;

// Index of the last row whose source_start is at or before sample: the row
// whose frame covers it. Rows run oldest first. Null when sample is older
// than the oldest row, which is the case that matters: that speech is off the
// top of what is remembered. A sample past the newest row is the newest row,
// since the newest frame has no successor to end it.
export function rowForSample(rows: { source_start: number }[], sample: number): number | null {
  return search(rows.length, (i) => rows[i]!.source_start, sample);
}

function search(n: number, at: (i: number) => number, sample: number): number | null {
  if (n === 0 || sample < at(0)) return null;
  let lo = 0;
  let hi = n - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (at(mid) <= sample) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

// The source_start of every row the waterfall still holds, in the same ring
// shape as the waterfall's own, so row age here is row age there. Starts are
// doubles: a sample counter stays exact to 2^53, which is years of samples.
export class RowClock {
  private readonly starts = new Float64Array(RING_ROWS);
  private count = 0;
  private head = -1;
  get size(): number {
    return this.count;
  }

  // Returns true when the new row proves the source restarted (its clock went
  // backwards): everything before it is forgotten. A new source counts from
  // its own zero, so an old row left in would sit in the wrong place.
  push(sourceStart: number): boolean {
    // A row without a usable clock cannot be placed against, and a NaN left in
    // the ring would break the search for every row after it.
    if (!Number.isFinite(sourceStart)) return false;
    let reset = false;
    if (this.count > 0 && sourceStart < this.starts[this.head]!) {
      this.clear();
      reset = true;
    }
    this.head = (this.head + 1) % RING_ROWS;
    this.starts[this.head] = sourceStart;
    this.count = Math.min(this.count + 1, RING_ROWS);
    return reset;
  }

  clear(): void {
    this.count = 0;
    this.head = -1;
  }

  // Rows back from the newest (0) that cover sample; null when it is older
  // than every row held.
  ageOf(sample: number): number | null {
    // Logical index 0 is the oldest row held, which sits `count - 1` slots
    // behind the head, wrapped.
    const slot = (i: number) => (this.head - (this.count - 1 - i) + RING_ROWS) % RING_ROWS;
    const i = search(this.count, (k) => this.starts[slot(k)]!, sample);
    return i === null ? null : this.count - 1 - i;
  }
}

export interface Caption {
  id: number;
  vrx: number;
  // Where the speech was heard, which may no longer be where its receiver is.
  hz: number;
  text: string;
  sourceStart: number;
  sourceEnd: number;
  confidence: number;
}

export type NewCaption = Omit<Caption, "id">;

// The vrx of a caption whose receiver is gone.
export const DETACHED = -1;

// The transcript event as a caption, or null when it lacks what pinning needs.
export function captionFromEvent(e: GatewayEvent): NewCaption | null {
  const num = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? v : null);
  const vrx = num(e.vrx);
  const hz = num(e.centerHz);
  const start = num(e.sourceStart);
  const end = num(e.sourceEnd);
  const text = typeof e.text === "string" ? e.text.trim() : "";
  if (vrx === null || hz === null || start === null || end === null || text === "") return null;
  const clipped = text.length > MAX_CAPTION_CHARS ? `${text.slice(0, MAX_CAPTION_CHARS - 1)}…` : text;
  return { vrx, hz, text: clipped, sourceStart: start, sourceEnd: Math.max(start, end), confidence: num(e.confidence) ?? 1 };
}

export class CaptionStore {
  private items: Caption[] = [];
  private nextId = 1;

  list(): readonly Caption[] {
    return this.items;
  }

  add(c: NewCaption): Caption {
    const caption = { ...c, id: this.nextId++ };
    this.items.push(caption);
    while (this.items.length > MAX_CAPTIONS) this.items.shift();
    return caption;
  }

  remove(id: number): void {
    this.items = this.items.filter((c) => c.id !== id);
  }

  clear(): void {
    this.items = [];
  }

  // A caption is pinned to where and when the speech was heard, not to its
  // receiver, so it outlives the receiver and scrolls off with its rows. A
  // trunk follower is removed the moment its call ends, which is exactly when
  // its last caption arrives. The id is cleared so a later receiver reusing
  // it, or a remap, cannot claim the caption.
  detachReceiver(vrx: number): void {
    this.items = this.items.map((c) => (c.vrx === vrx ? { ...c, vrx: DETACHED } : c));
  }

  // Old ids to new, as one step, the way ReceiverList.remap does it.
  remap(pairs: { old: number; new: number }[]): void {
    if (pairs.length === 0) return;
    const to = new Map(pairs.map((p) => [p.old, p.new]));
    this.items = this.items.map((c) => ({ ...c, vrx: to.get(c.vrx) ?? c.vrx }));
  }
}

export interface Placement {
  // CSS pixels from the waterfall's left edge, and from its top to the newest
  // row the speech covered; height runs to the oldest.
  x: number;
  top: number;
  height: number;
  opacity: number;
  // False when the frequency is panned out of view: kept, not shown.
  visible: boolean;
}

// How far outside the view a caption's frequency may sit and still show: the
// label is centred on it and wide.
const EDGE_PX = 80;
// Fully opaque for this share of the rows on screen, then fading to nothing
// at the bottom.
const FADE_FROM = 0.4;

// Null means the caption is gone for good: its rows have scrolled off.
export function placeCaption(
  c: Caption,
  clock: RowClock,
  view: View,
  widthCss: number,
  rowHeightCss: number,
  visibleRows: number,
  // Where the rows sit between arrivals, in rows: 0 at rest, negative while
  // the newest row is still scrolling in (smooth.ts).
  offsetRows = 0,
): Placement | null {
  const newest = clock.ageOf(c.sourceEnd);
  if (newest === null || newest >= visibleRows) return null;
  // The start may have scrolled off while the end has not: run to the oldest
  // row held, which is as far as the rows go.
  const oldest = Math.min(clock.ageOf(c.sourceStart) ?? clock.size - 1, visibleRows - 1);
  const x = hzToPx({ ...view, widthPx: widthCss }, c.hz);
  const f = newest / visibleRows;
  return {
    x,
    top: (newest + offsetRows) * rowHeightCss,
    height: (oldest - newest + 1) * rowHeightCss,
    opacity: f <= FADE_FROM ? 1 : Math.max(0, 1 - (f - FADE_FROM) / (1 - FADE_FROM)),
    visible: x >= -EDGE_PX && x <= widthCss + EDGE_PX,
  };
}

export interface LayerGeometry {
  view: View;
  widthCss: number;
  rowHeightCss: number;
  visibleRows: number;
  offsetRows?: number;
}

// The elements for the live captions inside the waterfall's box, which clips
// them. Text goes in as text.
export class CaptionLayer {
  private els = new Map<number, HTMLElement>();

  constructor(
    private readonly host: HTMLElement,
    private readonly store: CaptionStore,
    private readonly clock: RowClock,
    private readonly createEl: (tag: string) => HTMLElement = (tag) => document.createElement(tag),
  ) {}

  layout(g: LayerGeometry): void {
    const live = new Set<number>();
    for (const c of this.store.list()) {
      const p = placeCaption(c, this.clock, g.view, g.widthCss, g.rowHeightCss, g.visibleRows, g.offsetRows);
      if (!p) continue;
      live.add(c.id);
      let el = this.els.get(c.id);
      if (!el) {
        el = this.build(c);
        this.els.set(c.id, el);
        this.host.append(el);
      }
      el.hidden = !p.visible;
      el.style.transform = `translate(${p.x.toFixed(1)}px, ${p.top.toFixed(1)}px)`;
      el.style.height = `${Math.max(p.height, 2).toFixed(1)}px`;
      el.style.opacity = p.opacity.toFixed(2);
    }
    // Scrolled off, or removed from the store by a source change.
    for (const [id, el] of this.els) {
      if (live.has(id)) continue;
      el.remove();
      this.els.delete(id);
      this.store.remove(id);
    }
  }

  private build(c: Caption): HTMLElement {
    const el = this.createEl("div");
    el.className = c.confidence < 0.5 ? "caption doubt" : "caption";
    const text = this.createEl("div");
    text.className = "caption-text";
    text.textContent = c.text;
    el.append(text);
    return el;
  }
}

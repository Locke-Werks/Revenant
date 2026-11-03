// The waterfall. Rows live in a texture used as a ring buffer, one row per
// spectrum frame, newest drawn at the top. Each row remembers the view it was
// cut for, and the shader places it by that view, so during a pan or zoom old
// rows slide and stretch with the fingers instead of jumping when the gateway
// catches up.
//
// WebGL2 when available (every iOS since 15); otherwise a 2D canvas that
// scrolls its own pixels and cannot re-place history.
//
// The colours are the desktop's, from the gateway's display event. A new
// palette recolours the whole history under WebGL, where the palette is a
// lookup at draw time over the stored bytes. The 2D canvas keeps no bytes,
// only pixels, so there a new palette colours new rows and the rows already
// drawn keep theirs until they scroll off.

import { DEFAULT_DISPLAY } from "./display";
import { boxPx, parseHexColor, type DetectionBox } from "./markers";
import type { SpectrumFrame } from "./protocol";
import { MAX_WIDTH_PX, rowTransform, type RowTransform, type View } from "./view";

// About 28 s of history at 2.4 MS/s, where the gateway sends every one of
// the engine's 36.6 rows a second.
export const RING_ROWS = 1024;

// Four missed polls at 2 Hz before boxes stop, whatever the row rate.
export const BOX_CARRY_MS = 2000;

// That time in rows at a row interval in ms, as the pacer measures it.
export function boxCarryRows(intervalMs: number): number {
  if (!(intervalMs > 0)) return 1;
  return Math.min(RING_ROWS, Math.max(1, Math.round(BOX_CARRY_MS / intervalMs)));
}

// The carry at 2.4 MS/s, for a caller that has no measured interval.
export const BOX_CARRY_ROWS = boxCarryRows(1000 / 36.6);

// Where rows sit while the waterfall scrolls between them. shift is in rows,
// 0 at rest and negative while the newest row is still coming in (smooth.ts:
// the pacer's phase less one). Row k, counted from the newest, spans
// [rowTop(k), rowTop(k) + 1) rows down the screen; the shader finds a
// pixel's row with rowAt. Both renderer and overlays use these, which is what
// keeps a box, a caption and its row together mid-scroll.
export function rowTop(k: number, shift: number): number {
  return k + shift;
}
export function rowAt(down: number, shift: number): number {
  return Math.floor(down - shift);
}
const NO_BOXES: readonly DetectionBox[] = [];

// Where each slot's row came from, and the per-slot transform for the view on
// screen. Pure bookkeeping, shared by both renderers and tested on its own.
export class RowRing {
  readonly center = new Float64Array(RING_ROWS);
  readonly span = new Float64Array(RING_ROWS);
  readonly width = new Uint32Array(RING_ROWS);
  // RGBA per slot: a, b, limit, unused. Uploaded as a float texture.
  readonly meta = new Float32Array(RING_ROWS * 4);
  // Detection boxes per slot, in hertz. Overwritten with the slot, so a box
  // leaves the screen with the row it was stamped on.
  readonly boxes: (readonly DetectionBox[])[] = new Array<readonly DetectionBox[]>(RING_ROWS).fill(NO_BOXES);
  head = -1;
  private scratch: RowTransform = { a: 0, b: 0, limit: 0 };
  private carry: readonly DetectionBox[] = NO_BOXES;
  private carryLeft = 0;

  constructor(readonly texWidth: number) {}

  // Returns the slot the row goes in.
  push(centerHz: number, spanHz: number, widthPx: number): number {
    this.head = (this.head + 1) % RING_ROWS;
    this.center[this.head] = centerHz;
    this.span[this.head] = spanHz;
    this.width[this.head] = Math.min(widthPx, this.texWidth);
    this.boxes[this.head] = this.carryLeft > 0 ? this.carry : NO_BOXES;
    if (this.carryLeft > 0) this.carryLeft--;
    return this.head;
  }

  // Detections arrive a few times a second and rows dozens of times, so the rows
  // between two events carry the latest list; a list nobody refreshes stops
  // being drawn rather than painting a stale signal down the screen.
  stamp(boxes: readonly DetectionBox[], carryRows = BOX_CARRY_ROWS): void {
    this.carry = boxes;
    this.carryLeft = carryRows;
    if (this.head >= 0) this.boxes[this.head] = boxes;
  }

  // Boxes on one slot in pixels across a canvas showing `view`, clipped to
  // what that row covered.
  boxesPx(slot: number, view: View, widthPx: number): { x0: number; x1: number; box: DetectionBox }[] {
    const out: { x0: number; x1: number; box: DetectionBox }[] = [];
    if (this.width[slot] === 0) return out;
    const row = { centerHz: this.center[slot]!, spanHz: this.span[slot]! };
    for (const box of this.boxes[slot]!) {
      const p = boxPx(view, widthPx, row, box);
      if (p) out.push({ x0: p.x0, x1: p.x1, box });
    }
    return out;
  }

  // Recomputes one slot's transform, or all of them when the view moved.
  place(view: View, slot?: number): void {
    const from = slot ?? 0;
    const to = slot === undefined ? RING_ROWS : slot + 1;
    for (let i = from; i < to; i++) {
      const o = i * 4;
      const w = this.width[i]!;
      if (w === 0 || !(this.span[i]! > 0)) {
        this.meta[o] = this.meta[o + 1] = this.meta[o + 2] = 0;
        continue;
      }
      const t = rowTransform(
        view,
        { centerHz: this.center[i]!, spanHz: this.span[i]!, widthPx: w },
        this.texWidth,
        this.scratch,
      );
      this.meta[o] = t.a;
      this.meta[o + 1] = t.b;
      this.meta[o + 2] = t.limit;
    }
  }

  clear(): void {
    this.width.fill(0);
    this.meta.fill(0);
    this.boxes.fill(NO_BOXES);
    this.head = -1;
  }
}

export interface Renderer {
  resize(widthDev: number, heightDev: number): void;
  setView(view: View): void;
  pushRow(f: SpectrumFrame): void;
  // 256 RGBA entries, indexed by a row's byte.
  setPalette(rgba: Uint8Array): void;
  clear(): void;
  // Whether draw honours a shift. One that does not steps a row at a time.
  readonly scrolls: boolean;
  // shift: see rowTop.
  draw(shift?: number): void;
  // Stamps the latest detections onto the row current now, carried over
  // carryRows more (boxCarryRows).
  setDetections(boxes: readonly DetectionBox[], carryRows?: number): void;
  // Paints history's detection boxes onto an overlay the size of the
  // waterfall, at the shift the waterfall was drawn at. A renderer that burns
  // them into its own pixels does nothing.
  drawBoxes(ctx: CanvasRenderingContext2D, accent: string, shift?: number): void;
}

const VS = `#version 300 es
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

const FS = `#version 300 es
precision highp float;
precision highp int;
uniform sampler2D u_data;
uniform highp sampler2D u_meta;
uniform sampler2D u_pal;
uniform int u_head;
uniform vec2 u_size;
uniform float u_texw;
uniform float u_shift;
out vec4 o;
const int ROWS = ${RING_ROWS};
void main() {
  float s = gl_FragCoord.x / u_size.x;
  float down = (u_size.y - gl_FragCoord.y) / u_size.y;
  // rowAt in waterfall.ts. Past the oldest row is no row: the scroll has
  // pushed it off the bottom.
  int k = int(floor(down * float(ROWS) - u_shift));
  int slot = (u_head - min(k, ROWS - 1) + ROWS) % ROWS;
  vec4 m = texelFetch(u_meta, ivec2(slot, 0), 0);
  float u = m.x + m.y * s;
  float v = 0.0;
  if (k < ROWS && m.z > 0.0 && u >= 0.0 && u < m.z) {
    // Linear filtering at the row's last texel would blend in whatever an
    // earlier, wider row left beyond it.
    u = min(u, m.z - 0.5 / u_texw);
    v = texture(u_data, vec2(u, (float(slot) + 0.5) / float(ROWS))).r;
  }
  o = texture(u_pal, vec2((v * 255.0 + 0.5) / 256.0, 0.5));
}`;

class GlRenderer implements Renderer {
  private ring: RowRing;
  private prog: WebGLProgram;
  private data: WebGLTexture;
  private metaTex: WebGLTexture;
  private pal: WebGLTexture;
  private uHead: WebGLUniformLocation | null;
  private uSize: WebGLUniformLocation | null;
  private uTexw: WebGLUniformLocation | null;
  private uShift: WebGLUniformLocation | null;
  readonly scrolls = true;
  private view: View | null = null;
  private metaDirty = true;

  constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly gl: WebGL2RenderingContext,
  ) {
    const texWidth = Math.min(MAX_WIDTH_PX, gl.getParameter(gl.MAX_TEXTURE_SIZE) as number);
    this.ring = new RowRing(texWidth);
    this.prog = link(gl, VS, FS);
    gl.useProgram(this.prog);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    this.data = texture(gl, 0, gl.LINEAR);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, texWidth, RING_ROWS, 0, gl.RED, gl.UNSIGNED_BYTE, null);
    this.metaTex = texture(gl, 1, gl.NEAREST);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, RING_ROWS, 1, 0, gl.RGBA, gl.FLOAT, this.ring.meta);
    this.pal = texture(gl, 2, gl.LINEAR);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, DEFAULT_DISPLAY.waterfall);

    gl.uniform1i(gl.getUniformLocation(this.prog, "u_data"), 0);
    gl.uniform1i(gl.getUniformLocation(this.prog, "u_meta"), 1);
    gl.uniform1i(gl.getUniformLocation(this.prog, "u_pal"), 2);
    this.uHead = gl.getUniformLocation(this.prog, "u_head");
    this.uSize = gl.getUniformLocation(this.prog, "u_size");
    this.uTexw = gl.getUniformLocation(this.prog, "u_texw");
    this.uShift = gl.getUniformLocation(this.prog, "u_shift");
    gl.uniform1f(this.uTexw, texWidth);
    // A float texture the driver refused leaves texelFetch returning zeros,
    // which draws a blank waterfall without any error; fail to 2D instead.
    const err = gl.getError();
    if (err !== gl.NO_ERROR) throw new Error(`WebGL setup failed, error 0x${err.toString(16)}`);
    // A fresh texture is not guaranteed zeroed on every driver.
    this.clear();
  }

  resize(w: number, h: number): void {
    this.canvas.width = w;
    this.canvas.height = h;
  }

  setView(view: View): void {
    this.view = view;
    this.metaDirty = true;
  }

  pushRow(f: SpectrumFrame): void {
    const gl = this.gl;
    const slot = this.ring.push(f.centerHz, f.spanHz, f.px.length);
    const w = this.ring.width[slot]!;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.data);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, slot, w, 1, gl.RED, gl.UNSIGNED_BYTE, f.px.subarray(0, w));
    if (this.view && !this.metaDirty) {
      this.ring.place(this.view, slot);
      this.uploadMeta(slot);
    }
  }

  setPalette(rgba: Uint8Array): void {
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, this.pal);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 256, 1, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
  }

  clear(): void {
    this.ring.clear();
    this.metaDirty = true;
  }

  draw(shift = 0): void {
    const gl = this.gl;
    if (this.metaDirty && this.view) {
      this.ring.place(this.view);
      this.uploadMeta();
      this.metaDirty = false;
    } else if (this.metaDirty) {
      this.uploadMeta();
    }
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.uniform1i(this.uHead, Math.max(0, this.ring.head));
    gl.uniform2f(this.uSize, this.canvas.width, this.canvas.height);
    gl.uniform1f(this.uShift, shift);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  setDetections(boxes: readonly DetectionBox[], carryRows = BOX_CARRY_ROWS): void {
    this.ring.stamp(boxes, carryRows);
  }

  drawBoxes(ctx: CanvasRenderingContext2D, accent: string, shift = 0): void {
    const ring = this.ring;
    const view = this.view;
    if (!view || ring.head < 0) return;
    const w = ctx.canvas.width;
    const rowH = ctx.canvas.height / RING_ROWS;
    ctx.fillStyle = accent;
    // Neighbouring rows usually share one list and one view, so each run of
    // them is one rectangle per box instead of one per row.
    let k = 0;
    while (k < RING_ROWS) {
      const slot = (ring.head - k + RING_ROWS) % RING_ROWS;
      const boxes = ring.boxes[slot]!;
      let end = k + 1;
      while (end < RING_ROWS) {
        const s = (ring.head - end + RING_ROWS) % RING_ROWS;
        if (ring.boxes[s] !== boxes || ring.center[s] !== ring.center[slot] || ring.span[s] !== ring.span[slot]) break;
        end++;
      }
      if (boxes.length > 0) {
        const y = rowTop(k, shift) * rowH;
        const hh = (end - k) * rowH;
        for (const p of ring.boxesPx(slot, view, w)) {
          ctx.globalAlpha = p.box.alpha;
          ctx.fillRect(p.x0, y, p.x1 - p.x0, hh);
          // One device pixel at each band edge carries the box; the fill is
          // only a tint.
          ctx.globalAlpha = p.box.held ? 0.3 : 0.55;
          ctx.fillRect(Math.floor(p.x0), y, 1, hh);
          ctx.fillRect(Math.max(Math.floor(p.x0) + 1, Math.ceil(p.x1) - 1), y, 1, hh);
        }
      }
      k = end;
    }
    ctx.globalAlpha = 1;
  }

  private uploadMeta(slot?: number): void {
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.metaTex);
    if (slot === undefined) {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, RING_ROWS, 1, gl.RGBA, gl.FLOAT, this.ring.meta);
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, slot, 0, 1, 1, gl.RGBA, gl.FLOAT, this.ring.meta, slot * 4);
    }
  }
}

function texture(gl: WebGL2RenderingContext, unit: number, filter: number): WebGLTexture {
  const t = gl.createTexture();
  if (!t) throw new Error("createTexture failed");
  gl.activeTexture(gl.TEXTURE0 + unit);
  gl.bindTexture(gl.TEXTURE_2D, t);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  return t;
}

function link(gl: WebGL2RenderingContext, vs: string, fs: string): WebGLProgram {
  const p = gl.createProgram();
  if (!p) throw new Error("createProgram failed");
  for (const [type, src] of [
    [gl.VERTEX_SHADER, vs],
    [gl.FRAGMENT_SHADER, fs],
  ] as const) {
    const s = gl.createShader(type);
    if (!s) throw new Error("createShader failed");
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) ?? "shader");
    gl.attachShader(p, s);
  }
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p) ?? "link");
  return p;
}

// Scrolls the canvas down by ROW_PX and paints the new row at the top, placed
// by its own view. History stays where it was drawn when the view moves.
const ROW_PX = 2;

class CanvasRenderer implements Renderer {
  private ctx: CanvasRenderingContext2D;
  private line: ImageData | null = null;
  private pal = DEFAULT_DISPLAY.waterfall;
  private view: View | null = null;
  private t: RowTransform = { a: 0, b: 0, limit: 0 };
  private carry: readonly DetectionBox[] = NO_BOXES;
  private carryLeft = 0;

  constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly accent: readonly [number, number, number],
  ) {
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("no 2D context");
    this.ctx = ctx;
  }

  resize(w: number, h: number): void {
    this.canvas.width = w;
    this.canvas.height = h;
    this.line = this.ctx.createImageData(w, 1);
    this.clear();
  }

  setView(view: View): void {
    this.view = view;
  }

  pushRow(f: SpectrumFrame): void {
    const view = this.view;
    const line = this.line;
    if (!view || !line) return;
    const w = this.canvas.width;
    const rw = f.px.length;
    const t = rowTransform(view, { centerHz: f.centerHz, spanHz: f.spanHz, widthPx: rw }, rw, this.t);
    const d = line.data;
    for (let x = 0; x < w; x++) {
      const u = t.a + t.b * ((x + 0.5) / w);
      const v = u >= 0 && u < 1 ? f.px[Math.floor(u * rw)]! : 0;
      d[x * 4] = this.pal[v * 4]!;
      d[x * 4 + 1] = this.pal[v * 4 + 1]!;
      d[x * 4 + 2] = this.pal[v * 4 + 2]!;
      d[x * 4 + 3] = 255;
    }
    // History here is pixels that never move again, so boxes are burnt into
    // the row as it is drawn and scroll off with it.
    const boxes = this.carryLeft > 0 ? this.carry : NO_BOXES;
    if (this.carryLeft > 0) this.carryLeft--;
    const [ar, ag, ab] = this.accent;
    for (const box of boxes) {
      const p = boxPx(view, w, f, box);
      if (!p) continue;
      const x0 = Math.floor(p.x0);
      const x1 = Math.ceil(p.x1);
      for (let x = x0; x < x1; x++) {
        // The edges carry the box; the inside is a tint.
        const k = x === x0 || x === x1 - 1 ? (box.held ? 0.3 : 0.55) : box.alpha;
        const o = x * 4;
        d[o] = d[o]! + (ar - d[o]!) * k;
        d[o + 1] = d[o + 1]! + (ag - d[o + 1]!) * k;
        d[o + 2] = d[o + 2]! + (ab - d[o + 2]!) * k;
      }
    }
    this.ctx.drawImage(this.canvas, 0, ROW_PX);
    for (let y = 0; y < ROW_PX; y++) this.ctx.putImageData(line, 0, y);
  }

  setPalette(rgba: Uint8Array): void {
    this.pal = rgba;
  }

  // Entry 0, the desktop's map bottom, so an empty history is an empty band.
  clear(): void {
    const p = this.pal;
    this.ctx.fillStyle = `rgb(${p[0]}, ${p[1]}, ${p[2]})`;
    this.ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
  }

  readonly scrolls = false;

  draw(): void {}

  setDetections(boxes: readonly DetectionBox[], carryRows = BOX_CARRY_ROWS): void {
    this.carry = boxes;
    this.carryLeft = carryRows;
  }

  drawBoxes(): void {}
}

// How long a lost WebGL context gets to come back before the 2D renderer
// takes over.
export const RESTORE_WAIT_MS = 3000;

export interface RendererFactories {
  // Null or a throw when WebGL2 is unavailable or fails to set up.
  gl(): Renderer | null;
  canvas2d(): Renderer;
}

// Owns whichever renderer is live and survives a lost GL context: iOS drops
// contexts when the app is backgrounded under memory pressure. While lost,
// rows and draws are dropped; size and view are remembered and applied to the
// replacement. History is not kept across a loss.
export class WaterfallHost implements Renderer {
  state: "gl" | "lost" | "2d";
  private r: Renderer | null;
  private size: [number, number] | null = null;
  private view: View | null = null;
  private palette: Uint8Array | null = null;
  private wait: unknown = null;

  constructor(
    private readonly make: RendererFactories,
    private readonly timers: { setTimeout(fn: () => void, ms: number): unknown; clearTimeout(h: unknown): void },
  ) {
    const g = this.tryGl();
    this.r = g ?? make.canvas2d();
    this.state = g ? "gl" : "2d";
  }

  contextLost(): void {
    if (this.state !== "gl") return;
    this.state = "lost";
    this.r = null;
    this.wait = this.timers.setTimeout(() => {
      this.wait = null;
      if (this.state === "lost") this.become2d();
    }, RESTORE_WAIT_MS);
  }

  contextRestored(): void {
    if (this.state !== "lost") return;
    if (this.wait !== null) this.timers.clearTimeout(this.wait);
    this.wait = null;
    const g = this.tryGl();
    if (g) {
      this.state = "gl";
      this.adopt(g);
    } else {
      this.become2d();
    }
  }

  resize(w: number, h: number): void {
    this.size = [w, h];
    this.r?.resize(w, h);
  }
  setView(view: View): void {
    this.view = view;
    this.r?.setView(view);
  }
  pushRow(f: SpectrumFrame): void {
    this.r?.pushRow(f);
  }
  setPalette(rgba: Uint8Array): void {
    this.palette = rgba;
    this.r?.setPalette(rgba);
  }
  clear(): void {
    this.r?.clear();
  }
  get scrolls(): boolean {
    return this.r?.scrolls ?? false;
  }
  draw(shift = 0): void {
    this.r?.draw(shift);
  }
  setDetections(boxes: readonly DetectionBox[], carryRows = BOX_CARRY_ROWS): void {
    this.r?.setDetections(boxes, carryRows);
  }
  drawBoxes(ctx: CanvasRenderingContext2D, accent: string, shift = 0): void {
    this.r?.drawBoxes(ctx, accent, shift);
  }

  private become2d(): void {
    this.state = "2d";
    this.adopt(this.make.canvas2d());
  }

  private adopt(r: Renderer): void {
    this.r = r;
    // Before the resize, which clears a 2D canvas to the palette's bottom.
    if (this.palette) r.setPalette(this.palette);
    if (this.size) r.resize(this.size[0], this.size[1]);
    if (this.view) r.setView(this.view);
  }

  private tryGl(): Renderer | null {
    try {
      return this.make.gl();
    } catch (e) {
      console.error("waterfall: WebGL2 failed, using 2D", e);
      return null;
    }
  }
}

export function createWaterfall(first: HTMLCanvasElement, accent: string): WaterfallHost {
  const accentRgb = parseHexColor(accent);
  let canvas = first;
  let used = false;
  // A canvas that has handed out a webgl2 context cannot give a 2d one, so
  // each fallback gets a fresh element in the same place.
  const fresh = () => {
    if (!used) return canvas;
    const c = canvas.cloneNode(false) as HTMLCanvasElement;
    canvas.replaceWith(c);
    canvas = c;
    return c;
  };
  let host: WaterfallHost | null = null;
  host = new WaterfallHost(
    {
      gl: () => {
        const c = used ? canvas : fresh();
        used = true;
        const gl = c.getContext("webgl2", { antialias: false, alpha: false, preserveDrawingBuffer: false });
        if (!gl || gl.isContextLost()) return null;
        return new GlRenderer(c, gl);
      },
      canvas2d: () => new CanvasRenderer(fresh(), accentRgb),
    },
    { setTimeout: (fn, ms) => window.setTimeout(fn, ms), clearTimeout: (h) => window.clearTimeout(h as number) },
  );
  canvas.addEventListener("webglcontextlost", (e) => {
    // Without preventDefault the browser never offers the context back.
    e.preventDefault();
    console.warn("waterfall: WebGL context lost");
    host?.contextLost();
  });
  canvas.addEventListener("webglcontextrestored", () => {
    console.warn("waterfall: WebGL context restored");
    host?.contextRestored();
  });
  return host;
}

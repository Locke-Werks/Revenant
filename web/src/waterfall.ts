// The waterfall. Rows live in a texture used as a ring buffer, one row per
// spectrum frame, newest drawn at the top. Each row remembers the view it was
// cut for, and the shader places it by that view, so during a pan or zoom old
// rows slide and stretch with the fingers instead of jumping when the gateway
// catches up.
//
// WebGL2 when available (every iOS since 15); otherwise a 2D canvas that
// scrolls its own pixels and cannot re-place history.

import { colormap } from "./colormap";
import type { SpectrumFrame } from "./protocol";
import { MAX_WIDTH_PX, rowTransform, type RowTransform, type View } from "./view";

// About 50 s of history at the gateway's 20 rows/s.
export const RING_ROWS = 1024;

// Where each slot's row came from, and the per-slot transform for the view on
// screen. Pure bookkeeping, shared by both renderers and tested on its own.
export class RowRing {
  readonly center = new Float64Array(RING_ROWS);
  readonly span = new Float64Array(RING_ROWS);
  readonly width = new Uint32Array(RING_ROWS);
  // RGBA per slot: a, b, limit, unused. Uploaded as a float texture.
  readonly meta = new Float32Array(RING_ROWS * 4);
  head = -1;
  private scratch: RowTransform = { a: 0, b: 0, limit: 0 };

  constructor(readonly texWidth: number) {}

  // Returns the slot the row goes in.
  push(centerHz: number, spanHz: number, widthPx: number): number {
    this.head = (this.head + 1) % RING_ROWS;
    this.center[this.head] = centerHz;
    this.span[this.head] = spanHz;
    this.width[this.head] = Math.min(widthPx, this.texWidth);
    return this.head;
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
    this.head = -1;
  }
}

export interface Renderer {
  resize(widthDev: number, heightDev: number): void;
  setView(view: View): void;
  pushRow(f: SpectrumFrame): void;
  clear(): void;
  draw(): void;
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
out vec4 o;
const int ROWS = ${RING_ROWS};
void main() {
  float s = gl_FragCoord.x / u_size.x;
  float down = (u_size.y - gl_FragCoord.y) / u_size.y;
  int k = min(int(down * float(ROWS)), ROWS - 1);
  int slot = (u_head - k + ROWS) % ROWS;
  vec4 m = texelFetch(u_meta, ivec2(slot, 0), 0);
  float u = m.x + m.y * s;
  float v = 0.0;
  if (m.z > 0.0 && u >= 0.0 && u < m.z) {
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
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, colormap());

    gl.uniform1i(gl.getUniformLocation(this.prog, "u_data"), 0);
    gl.uniform1i(gl.getUniformLocation(this.prog, "u_meta"), 1);
    gl.uniform1i(gl.getUniformLocation(this.prog, "u_pal"), 2);
    this.uHead = gl.getUniformLocation(this.prog, "u_head");
    this.uSize = gl.getUniformLocation(this.prog, "u_size");
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

  clear(): void {
    this.ring.clear();
    this.metaDirty = true;
  }

  draw(): void {
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
    gl.drawArrays(gl.TRIANGLES, 0, 3);
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
  private pal = colormap();
  private view: View | null = null;
  private t: RowTransform = { a: 0, b: 0, limit: 0 };

  constructor(private readonly canvas: HTMLCanvasElement) {
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
    this.ctx.drawImage(this.canvas, 0, ROW_PX);
    for (let y = 0; y < ROW_PX; y++) this.ctx.putImageData(line, 0, y);
  }

  clear(): void {
    this.ctx.fillStyle = "#141418";
    this.ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
  }

  draw(): void {}
}

export function createWaterfall(canvas: HTMLCanvasElement): Renderer {
  const gl = canvas.getContext("webgl2", { antialias: false, alpha: false, preserveDrawingBuffer: false });
  if (gl) {
    try {
      return new GlRenderer(canvas, gl);
    } catch (e) {
      // A canvas that handed out a webgl2 context cannot give a 2d one, so the
      // fallback needs a fresh element in the same place.
      console.error("waterfall: WebGL2 failed, using 2D", e);
      const fresh = canvas.cloneNode(false) as HTMLCanvasElement;
      canvas.replaceWith(fresh);
      return new CanvasRenderer(fresh);
    }
  }
  return new CanvasRenderer(canvas);
}

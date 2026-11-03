import { test } from "node:test";
import assert from "node:assert/strict";
import { DEFAULT_DISPLAY, fillStops, paletteFromHex, parseDisplay } from "../src/display";
import {
  decodeBinary,
  encodeSpectrum,
  SPECTRUM_HEADER_SIZE,
  Tag,
  type BinaryFrame,
  type SpectrumFrame,
} from "../src/protocol";
import { traceRuns } from "../src/spectrum_trace";
import { levelInBand } from "../src/ui/top_bar";

const hex256 = (rgb: string) => rgb.repeat(256);

function must<T>(x: T | null | undefined): T {
  if (x === null || x === undefined) throw new Error("expected a value");
  return x;
}

function spectrumOf(f: BinaryFrame | null): SpectrumFrame {
  if (!f || f.tag !== Tag.Spectrum) throw new Error("expected a spectrum frame");
  return f;
}

test("a palette is 256 rrggbb entries made RGBA", () => {
  const p = must(paletteFromHex("102030" + "a0b0c0".repeat(254) + "ffeedd"));
  assert.equal(p.length, 1024);
  assert.deepEqual([...p.subarray(0, 4)], [0x10, 0x20, 0x30, 255]);
  assert.deepEqual([...p.subarray(1020, 1024)], [0xff, 0xee, 0xdd, 255]);
  // Short, long or not hex is refused rather than drawn half-filled.
  assert.equal(paletteFromHex("102030"), null);
  assert.equal(paletteFromHex(hex256("102030") + "00"), null);
  assert.equal(paletteFromHex(hex256("10203g")), null);
});

test("the display event gives both palettes and the trace's colours", () => {
  const d = must(parseDisplay({
    event: "display",
    waterfall: hex256("010203"),
    spectrum: hex256("040506"),
    trace: "#fef6d4",
    background: "#000000",
  }));
  assert.deepEqual([...d.waterfall.subarray(0, 4)], [1, 2, 3, 255]);
  assert.deepEqual([...d.spectrum.subarray(0, 4)], [4, 5, 6, 255]);
  assert.equal(d.trace, "#fef6d4");
  assert.equal(d.background, "#000000");
  assert.equal(parseDisplay({ event: "display", waterfall: "zz" }), null);
  assert.equal(parseDisplay({ event: "display", waterfall: hex256("010203"), spectrum: hex256("040506"), trace: "red", background: "#000000" }), null);
});

test("before any event the phone draws the desktop's default theme", () => {
  // Aurora's bottom is the desktop window's background, #06080e.
  assert.deepEqual([...DEFAULT_DISPLAY.waterfall.subarray(0, 4)], [0x06, 0x08, 0x0e, 255]);
  assert.deepEqual([...DEFAULT_DISPLAY.spectrum.subarray(1020, 1024)], [0xfe, 0xf6, 0xd4, 255]);
  assert.equal(DEFAULT_DISPLAY.background, "#080a0e");
});

test("the fill gradient runs bottom of the palette at the plot's foot", () => {
  const p = paletteFromHex("000000" + "808080".repeat(254) + "ffffff")!;
  const stops = fillStops(p, 3);
  assert.deepEqual(stops[0], { at: 0, rgba: "rgba(255, 255, 255, 0.549)" });
  assert.deepEqual(stops[2], { at: 1, rgba: "rgba(0, 0, 0, 0.549)" });
});

test("a frame carries its trace row in its own window", () => {
  const px = new Uint8Array([0, 128, 255]);
  const tracePx = new Uint8Array([10, 20, 30]);
  const buf = encodeSpectrum({
    offsetDb: -100, scaleDb: 0.25, sourceStart: 7n, centerHz: 1e8, spanHz: 3e3, px,
    traceOffsetDb: -120, traceScaleDb: 0.5, tracePx,
  });
  assert.equal(buf.byteLength, SPECTRUM_HEADER_SIZE + 3 + 8 + 3);
  const f = spectrumOf(decodeBinary(buf));
  assert.equal(f.traceOffsetDb, -120);
  assert.equal(f.traceScaleDb, 0.5);
  assert.deepEqual([...f.tracePx], [10, 20, 30]);
  assert.deepEqual([...f.px], [0, 128, 255]);

  // An older gateway's frame has no trace: the waterfall row stands in.
  const old = new Uint8Array(buf.slice(0, SPECTRUM_HEADER_SIZE + 3)).buffer;
  const g = spectrumOf(decodeBinary(old));
  assert.deepEqual([...g.tracePx], [0, 128, 255]);
  assert.equal(g.traceOffsetDb, -100);
  assert.equal(g.traceScaleDb, 0.25);
});

test("the trace is drawn from its own row, the window's top at the plot's top", () => {
  const view = { centerHz: 1000, spanHz: 4, widthPx: 8 };
  const f = { centerHz: 1000, spanHz: 4, px: new Uint8Array([0, 0, 0, 0]), tracePx: new Uint8Array([255, 255, 0, 0]) };
  const runs = traceRuns(f, view, 8, 101);
  assert.equal(runs.length, 1);
  const run = runs[0]!;
  assert.deepEqual(run.slice(0, 2), [0, 1]);
  assert.deepEqual(run.slice(-2), [6, 100]);
});

test("the meter reads the trace's window, which keeps room above the peak", () => {
  const f = {
    offsetDb: -100, scaleDb: 0.1, sourceStart: 0n, centerHz: 1000, spanHz: 4, px: new Uint8Array([255, 255, 255, 255]),
    traceOffsetDb: -120, traceScaleDb: 0.5, tracePx: new Uint8Array([0, 100, 0, 0]),
  };
  assert.equal(levelInBand(f, 998, 1002), -70);
});

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { FRAME } from "../src/audio/jitter";
import { createPcmDecoder, type OpusDecoderModule } from "../src/audio/opus_decoder";

// Paths are relative to web/, where npm test runs.
const VENDOR = "static/vendor/opus-decoder.min.js";
// Written by tests/webgate/test_audio_opus.cpp ("[write-fixture]") from the
// gateway's own LiveOpusEncoder: 20 packets of a 0.5 amplitude 1 kHz tone,
// each a little-endian u16 length then the packet.
const FIXTURE = "test/fixtures/tone1k.opus";

// Evaluates the vendored UMD file the way a classic <script> does, with no
// CommonJS or AMD in sight. Its wrapper extends globalThis.Worker at load,
// which node lacks; the decoder used here never touches it.
function loadVendored(): OpusDecoderModule {
  const g = globalThis as unknown as Record<string, unknown>;
  g.Worker ??= class {};
  new Function("exports", "module", "define", readFileSync(VENDOR, "utf8")).call(globalThis, undefined, undefined, undefined);
  return g["opus-decoder"] as OpusDecoderModule;
}

function readPackets(): Uint8Array[] {
  const b = readFileSync(FIXTURE);
  const out: Uint8Array[] = [];
  let at = 0;
  while (at + 2 <= b.length) {
    const len = b[at]! | (b[at + 1]! << 8);
    at += 2;
    out.push(b.subarray(at, at + len));
    at += len;
  }
  assert.equal(at, b.length);
  return out;
}

test("opus: the vendored decoder plays the gateway encoder's 1 kHz tone", async () => {
  const dec = await createPcmDecoder(loadVendored());
  const packets = readPackets();
  assert.equal(packets.length, 20);
  const pcm: Float32Array[] = [];
  for (const p of packets) {
    const f = dec.decode(p);
    if (!f) throw new Error("packet refused");
    assert.equal(f.length, FRAME);
    pcm.push(f);
  }
  // Skip the codec's start-up; a 0.5 amplitude sine is 0.354 RMS.
  let sum = 0;
  let n = 0;
  let crossings = 0;
  for (const f of pcm.slice(4)) {
    for (let i = 0; i < f.length; ++i) {
      sum += f[i]! * f[i]!;
      ++n;
      if (i > 0 && f[i - 1]! < 0 && f[i]! >= 0) ++crossings;
    }
  }
  const rms = Math.sqrt(sum / n);
  assert.ok(rms > 0.25 && rms < 0.45, `rms ${rms}`);
  // 1 kHz: one upward crossing per millisecond, 20 per packet.
  const perPacket = crossings / pcm.slice(4).length;
  assert.ok(perPacket > 18 && perPacket < 22, `crossings per packet ${perPacket}`);
  // Each frame is a copy, so transferring one to the worklet is safe.
  assert.ok(pcm[0]!.buffer !== pcm[1]!.buffer);
  dec.free();
});

test("opus: refused packets decode to null and are logged once a second", async () => {
  const warned: string[] = [];
  let now = 0;
  const dec = await createPcmDecoder(loadVendored(), { warn: (m) => warned.push(m), now: () => now });
  // TOC byte for a code 3 packet with a frame count of zero: invalid.
  const bad = new Uint8Array([0x03, 0x00]);
  assert.equal(dec.decode(bad), null);
  for (let i = 0; i < 49; ++i) {
    now += 20;
    assert.equal(dec.decode(bad), null);
  }
  now = 1000;
  dec.decode(bad);
  assert.equal(warned.length, 2);
  assert.match(warned[0]!, /^opus: 1 packet\(s\) refused, last: libopus -4/);
  assert.match(warned[1]!, /^opus: 50 packet\(s\) refused/);
  dec.free();
});

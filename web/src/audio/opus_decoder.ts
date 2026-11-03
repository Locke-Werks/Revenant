// Opus to PCM through the vendored WASM build of libopus
// (web/static/vendor/opus-decoder.min.js, see web/vendor/README.md).
//
// It runs on the main thread. A 20 ms mono packet decodes in well under a
// millisecond, so a worker would buy nothing but a second hop, and the
// library's worker variant spins its worker up from a blob URL the service
// worker cannot cache. The AudioWorklet is the wrong place too: Safari's
// worklet scope has no Worker, which the library's UMD wrapper extends at
// load, and decode time on the render thread is time stolen from output.

import { FRAME } from "./jitter";

// The subset of opus-decoder 0.7.12's API used here.
export interface OpusDecodedAudio {
  channelData: Float32Array[];
  samplesDecoded: number;
  sampleRate: number;
  errors: { message: string }[];
}
export interface OpusDecoderInstance {
  ready: Promise<void>;
  decodeFrame(packet: Uint8Array): OpusDecodedAudio;
  reset(): Promise<void>;
  free(): void;
}
export interface OpusDecoderModule {
  OpusDecoder: new (opts: { channels: number; sampleRate: number; preSkip: number }) => OpusDecoderInstance;
}

export interface PcmDecoder {
  // 960 fresh samples (safe to transfer), or null for a packet libopus
  // refused; the jitter buffer plays a missing packet as silence.
  decode(packet: Uint8Array): Float32Array | null;
  reset(): Promise<void>;
  free(): void;
}

export const VENDOR_URL = "/vendor/opus-decoder.min.js";
const GLOBAL_NAME = "opus-decoder";

export interface DecoderLog {
  warn(msg: string): void;
  now(): number;
}
const defaultLog: DecoderLog = { warn: (m) => console.warn(m), now: () => Date.now() };

// At most one decoder warning per this long, carrying how many were folded.
export const LOG_EVERY_MS = 1000;

export async function createPcmDecoder(mod: OpusDecoderModule, log: DecoderLog = defaultLog): Promise<PcmDecoder> {
  // preSkip 0: a live stream joined mid-way has no Ogg pre-skip to honour, and
  // skipping would make the first packets come out short.
  const dec = new mod.OpusDecoder({ channels: 1, sampleRate: 48_000, preSkip: 0 });
  await dec.ready;
  let lastLog = Number.NEGATIVE_INFINITY;
  let suppressed = 0;
  return {
    decode(packet) {
      // The library console.errors every packet libopus refuses. A corrupt
      // stream would do that fifty times a second, so its messages are
      // caught here and summarised instead.
      const saved = console.error;
      let msg: string | null = null;
      console.error = (...a: unknown[]) => {
        msg = a.map(String).join(" ");
      };
      let r: OpusDecodedAudio;
      try {
        r = dec.decodeFrame(packet);
      } finally {
        console.error = saved;
      }
      const errText = msg ?? r.errors[0]?.message ?? null;
      if (errText !== null) {
        ++suppressed;
        const t = log.now();
        if (t - lastLog >= LOG_EVERY_MS) {
          log.warn(`opus: ${suppressed} packet(s) refused, last: ${errText}`);
          lastLog = t;
          suppressed = 0;
        }
      }
      const pcm = r.channelData[0];
      if (r.errors.length > 0 || r.samplesDecoded !== FRAME || !pcm || pcm.length !== FRAME) return null;
      return pcm;
    },
    reset: () => dec.reset(),
    free: () => dec.free(),
  };
}

// Loads the vendored script once, as a classic script, which is how its UMD
// wrapper puts the module on globalThis.
let loading: Promise<OpusDecoderModule> | null = null;
export function loadOpusModule(doc: Document = document): Promise<OpusDecoderModule> {
  const g = globalThis as unknown as Record<string, OpusDecoderModule | undefined>;
  const have = g[GLOBAL_NAME];
  if (have) return Promise.resolve(have);
  loading ??= new Promise<OpusDecoderModule>((resolve, reject) => {
    const s = doc.createElement("script");
    s.src = VENDOR_URL;
    s.onload = () => {
      const m = g[GLOBAL_NAME];
      if (m) resolve(m);
      else reject(new Error("opus decoder script loaded without defining its module"));
    };
    s.onerror = () => reject(new Error(`could not load ${VENDOR_URL}`));
    doc.head.appendChild(s);
  }).catch((e: unknown) => {
    // Let a later Listen try again, after the network comes back.
    loading = null;
    throw e;
  });
  return loading;
}

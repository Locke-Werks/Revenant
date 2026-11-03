// The webgate wire format, phone side. Mirrors tools/webgate/protocol.h; the
// two change together.
//
// One WebSocket: text frames carry JSON (commands out, replies and events in),
// binary frames carry spectrum rows and Opus audio, little-endian, the first
// byte a Tag.

export const Tag = {
  Spectrum: 1,
  Opus: 2,
} as const;

// tag u8, offset_db f32, scale_db f32, source_start u64, center_hz f64,
// span_hz f64, width_px u32, px[width_px]
export const SPECTRUM_HEADER_SIZE = 1 + 4 + 4 + 8 + 8 + 8 + 4;

// tag u8, vrx u64, seq u32, packet[...]
export const OPUS_HEADER_SIZE = 1 + 8 + 4;

// The gateway refuses a command text frame larger than this.
export const MAX_COMMAND_BYTES = 64 * 1024;

export const OPS = [
  "addVrx",
  "removeVrx",
  "tune",
  "setMode",
  "setBandwidth",
  "setSquelch",
  "setCenter",
  "setGain",
  "setGainAuto",
  "listSources",
  "openSource",
  "setView",
  "audio",
  "keepListening",
  "setTranscription",
  "setVrxTranscribe",
] as const;
export type Op = (typeof OPS)[number];

export const EVENTS = [
  "vrxStatus",
  "decoded",
  "transcript",
  "transcriptionStatus",
  "source",
  "link",
  "vrxRemoved",
  "hls",
] as const;
export type EventName = (typeof EVENTS)[number];

// ---- Binary frames ----------------------------------------------------------

export interface SpectrumFrame {
  offsetDb: number;
  scaleDb: number;
  // u64 on the wire. A bigint, because a sample counter passes 2^53.
  sourceStart: bigint;
  centerHz: number;
  spanHz: number;
  // db = offsetDb + px * scaleDb, so px 0 is the floor.
  px: Uint8Array;
}

export interface OpusFrame {
  vrx: bigint;
  seq: number;
  packet: Uint8Array;
}

export type BinaryFrame =
  | ({ tag: typeof Tag.Spectrum } & SpectrumFrame)
  | ({ tag: typeof Tag.Opus } & OpusFrame);

// Returns null for a frame that is too short, carries an unknown tag, or whose
// declared width does not match the bytes present. The returned arrays are
// views onto the input buffer, not copies.
export function decodeBinary(buf: ArrayBuffer): BinaryFrame | null {
  if (buf.byteLength < 1) return null;
  const dv = new DataView(buf);
  const tag = dv.getUint8(0);

  if (tag === Tag.Spectrum) {
    if (buf.byteLength < SPECTRUM_HEADER_SIZE) return null;
    const width = dv.getUint32(33, true);
    if (buf.byteLength !== SPECTRUM_HEADER_SIZE + width) return null;
    return {
      tag: Tag.Spectrum,
      offsetDb: dv.getFloat32(1, true),
      scaleDb: dv.getFloat32(5, true),
      sourceStart: dv.getBigUint64(9, true),
      centerHz: dv.getFloat64(17, true),
      spanHz: dv.getFloat64(25, true),
      px: new Uint8Array(buf, SPECTRUM_HEADER_SIZE, width),
    };
  }

  if (tag === Tag.Opus) {
    if (buf.byteLength < OPUS_HEADER_SIZE) return null;
    return {
      tag: Tag.Opus,
      vrx: dv.getBigUint64(1, true),
      seq: dv.getUint32(9, true),
      packet: new Uint8Array(buf, OPUS_HEADER_SIZE),
    };
  }

  return null;
}

// Encoders exist so the web tests can build frames the gateway would send.
export function encodeSpectrum(f: SpectrumFrame): ArrayBuffer {
  const buf = new ArrayBuffer(SPECTRUM_HEADER_SIZE + f.px.length);
  const dv = new DataView(buf);
  dv.setUint8(0, Tag.Spectrum);
  dv.setFloat32(1, f.offsetDb, true);
  dv.setFloat32(5, f.scaleDb, true);
  dv.setBigUint64(9, f.sourceStart, true);
  dv.setFloat64(17, f.centerHz, true);
  dv.setFloat64(25, f.spanHz, true);
  dv.setUint32(33, f.px.length, true);
  new Uint8Array(buf, SPECTRUM_HEADER_SIZE).set(f.px);
  return buf;
}

export function encodeOpus(f: OpusFrame): ArrayBuffer {
  const buf = new ArrayBuffer(OPUS_HEADER_SIZE + f.packet.length);
  const dv = new DataView(buf);
  dv.setUint8(0, Tag.Opus);
  dv.setBigUint64(1, f.vrx, true);
  dv.setUint32(9, f.seq, true);
  new Uint8Array(buf, OPUS_HEADER_SIZE).set(f.packet);
  return buf;
}

// ---- JSON messages ----------------------------------------------------------

export type JsonValue =
  | null
  | boolean
  | number
  | string
  | JsonValue[]
  | { [key: string]: JsonValue };

// Phone to gateway. id is a non-negative integer no larger than 2^53 and is
// echoed in the reply. args is optional and must be an object when present.
export interface Command {
  id: number;
  op: Op;
  args?: { [key: string]: JsonValue };
}

export interface OkReply {
  id: number;
  ok: true;
  result: JsonValue;
}

// error is the engine's text, unchanged.
export interface ErrorReply {
  id: number;
  ok: false;
  error: string;
}

export type Reply = OkReply | ErrorReply;

export interface GatewayEvent {
  event: EventName;
  [field: string]: JsonValue;
}

export type ServerMessage = Reply | GatewayEvent;

export function encodeCommand(cmd: Command): string {
  const text = JSON.stringify(cmd);
  if (text.length > MAX_COMMAND_BYTES) {
    // Length in UTF-16 units understates bytes; the gateway checks bytes. This
    // catches the obvious case before the socket does.
    throw new RangeError(`command is ${text.length} characters, limit ${MAX_COMMAND_BYTES}`);
  }
  return text;
}

export function isReply(m: ServerMessage): m is Reply {
  return typeof (m as Reply).id === "number" && typeof (m as Reply).ok === "boolean";
}

export function isEvent(m: ServerMessage): m is GatewayEvent {
  return typeof (m as GatewayEvent).event === "string";
}

// Returns null for text that is not a reply or an event.
export function parseServerMessage(text: string): ServerMessage | null {
  let v: unknown;
  try {
    v = JSON.parse(text);
  } catch {
    return null;
  }
  if (typeof v !== "object" || v === null || Array.isArray(v)) return null;
  const m = v as ServerMessage;
  return isReply(m) || isEvent(m) ? m : null;
}

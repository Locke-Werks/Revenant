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
// span_hz f64, width_px u32, px[width_px], then the trace: trace_offset_db
// f32, trace_scale_db f32, trace_px[width_px]. px is the waterfall row in the
// desktop waterfall's window, trace_px the spectrum trace in the desktop
// spectrum's; a frame that stops after px draws px as its trace.
export const SPECTRUM_HEADER_SIZE = 1 + 4 + 4 + 8 + 8 + 8 + 4;
export const SPECTRUM_TRACE_HEADER_SIZE = 4 + 4;

// tag u8, vrx u64, seq u32, packet[...]
export const OPUS_HEADER_SIZE = 1 + 8 + 4;

// The gateway refuses a command text frame larger than this.
export const MAX_COMMAND_BYTES = 64 * 1024;

// The gateway sends a "ping" event this often (kPingIntervalMs).
export const PING_INTERVAL_MS = 15_000;

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
  "setDetection",
  "resume",
  "pong",
  "listDecoders",
  "setDecoders",
  "takeOver",
  "autoDv",
  "trunkFollow",
  "trunkStatus",
  "setTrunkRules",
  "dvTab",
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
  "hello",
  "ping",
  "vrxEventsEnded",
  "detections",
  "display",
  "desktop",
  "rds",
  "lease",
  "vrx",
  "vrxList",
  "engine",
  "calls",
  "talkgroups",
  "trunk",
  "dv",
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
  // db = offsetDb + px * scaleDb, so px 0 is the floor. The waterfall row.
  px: Uint8Array;
  // The trace row, the same width, in its own window.
  traceOffsetDb: number;
  traceScaleDb: number;
  tracePx: Uint8Array;
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
    const plain = SPECTRUM_HEADER_SIZE + width;
    const traced = plain + SPECTRUM_TRACE_HEADER_SIZE + width;
    if (buf.byteLength !== plain && buf.byteLength !== traced) return null;
    const offsetDb = dv.getFloat32(1, true);
    const scaleDb = dv.getFloat32(5, true);
    const px = new Uint8Array(buf, SPECTRUM_HEADER_SIZE, width);
    const hasTrace = buf.byteLength === traced;
    return {
      tag: Tag.Spectrum,
      offsetDb,
      scaleDb,
      sourceStart: dv.getBigUint64(9, true),
      centerHz: dv.getFloat64(17, true),
      spanHz: dv.getFloat64(25, true),
      px,
      traceOffsetDb: hasTrace ? dv.getFloat32(plain, true) : offsetDb,
      traceScaleDb: hasTrace ? dv.getFloat32(plain + 4, true) : scaleDb,
      tracePx: hasTrace ? new Uint8Array(buf, plain + SPECTRUM_TRACE_HEADER_SIZE, width) : px,
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
// The trace is optional here so a test can build an older gateway's frame.
export type SpectrumFrameInput = Omit<SpectrumFrame, "traceOffsetDb" | "traceScaleDb" | "tracePx"> &
  Partial<Pick<SpectrumFrame, "traceOffsetDb" | "traceScaleDb" | "tracePx">>;

export function encodeSpectrum(f: SpectrumFrameInput): ArrayBuffer {
  const trace = f.tracePx && f.tracePx.length === f.px.length ? f.tracePx : null;
  const plain = SPECTRUM_HEADER_SIZE + f.px.length;
  const buf = new ArrayBuffer(plain + (trace ? SPECTRUM_TRACE_HEADER_SIZE + trace.length : 0));
  const dv = new DataView(buf);
  if (trace) {
    dv.setFloat32(plain, f.traceOffsetDb ?? f.offsetDb, true);
    dv.setFloat32(plain + 4, f.traceScaleDb ?? f.scaleDb, true);
    new Uint8Array(buf, plain + SPECTRUM_TRACE_HEADER_SIZE).set(trace);
  }
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

// "link". After an engine restart, the Up event of a session the gateway
// rebuilt carries recreated and lost: each receiver the phone opened was added
// again at the same frequency and mode under a new id, or could not be, with
// the engine's reason (and a vrxRemoved for that id as well). Audio, keep
// listening and the rest follow the new ids; the app swaps old for new.
export interface LinkEvent {
  event: "link";
  state: "starting" | "waiting" | "up" | "refused";
  detail: string;
  recreated?: { old: number; new: number }[];
  lost?: { vrx: number; reason: string }[];
}

// "lease": the engine serves one user at a time (tools/webgate/phone_hub.h).
// "held" is the first event on a connection whose phone holds the engine;
// "inUse" means another client holds it, since `since`, and every command but
// takeOver is refused; "lost" means `holder` took it over from this phone,
// whose session is gone. takeOver from inUse or lost takes the engine, and the
// connection then gets a fresh session: lease held, hello, link.
export interface LeaseEvent {
  event: "lease";
  state: "held" | "inUse" | "lost";
  holder: string;
  since?: string;
  message?: string;
}

// "vrx": one receiver the engine has, whoever opened it, shaped like the
// addVrx result plus openedBy. Sent for every receiver when a session starts
// or resumes and whenever one is added or changed.

// "transcriptionStatus" carries the fields of the setTranscription result
// (enabled, modelState, modelName, bytesDone, bytesTotal, detail, ...). The
// gateway sends it on connect, on resume, and whenever the switch, the model
// state, its name or the detail change or a download moves a whole percent
// (transcription_report_due in tools/webgate/dispatch.h).

// "source". While open it carries the band and the engine's pacing:
// realtimeFactor is capture seconds per wall second over the engine's recent
// window (0 means not measured yet, not stalled) and pacedBy the pace in force
// (0 unthrottled). The gateway sends it on a stream, centre or pace change and when
// the factor moves by 0.01 or more (kRealtimeReportStep in protocol.h).
export interface SourceEvent {
  event: "source";
  open: boolean;
  rateHz?: number;
  centerHz?: number;
  epoch?: number;
  realtimeFactor?: number;
  pacedBy?: number;
  // Where the engine's spectrum bins really lie, which is not the band: it
  // starts half a coarse channel low and stops that much short of the top.
  // Absent from older gateways and when the spectrum is off.
  dataLowHz?: number;
  dataHighHz?: number;
}

// "detections" is {items: [...]}, each item shaped like Detection in
// detections.ts. The setDetection reply is {minConfidence}.
export interface DetectionEvent {
  event: "detections";
  items: JsonValue[];
}

// "display" is the desktop's colours, sent on connect, on resume and when the
// desktop's settings change them (tools/webgate/display_sync.h). waterfall
// and spectrum are 256 colours of 6 hex digits, rrggbb, indexed by a row's
// byte: the waterfall's through the desktop's contrast curve, and the fill
// under the trace. trace and background are "#rrggbb".
export interface DisplayEvent {
  event: "display";
  waterfall: string;
  spectrum: string;
  trace: string;
  background: string;
}

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

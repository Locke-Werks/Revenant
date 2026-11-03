// The engine's decoders as the phone offers them, the digital voice modes,
// and what a digital voice receiver's call state is.

import type { GatewayEvent, JsonValue } from "./protocol";

export interface DecoderInfo {
  name: string;
  description: string;
  input: string;
  // Receiver modes it reads. Empty from an engine too old to say, which is
  // "unknown", so such a decoder is offered everywhere and the gateway judges.
  modes: string[];
}

export interface DecoderCatalogue {
  decoders: DecoderInfo[];
  // Digital voice modes whose voice will play; null when the engine does not
  // report its vocoder plugins.
  voiceModes: string[] | null;
}

// Each has a decoder of the same name, which the gateway attaches by default.
export const DV_MODES = ["p25p1", "dstar", "tetra", "dmr"] as const;

const MODE_LABELS: Record<string, string> = { p25p1: "P25", dstar: "D-STAR", tetra: "TETRA", dmr: "DMR" };

export function modeLabel(mode: string): string {
  return MODE_LABELS[mode] ?? mode.toUpperCase();
}

export function isDigitalVoice(mode: string): boolean {
  return (DV_MODES as readonly string[]).includes(mode);
}

const strings = (v: unknown): string[] => (Array.isArray(v) ? v.filter((s): s is string => typeof s === "string") : []);

export function parseCatalogue(reply: JsonValue): DecoderCatalogue {
  const o = reply !== null && typeof reply === "object" && !Array.isArray(reply) ? reply : {};
  const decoders: DecoderInfo[] = [];
  for (const d of Array.isArray(o.decoders) ? o.decoders : []) {
    if (d === null || typeof d !== "object" || Array.isArray(d) || typeof d.name !== "string" || d.name === "") continue;
    decoders.push({
      name: d.name,
      description: typeof d.description === "string" ? d.description : "",
      input: typeof d.input === "string" ? d.input : "",
      modes: strings(d.modes),
    });
  }
  return { decoders, voiceModes: Array.isArray(o.voiceModes) ? strings(o.voiceModes) : null };
}

export function decodersFor(mode: string, all: readonly DecoderInfo[]): DecoderInfo[] {
  return all.filter((d) => d.modes.length === 0 || d.modes.includes(mode)).sort((a, b) => a.name.localeCompare(b.name));
}

// The set after one toggle, in the catalogue's order so the reply compares
// cleanly with what is shown.
export function toggled(current: readonly string[], name: string): string[] {
  return current.includes(name) ? current.filter((n) => n !== name) : [...current, name];
}

// A decoded message says a call is encrypted with P25's "encrypted" flag or
// DMR's "privacy"; either false says the call is in the clear. Undefined when
// the message says neither.
export function encryptedFlag(e: GatewayEvent): boolean | undefined {
  const f = (e as { fields?: unknown }).fields;
  if (f === null || typeof f !== "object") return undefined;
  const o = f as Record<string, unknown>;
  if (o.encrypted === true || o.privacy === true) return true;
  if (o.encrypted === false || o.privacy === false) return false;
  return undefined;
}

// The status shown on a digital voice receiver's chip, or null.
export function voiceStatus(mode: string, voiceModes: readonly string[] | null, encrypted: boolean): string | null {
  if (!isDigitalVoice(mode)) return null;
  if (encrypted) return "encrypted";
  if (voiceModes !== null && !voiceModes.includes(mode)) return "no vocoder";
  return null;
}

// The engine's refusals are full sentences, sometimes prefixed with the
// argument they are about. A toast wants the first clause of the sentence.
export function shortRefusal(text: string, max = 120): string {
  let t = text.replace(/^names:\s*/, "").trim();
  const stop = t.search(/[.;](\s|$)/);
  if (stop > 0) t = t.slice(0, stop);
  return t.length > max ? `${t.slice(0, max - 1)}…` : t;
}

// What the desktop has saved, as the gateway's "desktop" event carries it
// (tools/webgate/desktop_settings.h): the radio the desktop last opened, front
// end parameters included, and its detection confidence bar.
//
// The phone has no radio settings of its own. Opening a radio on the device
// the desktop last used composes the URI from the desktop's parameters, the
// way the desktop restores them at launch (ui/models/radio_memory.h
// restore_radio_uri, ui/models/source_choice.h with_query_value); the
// centre is the one exception, because the phone follows where it is tuned.

import type { GatewayEvent } from "./protocol";
import type { TuneRange } from "./band_edge";

export interface DesktopRadio {
  uri: string;
  backend: string;
  // The text between "://" and the query: an index ("0") or a serial.
  device: string;
  // In the order the desktop wrote them, which a composed URI keeps.
  params: Array<[string, string]>;
}

export interface DesktopState {
  radio: DesktopRadio | null;
  confidenceBar: number | null;
}

export const NO_DESKTOP: DesktopState = { radio: null, confidenceBar: null };

function str(v: unknown): string | null {
  return typeof v === "string" ? v : null;
}

// Anything malformed reads as nothing saved, so the phone falls back to its
// own defaults rather than a half-parsed radio.
export function parseDesktop(e: GatewayEvent): DesktopState {
  const bar = (e as { confidenceBar?: unknown }).confidenceBar;
  const confidenceBar = typeof bar === "number" && Number.isFinite(bar) ? bar : null;
  const r = (e as { radio?: unknown }).radio;
  let radio: DesktopRadio | null = null;
  if (r && typeof r === "object") {
    const o = r as Record<string, unknown>;
    const uri = str(o.uri);
    const backend = str(o.backend);
    const device = str(o.device);
    const raw = o.params;
    if (uri !== null && backend !== null && device !== null && raw && typeof raw === "object") {
      const params: Array<[string, string]> = [];
      for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
        if (typeof v === "string") params.push([k, v]);
      }
      radio = { uri, backend, device, params };
    }
  }
  return { radio, confidenceBar };
}

// `uri` with `key` set to `value`, or removed when there is no value. Every
// other key keeps its place and its spelling: ui/models/source_choice.h
// with_query_value.
export function withQueryValue(uri: string, key: string, value: string | null): string {
  const mark = uri.indexOf("?");
  let out = mark < 0 ? uri : uri.slice(0, mark);
  let hasQuery = false;
  const append = (part: string) => {
    out += (hasQuery ? "&" : "?") + part;
    hasQuery = true;
  };
  if (mark >= 0) {
    for (const part of uri.slice(mark + 1).split("&")) {
      if (part === "" || part.split("=")[0] === key) continue;
      append(part);
    }
  }
  if (value !== null) append(`${key}=${value}`);
  return out;
}

function deviceOf(uri: string): string {
  const at = uri.indexOf("://");
  const rest = at < 0 ? uri : uri.slice(at + 3);
  const mark = rest.indexOf("?");
  return mark < 0 ? rest : rest.slice(0, mark);
}

export interface ListedRadio {
  uri: string;
  backend: string;
  serial?: string;
  tuneRanges: TuneRange[];
}

// Whether the desktop's last radio is this listed one: the same backend and
// the same device, by index as the URI names it or by serial.
export function sameDevice(s: ListedRadio, d: DesktopRadio): boolean {
  if (s.backend !== d.backend) return false;
  if (deviceOf(s.uri) === d.device) return true;
  return typeof s.serial === "string" && s.serial !== "" && s.serial === d.device;
}

export function reaches(ranges: TuneRange[], hz: number): boolean {
  return ranges.some((r) => r.lowHz <= hz && hz <= r.highHz);
}

// The URI to open a listed radio with, or null when the desktop's last radio
// was another device and the caller composes its own.
//
// Every parameter the desktop carries is kept: rate, bw, gain, agc, direct,
// ppm, bias, whatever it holds. freq alone is the phone's: its last centre
// when this tuner reaches it, else the desktop's, else (a desktop URI with no
// freq on a tunable radio) the caller's default through `fallbackHz`.
export function desktopOpenUri(
  s: ListedRadio,
  desktop: DesktopRadio | null,
  centreHz: number | null,
  fallbackHz: (ranges: TuneRange[], centreHz: number | null) => number,
): string | null {
  if (desktop === null || !sameDevice(s, desktop)) return null;
  const base = s.uri.includes("?") ? s.uri.slice(0, s.uri.indexOf("?")) : s.uri;
  let uri = base;
  const ranges = s.tuneRanges ?? [];
  const tunable = ranges.length > 0;
  const own = desktop.params.find(([k]) => k === "freq")?.[1];
  let freq: string | null = null;
  if (tunable) {
    if (centreHz !== null && reaches(ranges, centreHz)) freq = String(Math.round(centreHz));
    else if (own !== undefined) freq = own;
    else freq = String(Math.round(fallbackHz(ranges, centreHz)));
  }
  let wroteFreq = false;
  for (const [k, v] of desktop.params) {
    if (k === "freq") {
      // A radio with no tune range never takes freq=: the backend refuses it
      // by name.
      if (freq !== null) {
        uri = withQueryValue(uri, "freq", freq);
        wroteFreq = true;
      }
    } else {
      uri = withQueryValue(uri, k, v);
    }
  }
  if (freq !== null && !wroteFreq) uri = withQueryValue(uri, "freq", freq);
  return uri;
}

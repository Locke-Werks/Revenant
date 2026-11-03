// What each WFM receiver's RDS says, from the gateway's rds events. The
// gateway sends PS and RadioText only once every segment has arrived, and
// sends an event only when something changed.

import type { GatewayEvent } from "./protocol";

export interface RdsStation {
  pi: number | null;
  callSign: string | null;
  ps: string | null;
  rt: string | null;
  pty: string | null;
}

const text = (v: unknown): string | null => (typeof v === "string" && v.trim() !== "" ? v.trim() : null);

export function parseRds(e: GatewayEvent): { vrx: number; station: RdsStation } | null {
  if (typeof e.vrx !== "number" || !Number.isFinite(e.vrx)) return null;
  return {
    vrx: e.vrx,
    station: {
      pi: typeof e.pi === "number" ? e.pi : null,
      callSign: text(e.callSign),
      ps: text(e.ps),
      rt: text(e.rt),
      pty: text(e.pty),
    },
  };
}

export class RdsModel {
  private stations = new Map<number, RdsStation>();
  // The last RadioText put in the feed per receiver. A station repeats its
  // message for minutes, and toggles between two, so only a new text counts.
  private lastRt = new Map<number, string>();

  // The station after this event, and the RadioText to add to the feed, if
  // there is a new one.
  apply(e: GatewayEvent): { vrx: number; newRt: string | null } | null {
    const parsed = parseRds(e);
    if (!parsed) return null;
    this.stations.set(parsed.vrx, parsed.station);
    const rt = parsed.station.rt;
    if (rt === null || this.lastRt.get(parsed.vrx) === rt) return { vrx: parsed.vrx, newRt: null };
    this.lastRt.set(parsed.vrx, rt);
    return { vrx: parsed.vrx, newRt: rt };
  }

  get(vrx: number): RdsStation | null {
    return this.stations.get(vrx) ?? null;
  }

  // The station's name as a chip or readout shows it: PS, or the call sign
  // an RBDS station's PI derives when no PS has arrived.
  name(vrx: number): string | null {
    const s = this.stations.get(vrx);
    return s ? (s.ps ?? s.callSign) : null;
  }

  // A receiver that went, or a restart's new ids: the next events say what
  // the new receivers carry, so nothing is carried across.
  drop(vrx: number): void {
    this.stations.delete(vrx);
    this.lastRt.delete(vrx);
  }

  clear(): void {
    this.stations.clear();
    this.lastRt.clear();
  }
}

// Canvas drawing for receiver passbands, shared by the spectrum trace and the
// overlay above the waterfall. Receivers are drawn on the current view every
// frame, never into history, so a drag moves them with the finger.

import { passbandPx } from "./markers";
import type { View } from "./view";

export interface ReceiverMark {
  vrx: number;
  hz: number;
  mode: string;
  bandwidthHz: number | null;
}

// The active receiver is the one the controls act on; the others only need
// to be findable.
const ACTIVE_FILL = 0.22;
const OTHER_FILL = 0.1;
const OTHER_EDGE = 0.35;

export function drawReceivers(
  ctx: CanvasRenderingContext2D,
  view: View,
  list: readonly ReceiverMark[],
  active: number | null,
  accent: string,
): void {
  const w = ctx.canvas.width;
  const h = ctx.canvas.height;
  const line = Math.max(1, Math.round(globalThis.devicePixelRatio || 1));
  ctx.fillStyle = accent;
  // Inactive first so the active one sits on top where they overlap.
  for (const pass of [false, true]) {
    for (const r of list) {
      if ((r.vrx === active) !== pass) continue;
      const p = passbandPx(view, w, r);
      if (!p) continue;
      ctx.globalAlpha = pass ? ACTIVE_FILL : OTHER_FILL;
      ctx.fillRect(p.x0, 0, p.x1 - p.x0, h);
      if (pass) {
        ctx.globalAlpha = 1;
        ctx.fillRect(Math.round(p.xc - line / 2), 0, line, h);
      } else {
        ctx.globalAlpha = OTHER_EDGE;
        ctx.fillRect(p.x0, 0, line, h);
        ctx.fillRect(p.x1 - line, 0, line, h);
      }
    }
  }
  ctx.globalAlpha = 1;
}

# Phone tuning, detections and visual pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drag and dial tuning, detection markers plus a Signals list, and the desktop's visual rules on the phone app.

**Architecture:** The gateway polls `Session.detections` per attached phone and pushes a `detections` event; everything else is the web app. Pure logic (gesture classification, rate limiting, dial physics, detection list, auto-scale) lives in testable modules with no DOM; DOM wiring is thin.

**Tech Stack:** C++ (tools/webgate, Catch2), TypeScript (web/, esbuild, node:test).

**Spec:** `docs/superpowers/specs/2026-11-03-webgate-tuning-detections-design.md`

## Global Constraints

- Build only with `scripts/build.ps1` (`-Target revenant_web` for web-only). Never bare cmake.
- No engine schema change.
- Web tests: `cd web && npm test` (tsc --noEmit plus node:test). Gateway tests: `ctest --preset dev -R webgate`.
- Default detection minConfidence 0.0, minMargin 0.0 (the desktop's defaults).
- Detection poll 2 Hz; send only live and held tracks overlapping the view plus one view-width either side, capped at 200 by SNR.
- `tune` from drag or dial at most every 50 ms, plus one final send on release.
- Fine steps: CW, USB, LSB, DSB 10 Hz; AM, NFM, raw 100 Hz; WFM 1 kHz. Fast multiplier 100. Dial settles within 600 ms.
- View follows a dragged receiver only inside the outer 10% of the view.
- Auto-scale time constant about 30 s, both floor and ceiling.
- Motion under 200 ms except dial momentum. No em dashes in any text. Commits as Locke Werks, no AI trailers.
- File content via Write/Edit tools, never heredocs.

## Review Focus

1. A drag that starts on a receiver chip or the sheet must not tune: gestures attach to the waterfall element only.
2. Detections arriving after the phone retuned the centre: boxes already drawn stay on their rows; new ones use the current view.
3. An engine with no detector yet, or one that restarts: empty list, not an error toast; polling resumes after link up.
4. A dial flick still coasting when the user taps a Signals row: the tap wins and momentum stops.
5. A track id above 2^53 must survive as a string end to end.

---

### Task 1: Gateway detections event and setDetection

**Files:** Modify `tools/webgate/phone_streams.{h,cpp}`, `tools/webgate/dispatch.cpp`, `tools/webgate/protocol.{h,cpp}`. Test `tests/webgate/test_phone_streams.cpp`, `tests/webgate/test_dispatch.cpp`.

**Interfaces:** Produces event `{"event":"detections","items":[{id:string,centerHz,bandwidthHz,snrDb,confidence,state:"live"|"held",label:string,labelConfidence,lastSeen}]}` and op `setDetection {minConfidence:number}` replying `{minConfidence}`. `StreamsConfig` gains `std::function<Expected<rpc::DetectionList>(double,double)> detections` (empty reads the engine) and `std::chrono::milliseconds detection_poll{500}`.

- [ ] Tests: event shape and field names; merged tracks dropped; tracks outside view plus one width either side dropped; 250 tracks in view yields the 200 highest SNR; id 2^60 serialised as decimal string; `setDetection` refuses NaN, negative and >= 1 with a reason; minConfidence survives resume; no poll while no view is set; a failed poll sends nothing and does not toast.
- [ ] Run, see them fail.
- [ ] Implement: poll from `tick()` on its own interval; label is the label's name when its kind is a modulation, else empty.
- [ ] Run gateway tests, pass. Commit "Push detections to the phone and accept setDetection".

### Task 2: Web protocol and detection model

**Files:** Modify `web/src/protocol.ts` (add `detections` to EVENTS, `setDetection` to OPS, `DetectionEvent` type). Create `web/src/detections.ts`. Test `web/test/detections.test.ts`.

**Interfaces:** `parseDetections(e: GatewayEvent): Detection[]`; `sortForList(d: Detection[]): Detection[]` (SNR descending, then id); `snapTap(hz: number, d: Detection[]): number` (centre of the narrowest detection whose band holds hz, else hz); `fitBandwidth(widthHz: number, choices: number[]): number` (smallest choice >= width, else the largest).

- [ ] Tests for each function including malformed items skipped and overlapping bands.
- [ ] Implement, pass, commit "Model detections on the phone".

### Task 3: Gesture rework and tune rate limiter

**Files:** Modify `web/src/gestures.ts`, `web/src/app.ts`. Create `web/src/tune_limiter.ts`. Test `web/test/gestures.test.ts`, `web/test/tune_limiter.test.ts`.

**Interfaces:** `GestureSink` gains `dragTune(dHz: number)` and `dragEnd()`; `pan` now only from two fingers. `class TuneLimiter { constructor(send:(hz:number)=>void, now:()=>number, schedule:(fn,ms)=>cancel); set(hz:number):void; flush():void; cancel():void }`.

- [ ] Tests: one-finger move past TAP_SLOP_PX emits dragTune not pan; two fingers emit pan and zoom; tap and long press unchanged; limiter sends first value at once, at most one per 50 ms, the last value on flush, nothing after cancel.
- [ ] Implement. In app.ts: drag moves the active receiver marker immediately, sends via the limiter, flushes on dragEnd; view follows when the receiver enters the outer 10%; tap uses `snapTap`.
- [ ] Pass, commit "Tune by dragging on the waterfall".

### Task 4: Dial strip

**Files:** Create `web/src/dial.ts` (pure physics) and `web/src/ui/dial_strip.ts` (DOM). Test `web/test/dial.test.ts`.

**Interfaces:** `fineStep(mode: string): number`; `stepFor(mode, pxPerMs): number` (fine below a speed threshold, fine*100 above); `class DialPhysics { drag(dxPx, tMs); release(tMs); frame(tMs): number /* Hz delta */; stop(); get moving(): boolean }` with settle within 600 ms; `roundToStep(hz, step)`.

- [ ] Tests: step table per mode; slow versus fast; momentum decays to zero within 600 ms; settled value rounded to the fine step; stop() halts immediately; a tap (no movement) produces no delta.
- [ ] Implement physics; DialStrip draws scrolling ticks under a fixed centre line with labels, feeds the TuneLimiter, and is stopped by any other tune source.
- [ ] Pass, commit "Add the dial strip".

### Task 5: Per-digit frequency readout

**Files:** Modify `web/src/ui/top_bar.ts`. Test `web/test/receivers.test.ts` (or a new `top_bar.test.ts`).

**Interfaces:** `digitStep(hz: number, digitIndexFromRight: number, dir: 1|-1): number` clamped at zero.

- [ ] Tests: carry and borrow across digits, clamp at zero.
- [ ] Implement: monospace per-digit spans; vertical swipe of 16 px per step on a digit; tap still opens typed entry.
- [ ] Pass, commit "Step the frequency readout by digit".

### Task 6: Detection markers on spectrum and waterfall

**Files:** Modify `web/src/spectrum_trace.ts`, `web/src/waterfall.ts`, `web/src/app.ts`. Test `web/test/waterfall.test.ts`.

**Interfaces:** `RowRing` stores per-row detection boxes `{lowHz, highHz, alpha, held}` stamped at arrival; renderers draw them through the existing `rowTransform` so they shift with the rows. Spectrum trace draws translucent bands with labels when wider than the label.

- [ ] Tests: boxes stamped on the row current at arrival; a later retune moves them with their rows; boxes leave with their rows when the ring wraps.
- [ ] Implement for both GL and canvas renderers, accent colour, alpha by confidence, held dimmer.
- [ ] Pass, commit "Draw detections on the spectrum and waterfall".

### Task 7: Sheet tabs and the Signals list

**Files:** Modify `web/src/ui/sheet.ts`, `web/src/ui/radio_panel.ts` (hosted in a tab), create `web/src/ui/signals.ts`. Test `web/test/receivers.test.ts`.

**Interfaces:** Sheet tabs Tune, Signals, Radio. `SignalsPanel.update(d: Detection[])`; row tap calls `onPick(det)` which tunes, sets bandwidth via `fitBandwidth` with the Tune tab's choices, stops dial momentum, and pans the view if off screen. Confidence slider sends `setDetection`.

- [ ] Tests: row order; "time since heard" formatting; pick calls tune and bandwidth with fitted width; slider debounced to one send per 250 ms.
- [ ] Implement; sections not applying to the active receiver hidden.
- [ ] Pass, commit "Add the Signals tab".

### Task 8: Visual pass

**Files:** Modify `web/static/app.css`, `web/static/index.html`, `web/src/colormap.ts`, `web/src/app.ts`, `web/src/ui/status.ts`. Create `web/src/autoscale.ts`, `web/src/ui/ruler.ts`. Test `web/test/autoscale.test.ts`.

**Interfaces:** `class AutoScale { constructor(tauMs=30000); update(minDb, maxDb, tMs): {floor, ceiling} }`.

- [ ] Safe-area defect (owner, 2026-11-03): installed app shows a gap at the bottom while content sits under the notch at the top. Add a `?insets` debug overlay printing `env(safe-area-inset-*)`, `innerHeight`, `visualViewport.height` and `offsetTop`; owner reads it on the phone; fix the cause (inset reporting or a standalone-mode viewport offset) rather than hard-coding padding; checklist item added in Task 9.
- [ ] Tests: converges to a step change within about 3 tau, both extents move, never inverts.
- [ ] Implement: near-black, one accent; full-bleed display; ruler between trace and waterfall; problems as chips; system sans plus monospace digits; transitions under 200 ms; colour map lightness monotonic.
- [ ] Pass, rebuild with `-Target revenant_web`, screenshot at 390x844 in Chrome, commit "Restyle the phone app to the desktop rules".

### Task 9: Docs and budget

**Files:** Modify `docs/webgate.md`.

- [ ] Add checklist items: drag tuning, dial, digit swipe, detections on a live band, Signals tap, confidence slider. Document the new event and op.
- [ ] Run `scripts/frame-budget.ps1 -Phone`; numbers within noise of the previous run.
- [ ] Commit "Document phone tuning and detections".

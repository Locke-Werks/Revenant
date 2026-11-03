# Phone app: tuning, detections and visual pass

Date: 2026-11-03. Status: design agreed in conversation, awaiting spec review.

Follows `2026-11-02-webgate-design.md`, which shipped the first phone release
and left the detections overlay out of scope. After the first session on the
phone with a real dongle the owner asked for three things: a dial or drag to
tune, detections, and a layout that is not ugly.

## Decisions (owner, 2026-11-03)

- Tuning is both: one finger on the waterfall drags the active receiver, and a
  dial strip gives fine tuning (option C).
- Detections are markers on the spectrum and waterfall plus a Signals list in
  the sheet; tapping either tunes the active receiver there (option B). No
  automatic scanning.
- The phone follows the desktop interface rules agreed on 2026-09-22 (the
  architecture handoff's "Interface principles", "Layout" and "Visual
  identity").
- The SDRuno-style narrow fine-tuning display is out of this round: on the
  phone it needs a per-receiver transform streamed from the engine.

## Gestures on the waterfall

| Gesture | Does |
| --- | --- |
| One finger drag | Moves the active receiver by the dragged frequency. The view does not move unless the receiver reaches the outer 10% of the view, where the view follows. |
| Two finger drag | Pans the view. |
| Pinch | Zooms about the fingers' midpoint, as now. |
| Tap | Tunes the active receiver to the tap, snapped to a detection's centre when the tap lands inside its band. |
| Long press | Adds a receiver there, as now. |

A drag sends `tune` at most every 50 ms and once more on release with the
final frequency, so the engine sees a bounded rate and ends on the exact
value. The receiver marker moves with the finger immediately; the passband
drawn is the one the phone asked for until the engine's status confirms it.

## Dial strip

A horizontal tick ribbon above the home indicator, the width of the screen.

- Dragging it retunes the active receiver; flicking carries momentum with
  friction and settles within 600 ms.
- Step size follows finger speed: slow drags move in the mode's fine step
  (CW and SSB 10 Hz, AM and NFM 100 Hz, WFM 1 kHz), fast drags in 100 times
  that. The settled frequency rounds to the fine step.
- The ribbon's labels show the frequency under the centre line; the centre
  line is fixed, the ticks scroll.
- Taps on the strip do nothing, so a thumb resting there cannot retune.
- Uses the same `tune` rate limit as the waterfall drag.

## Frequency readout

The top bar's readout becomes the per-digit monospace display the desktop
uses. Tap still opens typed entry. Vertical swipe on a digit steps that digit
up or down, which is the phone's form of wheel-on-a-digit.

## Detections

### Gateway

webgate polls `Session.detections(minConfidence, minMargin)` for each
attached session at 2 Hz while the phone's view is open, and sends one
`detections` event per poll:

```
{"event":"detections","items":[{"id":"…","centerHz":…,"bandwidthHz":…,
  "snrDb":…,"confidence":…,"state":"live|held","label":"NFM",
  "labelConfidence":…,"lastSeen":…}]}
```

- Only `live` and `held` tracks are sent; merged ones are dropped.
- `id` is the engine's track id as a decimal string, since it is a UInt64.
- `label` is the label's name when its kind is a modulation, empty otherwise.
- Only tracks overlapping the phone's current view plus one view-width either
  side are sent, capped at 200 by SNR, so a busy band cannot flood the socket.
- New control message `setDetection {minConfidence}`; default is the
  desktop's default threshold. Stored per session and re-applied on resume.
- No engine schema change. The detector starts on the first request, as it
  does for the desktop.

### Phone

- Each detection is a translucent band over the spectrum trace, in the
  accent colour, alpha by confidence, with its label above it when there is
  room.
- On the waterfall a detection's box is drawn into the rows current when it
  arrives and stays on those rows, scrolling off with them, as on the
  desktop. Held tracks draw dimmer.
- The sheet gains a Signals tab: rows sorted by SNR showing frequency, label,
  SNR and time since last seen. Tapping a row tunes the active receiver to
  the detection's centre, sets the bandwidth to the detection's width
  rounded up to the mode's nearest supported width, and scrolls the view to
  it if it is off screen. A confidence slider sits at the top of the tab.

## Visual pass

- Full-bleed spectrum and waterfall; controls float over it. No boxed panels
  around the display.
- Near-black background, one accent hue, the desktop's colour map with
  monotonic lightness.
- Frequency ruler drawn between the spectrum trace and the waterfall.
- Top bar: frequency readout, mode chip, S-meter. Receiver chips smaller
  beneath it. Engine and link problems as chips, not text blocks.
- Bottom sheet with three tabs: Tune, Signals, Radio. Sections that do not
  apply to the active receiver are hidden, not disabled.
- Motion only for cause and effect, under 200 ms, except dial momentum.
- Spectrum and waterfall auto-scale both floor and ceiling over about 30 s
  from what is in view, remapping the gateway's uint8 values on the phone.
- Retuning the centre shifts existing waterfall rows by the frequency change
  instead of clearing them.
- System font stack for text, a monospace for digits.

## Testing

- Web unit tests: gesture classification (one finger drag versus tap versus
  two finger pan), drag rate limit and final send, dial step by speed and
  rounding, momentum settle time, tap snapping to a detection, detection
  list ordering and tap-to-tune bandwidth rounding, auto-scale convergence,
  waterfall row shift on retune.
- Gateway tests: detections event shape, merged tracks dropped, view
  filtering and the 200 cap, `setDetection` validation and resume.
- On the phone: checklist additions in `docs/webgate.md` for drag tuning,
  dial, detections on a live band, and the Signals list.
- `frame-budget.ps1 -Phone` stays within noise with detections polling.

## Out of scope

The narrow fine-tuning display and AFT, auto filter, scanning, band
shortcuts on the phone, landscape-specific layout beyond what already works,
Android.

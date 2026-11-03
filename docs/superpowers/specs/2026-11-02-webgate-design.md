# Web UI for mobile: revenant-webgate

Date: 2026-11-02. Status: design approved in conversation, awaiting spec review.

## Goal

Control and listen to Revenant from a phone browser on the home LAN. First
target is iPhone Safari; Android follows later and must not need a different
gateway.

The phone is a remote control, not a port of the desktop UI. Compute stays on
the engine machine; the phone renders and plays audio.

It is an installed web app, not a page to browse to. The browser is how it
gets onto the phone once; after that it is opened from its home screen icon,
runs full screen with no browser chrome, and behaves like an app.

## Installed app

- Web app manifest: `display: standalone`, portrait-first with landscape
  allowed, theme and background colours matching the desktop, `start_url`
  and `scope` at `/`.
- Its own icon, generated as part of the build like every Revenant program,
  shipped as `assets/revenant-remote.png` at 512 px plus the
  `apple-touch-icon` sizes iOS wants (180 px). Apple splash images for the
  current iPhone sizes.
- `apple-mobile-web-app-capable` and `apple-mobile-web-app-status-bar-style`
  so iOS launches it standalone with the status bar over the app's own
  colour; safe-area insets respected for the notch and home indicator.
- Service worker caching the app shell (HTML, JS, WASM, icons), versioned by
  build, so the app opens instantly and, off the LAN or with webgate down,
  shows its own "Revenant is not reachable" screen with a retry instead of a
  Safari error page. Streams and control are never cached.
- Installed from `https://<hostname>.local:17700/`, not from an IP. iOS
  resolves mDNS `.local` names, so the icon survives the box's address
  changing. The certificate's SANs carry `<hostname>.local` for this reason.
- First visit in Safari shows an install screen (Share, Add to Home Screen)
  instead of the app; the app itself only runs standalone. A query flag
  overrides this for development.
- Nothing is stored on the phone beyond the shell and UI preferences (active
  receiver, view, sheet state) in `localStorage`.

Risk to prove first: background audio from an `<audio>` element in an iOS
home screen app has behaved differently from the same page in Safari across
iOS versions. The first milestone is a throwaway standalone app that plays an
HLS stream from webgate, locked, for ten minutes on the owner's iPhone. If
standalone mode kills it, keep-listening is redesigned before anything else
is built on it.

## Decisions

- LAN only. No authentication on the phone side. Anyone on the LAN who can
  reach the port can drive the radio; `docs/webgate.md` states this plainly.
- HTTPS with a locally generated CA, installed on the phone once. Plain HTTP
  was refused because Safari withholds AudioWorklet, Wake Lock and PWA
  install from insecure contexts.
- A separate C++ process in this repository, `tools/webgate`, built on the
  existing `revenant_rpc_client`. A Node or Go sidecar was refused because it
  needs a second Cap'n Proto binding tracking the schema; serving from inside
  the engine was refused because it puts TLS and a web server in the process
  that owns the GPU and the dongle, and breaks the loopback-only rule in
  `docs/rpc.md`.
- No engine schema changes. Everything used already exists on `Session`.
- Two audio paths: low-latency Opus over WebSocket while the screen is on,
  and an HLS stream for background listening ("Keep listening").

## Topology

```
iPhone Safari --HTTPS/WSS :17700--> revenant-webgate --capnp 127.0.0.1:17690--> revenant-engine
              --HTTP :17701 (CA profile and redirect only)
```

webgate binds 0.0.0.0 on both ports. It reaches the engine on loopback only,
with the engine token from the usual locations (`docs/rpc.md`, token
section). The phone never sees the engine token.

The phone and the Qt UI may be connected at once. Each holds its own engine
`Session`; they share the one radio, so a centre retune from either moves
both.

## Components

`tools/webgate/`, one responsibility per file:

| Unit | Does | Depends on |
| --- | --- | --- |
| `engine_link` | Connects to the engine, reconnects with backoff, starts a local engine with `--no-source` inside a Job object when none answers and `revenant-engine.exe` is beside it | the launch rules, moved out of `ui/models/engine_start.h` into a shared library both use |
| `certs` | First run: creates a CA. Each start: issues a server certificate whose SANs are the hostname, `<hostname>.local`, and every current LAN IPv4 address (no IPv6, owner 2026-11-02), reissuing from the same CA when that set changed. Builds the iOS `.mobileconfig` | OpenSSL |
| `http` | TLS listener on 17700 serving the static app and the WebSocket upgrade; plain listener on 17701 serving `/ca` and a redirect | kj-http, kj-tls |
| `session` | One per WebSocket. Owns an engine `Session` and the receivers it made; ends them when the socket closes | `engine_link` |
| `protocol` | JSON control messages to and from `Session` calls; binary frame encoding | none |
| `spectrum_reducer` | Full-span frame to a phone view: max-pool to pixel width, uint8 dB | none |
| `audio_opus` | 48 kHz float32 chunks to 20 ms Opus frames, ~32 kbit/s | libopus |
| `audio_hls` | Same chunks to AAC-LC in fMP4, 1 s segments, sliding playlist | Media Foundation AAC encoder |

State lives in `%LOCALAPPDATA%\Revenant\webgate\`: `ca.key`, `ca.crt`,
`server.key`, `server.crt`. The CA key never leaves that directory and is
ACL'd like the RPC token.

`web/`, a new top-level directory: TypeScript with no framework, bundled by
esbuild, plus a WASM Opus decoder. CMake runs the build and copies the
output beside `revenant-webgate.exe`. Node is a build-time tool only; the
payload carries no runtime.

New vcpkg dependencies: `opus` (the libopus port), and the `openssl` feature of
`capnproto`.

## Wire protocol

One WebSocket per phone. Control is JSON text frames. Streams are binary
frames whose first byte is a type tag.

### Spectrum

- App sends `setView {centerHz, spanHz, widthPx}` on connect and on pan,
  zoom or rotation.
- webgate subscribes with `everyNth` chosen for about 20 frames a second at
  the source's real block rate.
- Each frame is reduced to `widthPx` bins by maximum, never mean, so a
  single-bin carrier survives any reduction. Zooming in reduces from full
  resolution again rather than magnifying.
- Values are uint8 dB; the frame header carries offset, scale, the frame's
  source sample index (for caption alignment) and the view it was cut for.
- At most one spectrum frame in flight per socket. A frame arriving while
  the previous is unsent is skipped and counted, never queued.
- About 24 KB/s at 1,200 px and 20 fps.

The phone draws the waterfall from these rows.

### Live audio

- `subscribeAudio` on the phone's active receiver; webgate encodes Opus.
- Phone decodes in WASM and plays through an AudioWorklet behind an adaptive
  jitter buffer starting near 80 ms.
- When the socket backs up, webgate drops the oldest Opus packets. The
  engine's queue must never grow because a phone is slow.

### Background audio ("Keep listening")

- Turning it on is the user gesture iOS requires. Audio moves to an HLS
  `<audio>` element at once, about 3 to 4 s behind live, and a banner says
  so. Locking the phone changes nothing.
- On returning to the foreground HLS keeps playing while the live path
  reopens, fills its jitter buffer and calls `resume()`.
  - Resume succeeds: cut to live when the buffer is ready, with a short fade.
  - Resume refused: a "Go live" pill; HLS plays until it is tapped.
  - Silence is never the result of a handoff.
- The cut skips the 3 to 4 s HLS was behind. Accepted.
- webgate keeps encoding HLS for 30 s after the switch back, so a quick
  lock and unlock does not rebuild the stream.

iOS ignores `volume` on media elements, so the two paths are never played
together and crossfaded.

### Control (phone to webgate)

`addVrx`, `removeVrx`, `tune`, `setMode`, `setBandwidth`, `setSquelch`,
`setCenter`, `setGain`, `setGainAuto`, `listSources`, `openSource`,
`setView`, `audio`, `keepListening`, `setTranscription`, `setVrxTranscribe`.

Each maps to the matching `Session` call. Every message carries an id and
gets one reply: the result, or the engine's error text verbatim.

### Events (webgate to phone)

Receiver status, decoded messages (`subscribeDecoded`), transcripts
(`subscribeTranscripts`), transcription status, source state, engine link
state, and receiver removals with the engine's `RetuneRemoval` reason.

## Phone UI, first release

Portrait, one hand:

- Top bar: frequency (tap to type), mode chip, S-meter, speech switch with
  model state beside it (downloading with percent, verifying, loading,
  ready, failed with reason).
- Main area: thin spectrum trace over the waterfall. Drag pans, pinch zooms,
  tap tunes the active receiver, long-press adds a receiver there.
- Transcripts pinned to the waterfall rows their source sample range covers,
  fading as they scroll, as the desktop does.
- Receiver chips; tap selects the active receiver.
- Bottom sheet: tuning (mode, bandwidth, squelch), the decode and transcript
  feed for the active receiver, and the radio panel (source, centre, gain).

Landscape gives the waterfall the full width with a collapsed side rail.

Out of scope for the first release: calibration, plugins, timeshift, trunk
rules, capture search, the detections overlay, Android-specific work.

## Failure handling

- Engine down: phone shows "engine restarting" over a frozen, dimmed
  waterfall. On reconnect webgate recreates the phone's receivers from the
  parameters it holds.
- Gateway unreachable: WebSocket reconnects with backoff; a stalled HLS
  stream is reported, not left silent.
- Refused command: the engine's sentence as a toast; no automatic retry.
- Untrusted certificate: the 17701 page carries the profile and the three
  iOS steps (install profile; Settings, General, VPN and Device Management;
  Settings, General, About, Certificate Trust Settings).
- Receiver removed by a retune elsewhere: the phone shows why.

## Testing

C++ unit tests (Catch2):

- `spectrum_reducer`: a single-bin carrier survives every width; uint8
  scaling round-trips within one step; view edges.
- `audio_opus`, `audio_hls`: accept the engine's real chunk sizes; HLS
  segment durations and playlist sequence numbers.
- `certs`: SANs cover hostname and all addresses; an address change
  reissues from the unchanged CA; key file ACLs.
- `protocol`: each control message maps to the right `Session` call;
  malformed messages are refused with a reason.

Integration: engine on the synthetic source plus webgate; a WebSocket test
client adds a receiver, receives spectrum, Opus and a decode, disconnects,
and the receiver is confirmed gone.

Web client, Node's built-in test runner at build time: jitter buffer, the
background/live handoff state machine, waterfall row to caption mapping.

On the phone, a written checklist in `docs/webgate.md`: CA install, tap to
tune, pinch zoom, lock and unlock with and without a tap, first speech
enable through the model download, engine restart mid-listen.

Performance: `frame-budget.ps1` with and without a phone connected; the
engine's numbers must not move.

# Phone access: revenant-webgate

`revenant-webgate` lets an iPhone control Revenant and listen to it over the
home network. The phone is a remote control: the engine does all the work and
the phone draws the waterfall, plays audio and sends commands. The phone app is
a web app that is installed to the home screen once and then opens full screen
like any other app. Android is not supported yet.

```
iPhone --HTTPS and WSS :17700--> revenant-webgate --Cap'n Proto 127.0.0.1:17690--> revenant-engine
       --HTTP :17701 (certificate profile and a redirect only)
```

## Security: read this first

**There is no authentication on the phone side. Anyone on the same network who
can reach port 17700 can tune the radio, open and close the source, add and
remove receivers, hear the audio and read the decoded text.**

- It is for a home LAN you trust. Do not forward the ports, do not run it on a
  shared or public network, and do not expose it through a tunnel.
- The gateway checks the `Origin` header on the WebSocket and refuses a browser
  page from an origin that is not the gateway's own, so a web page open in
  another browser tab on the LAN cannot drive the radio. That limits browsers.
  It does not limit a script or another program, which sends no `Origin`.
- Live audio is open too. The HLS stream under `/hls/<id>/` that carries
  Keep listening audio can be fetched by any device on the LAN that can reach
  port 17700, without a WebSocket and without the phone's session, for as long
  as the session exists. Treat everything a receiver hears as audible to the
  whole network.
- The certificate profile at `/ca` is served over plain HTTP on 17701, because
  the phone cannot trust HTTPS until it has it. Someone on the network could
  swap it in transit, so install the profile only on a network you trust.
- The gateway reaches the engine on loopback with the engine's token. The phone
  never sees the token (`docs/rpc.md`, "Where the token lives").
- The certificate authority key stays in `%LOCALAPPDATA%\Revenant\webgate\certs`.
  Anyone who can read it can issue a certificate your phone trusts, so do not
  copy that folder anywhere. The CA is name-constrained: it can only vouch for
  this computer's name, names ending in `.local`, and the private IPv4 ranges
  10.0.0.0/8, 172.16.0.0/12 and 192.168.0.0/16, so a stolen key cannot
  impersonate a site on the internet to your phone. The server certificate
  leaves out any of this computer's addresses outside those ranges.
- The gateway replaces a CA that lacks those constraints, or that was made for
  a different computer name, and prints a line saying so. Every phone then has
  to delete the old profile and install the new one (see Installing on the
  iPhone).
- IPv4 only. The gateway does not listen on IPv6 and the certificate carries no
  IPv6 names.

## Running it

```
revenant-webgate.exe
```

The installer puts `revenant-webgate.exe` and a `web` folder beside
`revenant-engine.exe`, adds a Start menu entry, "Revenant phone gateway", and
with the "Start the Revenant phone gateway when anyone signs in" option (on by
default; `/O:gateway_at_signin=off` unattended) starts it at every sign-in.
The `web` folder has to stay beside the exe, or be named with `--web-root`.

### At sign-in

The sign-in start is an `HKLM\Software\Microsoft\Windows\CurrentVersion\Run`
value named "Revenant phone gateway", so it runs as whoever signs in, in their
session, reading their engine token and desktop settings. Uninstall removes it;
so does unticking it in Task Manager's Startup apps.

It has no window. Everything it prints goes to
`%LOCALAPPDATA%\Revenant\webgate\webgate.log` (or `webgate.log` in
`--state-dir`), which keeps the last 1 MB and one older file,
`webgate.log.1`. Started from a terminal it prints there as well, though the
prompt comes back at once, because it is a Windows (not console) program.

One gateway runs per sign-in session and HTTPS port. A second start on the
same port, from the Start menu or a terminal, writes "already running" to the
log and exits.

Two people signed in at once: the first one's gateway holds the ports, and
the second one's fails to bind them and exits with that in its log. Phones
reach the first person's gateway, whether that session is in front or not.

`revenant-webgate.exe --stop` asks every running gateway on the machine to
exit, waits up to 10 s, then terminates any still running; each one ends an
engine it started either way. Exit code 0 means none remain. The installer
runs it before an upgrade and an uninstall, so a running gateway does not
block either. Each gateway has its own stop event, which only its own
account, Administrators and SYSTEM can set, so run elevated it reaches every
user's gateway and run as a user it reaches only that user's own, reporting
the rest as not reachable. Ending `revenant-webgate.exe` in Task Manager also
works.

### Cold start: the engine only while a phone wants it

The gateway starts no engine of its own when it starts, and holds no lease:
an idle gateway at sign-in costs no GPU context and no radio, and the desktop
starts as usual. An engine the desktop or anyone else started is found and
used as it is.

When a phone connects and no engine answers on `--engine`, the gateway starts
`revenant-engine.exe` (from the folder it is in) with `--no-source`. The phone
is sent `{"event":"engine","state":"starting"}` and shows "Starting the
engine..." over the waterfall; the claim waits for the engine (up to 20 s) on
a thread of its own, so other phones, pings and HTTP are served meanwhile, and
a phone that leaves during the start has its claim dropped when it lands. The
gateway then opens the desktop's last radio on it, the way the
desktop does on an engine it started: `engine/lastSource` with that radio's
remembered centre, rate and gain written over it. A desktop that has never
opened a radio leaves the engine with none, and the phone opens one from the
radio panel. Nothing is reopened on an engine the gateway did not start.

### Engine stop: when the phone is known gone

The gateway stops an engine it started once nobody holds the engine's lease
for 10 s. The phone holds the lease for as long as its session lives: through
the 30 s linger after its connection drops, and through Keep listening while
it fetches audio. So the lease coming free is the phone gone, not a phone
locked in a pocket. A desktop on that engine claims the lease too, and an
engine with its lease held is never stopped. A phone that connects during the
10 s keeps it running. The stop is decided under the same lock a phone's
claim takes, so a claim either lands first and cancels it, or waits for the
stopped engine to be gone and then gets a new one; never an engine being
ended under it.

The stop ends the engine's Job object, so the engine and anything it started
exit together, and the gateway waits for the process to be gone before it
logs "stopped the engine it started". A process that has exited has let go of
the dongle and the GPU. The gateway then stays up, idle, and the next phone
starts the engine again.

An engine the gateway did not start, a desktop's or one started by hand, is
never stopped by it. An engine the gateway started also ends when the gateway
does, crash included.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--port N` | 17700 | HTTPS and WebSocket port |
| `--http-port N` | 17701 | Plain HTTP port that serves the certificate profile and redirects to HTTPS |
| `--engine HOST:PORT` | 127.0.0.1:17690 | The engine's Cap'n Proto address. Only a loopback address is launched for |
| `--state-dir DIR` | `%LOCALAPPDATA%\Revenant\webgate` | Where the certificates live |
| `--web-root DIR` | `web` beside the exe | The phone app |
| `--token-file PATH` | the engine's own default | The engine's token file |
| `--session-linger-ms N` | 30000 | How long a phone's session, and its hold on the engine, survives a dropped connection (see below) |

Both ports listen on `0.0.0.0`. The first start creates the certificate
authority and a server certificate and prints the names the certificate covers:
the computer name, `<name>.local` and every IPv4 address the machine has. When
the machine's addresses change the next start issues a new server certificate
from the same authority, so the phone does not need the profile again.

### Windows Firewall

The installer does not add a firewall rule. Forge has no firewall action, and
a hook can only run a signed program from the payload, so a rule would mean
shipping a helper whose only job is to run `netsh`.

The first time the gateway listens, Windows asks whether to allow it. Allow it
on **Private networks only**. If the prompt was dismissed, never appeared, or
the network is classed as Public, add the rule by hand from an elevated
terminal:

```
netsh advfirewall firewall add rule name="Revenant webgate" dir=in action=allow protocol=TCP localport=17700,17701 profile=private program="C:\Program Files\Revenant\revenant-webgate.exe"
```

Remove it with `netsh advfirewall firewall delete rule name="Revenant webgate"`.
If the phone can load nothing, check that the machine's network is marked
Private (Settings, Network and internet, the connection's properties).

## Installing on the iPhone

Do this once. Use the computer name Windows reports, shown below as `<host>`.
The phone has to be on the same network and resolve `<host>.local`, which iOS
does through Bonjour.

1. In Safari, open `http://<host>.local:17701/ca`. Allow the profile download.
   (`http://<host>.local:17701/` shows these same steps.)
2. Settings, General, VPN & Device Management: tap the downloaded profile and
   install it.
3. Settings, General, About, Certificate Trust Settings: turn on full trust for
   the "Revenant Local CA (<computer name>)" certificate, where the computer
   name is the host name in lower case.
4. In Safari, open `https://<host>.local:17700/`. There is no certificate
   warning now.
5. Share, Add to Home Screen. Open Revenant from the new icon. The app only
   runs from the home screen; Safari shows the install instructions instead.

Install from the `.local` name and not an IP address: the icon then keeps
working when the computer's address changes.

## Using it

Tap the waterfall to tune the active receiver or add one. Long-press to add a
receiver there. Drag pans, pinch zooms. The frequency in the top bar can be
typed: `146.52` and `146.52M` are megahertz, `146520k` kilohertz. A bare number
from 1000 to 99999 is refused because it could mean either; add a unit. The
bottom sheet has mode, bandwidth, squelch, the decoded text and the radio panel
(source, centre, gain). The Speech switch turns on transcription; the first use
downloads a model and the top bar shows the progress.

Dragging the receiver's passband retunes it. The dial strip under the
waterfall tunes in steps; a vertical swipe on one digit of the top bar's
frequency steps that digit. With no receiver selected, the same gestures move
the radio's centre and the readout shows the display's centre, dimmed. Tuning a
receiver past the edge of the band recentres the radio so it stays in view.
Long-press a receiver chip to remove it, after a confirmation.

Detections are drawn as boxes on the waterfall, labelled where there is room.
The Signals tab lists them; tapping one tunes the active receiver to it at the
nearest width the sheet offers and pans the view to it. With no receiver it
asks for one first. The confidence slider there sets the lowest
classifier confidence shown, and is remembered on the phone.

Digital voice: P25 Phase 1 plays through the engine's built-in IMBE vocoder.
DMR and D-STAR need an AMBE+2 vocoder plugin in a `vocoders` folder beside
`revenant-engine.exe`; without one the chip says "no vocoder". A chip says
"encrypted" for five seconds after an encrypted call. The Decoders section of
the sheet turns each of the engine's decoders on or off per receiver.

A WFM receiver on a station with RDS shows the station name (PS) on its chip
and beside the top bar's frequency, and each new RadioText goes into the feed
once. Nothing needs turning on. The engine reads RDS from a companion receiver
it opens at 171000 S/s on the same tuning, so the receiver being listened to
keeps its rate and stereo; the companion costs one channel and about 4% more
GPU than the listening receiver. With no channel free for it the phone just
shows no RDS.

### One user at a time

The engine serves one user at a time
(`docs/superpowers/specs/2026-11-03-single-user-engine-design.md`). The
gateway's own connection to the engine only observes, so a gateway with no
phone attached holds nothing and the desktop can start while it runs. A phone
claims the engine as "phone <address> via webgate" when it connects.

- If something else holds the engine (the desktop, another phone, a script),
  the app shows "Revenant is in use by <holder> since <time>" with Take over.
  Nothing else works until then. Take over ends the other client's use and
  tells it why.
- If something else takes the engine from the phone, the app shows "Taken over
  by <holder>", its sound stops, and Take back takes it back.
- The phone keeps the engine through a dropped connection for as long as its
  session lingers (below). When the session expires the phone lets go, and
  receivers opened from it are removed by the engine.
- When the engine restarts, the gateway claims it again for the phone.

The phone shows and controls every receiver the engine has, whoever opened it:
the desktop before a takeover, an engine plugin, the trunk follower. Changing
the centre frequency moves the one radio, and a receiver that falls outside the
new span is removed with the reason shown.

### Display follows the desktop

The phone has no display settings. Its spectrum and waterfall use the desktop
client's: the scale pins, range fit, smoothing, waterfall contrast and pivot,
colour theme and OLED black. The gateway reads them from this Windows
account's registry, `HKCU\Software\Locke Werks\Revenant\display`, every two
seconds and applies the desktop's own scale rules to each phone's rows, so a
change on the desktop reaches the phone within a couple of seconds. With no
desktop settings stored the phone draws the desktop's defaults. A desktop
client running on another PC is not followed; only the one on the machine
running the gateway is.

### Everything else the phone takes from the desktop

The same registry, other groups, read by `tools/webgate/desktop_settings.h`
with the key names of `ui/models/settings.h`. A key the desktop has never
written is absent and nothing is sent for it.

- **Radio front end.** `engine/lastSource` is the URI the desktop last opened,
  for example `rtlsdr://0?agc=1&bw=3000000&direct=auto&freq=903536000&rate=3200000&gain=auto`.
  What that radio was left at is laid over it first, by the desktop's own `restore_radio_uri`: `source/lastRadio` names the radio and `source/radios/<radio>/{sampleRate,centreHz,gain,directSampling}` replace the rate, centre, gain and direct sampling, so a gain changed after opening is the gain the phone opens with. The gateway sends the result parsed in a `desktop` event (below). When the phone
  opens a radio on that same device (same backend, and the same index as the
  listed URI or the same serial) it opens it with every parameter the desktop
  carries, in the desktop's order: rate, bw, gain, agc, direct, ppm, bias,
  whatever is there. Only `freq` is the phone's: its last centre when the
  tuner reaches it, else the desktop's `freq`. A radio with no tune range never
  gets `freq`. Any other device is opened with the phone's own composition
  (2.4 MS/s and a centre). The composition is `web/src/desktop.ts`, a port of
  `with_query_value` from `ui/models/source_choice.h` and the overlay rules of
  `restore_radio_uri` in `ui/models/radio_memory.h`. Calibration is not part of
  this: the engine loads `%LOCALAPPDATA%\Revenant\calibration.txt` for every
  client.
- **Engine-wide switches.** `radio/impulseExcision`, `radio/combCancel`,
  `radio/spurCancel`, `detections/thresholdDb` and `speech/transcribe` are sent
  to the engine when a phone's session starts and again after an engine
  restart (which builds a new session), the way the desktop sends them on every
  connection. The threshold is refused rather than clamped when outside
  -60 to 120 dB, as the desktop does.
- **Detection confidence bar.** A new phone session starts at
  `detections/confidenceBar`, and the phone's slider follows it until the phone's
  user moves the slider, after which the phone's stored value wins.
- **Digital voice level.** See "Digital voice level" below.

The registry is re-read every two seconds. The `desktop` event is
`{"event":"desktop","confidenceBar":0.91,"radio":{"uri":...,"backend":"rtlsdr",
"device":"0","params":{"agc":"1",...}}}`, either field absent when the desktop
has not saved it, sent on connect, on resume and when the values change.
`listSources` entries also carry `serial` now, which the phone uses to match a
device.

### Digital voice level

The desktop deliberately does not AGC digital voice: it applies one fixed boost,
`audio/voiceGainDb` (whole dB, 0 to 30, default 0), and a soft limiter on the
sum (`ui/audio/voice_gain.h`, `ui/audio/mix_stages.h`). The gateway applies the
same to the phone's audio for digital voice receivers (p25p1, dmr, dstar,
tetra), after the 48 kHz resample and before the mix that feeds both the Opus and HLS encoders,
reading `HKCU\Software\Locke Werks\Revenant\audio` every two seconds. A change
ramps linearly over 20 ms. Analogue receivers are untouched; the engine already
levels them.

### What the phone hears

The phone hears one mixed stream: every receiver on the engine, analogue and
digital voice, whoever opened it, while the phone's audio is on. The selected
receiver, if any, is the lead. Each is resampled to 48 kHz, the digital voice
ones (p25p1, dmr, dstar, tetra) are boosted by `audio/voiceGainDb` while
analogue enters at the engine's level, they are summed, and the desktop's soft
limiter holds the sum under -1 dBFS (`tools/webgate/phone_mix.h`, reusing
`ui/audio/mix_stages.h`). Silence between calls is a voice stream that sends
nothing. The same mix feeds live audio and Keep listening, so both play the
same thing.

- Audio is a phone-level switch kept per session. `audio {on:true}` with no
  `vrx` turns it on with no lead; with a `vrx` it also makes that receiver the
  lead. `audio {on:false}` turns it off and forgets the lead. So the receivers
  Auto DV opens are heard with nothing selected. The phone turns its audio on
  by itself when a DV receiver exists or Auto DV is on
  (`web/src/audio/auto_audio.ts`); outside a tap iOS will not start the
  AudioContext, so the "tap to play" note shows until a tap does.
- The gateway subscribes audio for every receiver while the phone's audio is
  on, or Keep listening has its stream up, and unsubscribes when the receiver
  is removed or either ends. The 30 second linger after Keep listening ends
  does not hold them. Removing the lead leaves the rest playing. Which
  receivers are mixed is decided in one function, `PhoneStreams::mix_members`,
  capped at the desktop's rack limit of 64: lead first, then digital voice,
  then analogue.
- Gain staging is the desktop's (`ui/audio/audio_mix.h`,
  `ui/audio/audio_player.cpp`): every stream arrives levelled by the engine
  (`core/engine/listener_level.h`), digital voice is scaled by the voice boost,
  the streams are summed plainly with no per-stream headroom, and the soft
  limiter is the only stage on the sum. The desktop's per-strip gain is not
  known to the gateway and is taken as unity. Squelch needs nothing here: the
  engine writes zeros into a subscription while a receiver's squelch is
  closed, so a closed receiver adds silence.
- With 20 receivers, eighteen of them analogue on open-squelch noise at the
  engine's level, the test "twenty receivers sum bounded" measures the sum
  held under full scale and the lead's tone and a DV tone each well above the
  noise bed in their own bin, and prints the gateway's cost per 20 ms block.
- The engine delivers voice continuously, not an LDU at a time: one chunk per
  receiver chunk covering the same time, zeros between calls
  (`core/rpc/voice_audio.h`). The mix is built to take a burst of an LDU
  (180 ms) as well. The receiver being listened to sets the pace: a 20 ms block
  is cut when it holds a whole one, and it is never skipped. Every other stream
  is read through a jitter buffer: it contributes once it has queued 200 ms (or
  its longest chunk plus two blocks, if more), which is also the delay voice
  has against the receiver being listened to.
- Chunks are as long as the source makes them: 27 ms from a 2.4 MS/s radio,
  262 ms from 250 kS/s, 683 ms from a 96 kS/s recording. Each stream sizes its
  own thresholds from the longest chunk it has pushed, so a slow source is not
  mistaken for a stalled one: it is stalled after 200 ms or two and a half
  chunks of silence, whichever is longer, and its window is 400 ms or the
  jitter buffer plus two chunks. A voice stream from a slow source is
  correspondingly later against the lead.
- A stalled stream starts again when it returns, so the end of one call is not
  played at the start of the next; the receiver being listened to is not judged
  stalled by its own pushes, so the partial block it holds between chunks is
  kept. If the receiver being listened to stalls, or there is none, the voice
  streams are paced by the wall clock, one block per 20 ms, so the mix never
  runs ahead of real time.
- Live Opus packets carry the fixed stream id 0 (`kPhoneMixStream`,
  `tools/webgate/protocol.h`; `MIX_STREAM` in `web/src/protocol.ts`), meaning
  the phone's mix, whichever receiver leads or none. Engine receiver ids start
  at 1. A new lead is the same stream. The encoder and sequence are reset when
  audio is turned off, so every on starts at seq 0 against the decoder the
  phone reset on off. A page cached from before this release filters Opus by
  receiver id and would drop the mix; the service worker reloads the page on
  update, so it does not outlive the upgrade. Keep
  listening has one playlist per session, `/hls/<id>/live.m3u8`; the `hls`
  event gives the id, and the URL stays the same when the listener moves to
  another receiver, so the player is not reloaded. An engine restart builds a
  new session object and the phone is sent a new id and URL.
### DV tab

A fourth tab, DV, appears while the engine has a digital voice receiver (P25,
DMR, D-STAR, TETRA, a trunk follower) or Auto DV is on, and goes when the last
one does; if it was open, Tune opens instead. The logic is the desktop's, run
in the gateway so it carries on with the phone locked:

- Auto DV opens a receiver on every verified DV signal no receiver covers and
  never removes one, by `ui/models/dv_spawn.h`. As on the desktop, turning it
  on holds plugin receivers off, which stops trunk following while it is on;
  Follow comes back when Auto DV goes off.
- Auto DV keeps running while the phone is locked only while its gateway
  session lives: about 30 seconds after the socket drops, or for as long as
  Keep listening is on. Past that the session ends, and on unlock the phone
  sends its switches again and Auto DV picks up where the band is.
- The phone keeps Auto DV, Follow (by its control channel's frequency, since
  receiver ids change), the follower count and any rules it set in its own
  storage, and sends them on every connect and resume.
- When a phone session ends (linger, take over, lease lost), the gateway
  leaves the engine as the phone found it: it lifts the plugin hold that
  phone set and stops the following it started. Turning Auto DV off lifts
  only a hold this phone set.
- Follow trunk appears for a P25 receiver. The engine follows; the gateway
  sends the desktop's saved allow, deny and priority lists (`trunk/*` in the
  desktop's settings) unless the phone set its own, and puts them back if they
  drift.
- The calls and talkgroups tables are `ui/models/call_log.h` folded from the
  decoded messages the gateway already receives, with the desktop's talkgroup
  aliases. Tap a call to select its receiver.

### Keep listening

Live audio plays through the page and stops when iOS suspends it. Keep
listening switches the sound to a stream that iOS plays in the background, so
it carries on with the screen locked.

- Turn it on while the screen is on; iOS requires a tap to start background
  audio. The sound moves to the stream within a few seconds and is then about 3
  seconds behind live. A banner says so.
- The lock screen shows the frequency and mode with play and pause.
- On unlocking, the stream keeps playing while live audio reopens, then it cuts
  to live and skips the 3 seconds. If iOS refuses to restart live audio without
  a tap, a "Go live" button appears and the stream continues until it is tapped.
- Pausing from the lock screen stops the sound; the next unlock brings live
  audio back.

Limits:

- If the source is slower than real time there is less audio than time, and
  the stream underruns whatever the phone does.
- If the connection drops (a locked iPhone often leaves a connection half open),
  the gateway keeps the phone's receivers for 30 seconds (`--session-linger-ms`),
  or for as long as Keep listening is fetching audio.
  Within that time the app reattaches to the same receivers on its own. After
  it, or after the engine restarts and a receiver cannot be recreated, the
  receivers are gone and the app says why.
- After the app cuts back to live, the gateway keeps encoding the stream for
  30 more seconds, so a quick lock and unlock does not rebuild it. With Keep
  listening on, a dropped phone's receivers are also kept until 60 seconds
  after its last fetch of the stream, if that is later than the linger above.
- A phone that is abandoned keeps the engine, and its receivers running, for up
  to the linger time.

## Troubleshooting

**Black screen, waterfall frozen, "engine restarting".** No engine is running
or it has no source open. The app shows the gateway's link state and recovers by
itself when an engine appears. Start the engine, or open a source from the
radio panel. A black screen with no message and nothing in the app usually means
the gateway cannot be reached; see the next two items.

**Background and live audio stop, the app still shows the link up.** The
engine is hung rather than dead: it holds the connection open but does not
answer. The gateway has no timeout on engine calls yet, so Keep listening and
live audio stay silent until the engine answers again or its process exits,
after which the gateway recovers on its own. End `revenant-engine.exe` in Task
Manager if it does not come back.

**"Revenant is not reachable".** The app opened but the gateway did not answer
within 4 seconds. Check that `revenant-webgate.exe` is running, that the phone
is on the same network, and the firewall. The gateway's log is
`%LOCALAPPDATA%\Revenant\webgate\webgate.log`.

**The phone waits a few seconds on first connect.** The gateway is starting
the engine for it (see Cold start). Later phones within the engine's life do
not wait.

**"Background audio failed: background audio unavailable on this Windows
edition".** Keep listening encodes AAC with Media Foundation, which Windows N
editions do not include. Install the Media Feature Pack from Optional
features. The gateway delay-loads Media Foundation, so everything else,
including live audio, works without it.

**The install fails at "stop-phone-gateway".** A gateway could not be
stopped even by terminating it, which in practice means a process stuck in a
driver. Restart the machine and run the installer again. Gateways from before
`--stop` existed (0.3.14 and earlier) are terminated rather than asked.

**Certificate warning in Safari or the app will not install.** The profile is
not installed or not trusted. Redo steps 2 and 3; step 3 is the one that is
forgotten. If the computer's name changed, or the profile came from another
machine, delete the profile under VPN & Device Management and install again.
A warning only on an IP address is expected: the app should be opened by its
`.local` name.

**The phone cannot find `<host>.local`.** Bonjour names do not cross VLANs or
some guest networks and routers. Check both devices are on the same segment, or
download the profile from `http://<address>:17701/ca` with one of the addresses
the gateway printed, but the app itself should be opened by a name the
certificate covers, which the gateway prints at start.

**Nothing connects from the phone, though it works on the computer.** The
firewall. See Windows Firewall above, and that the network is Private.

**The gateway talks to an engine you did not expect.** It uses whatever engine
answers on `--engine`, and only starts its own when none does. Stop the other
engine, or point `--engine` and `--token-file` at the one you want.

**Chip "source behind real time", or choppy audio.** The source cannot produce
samples as fast as they are being consumed, so there is less audio than time.
Audio from the phone is then cut up. This is the source, not the network: the
synthetic source with many emitters is the usual cause (use `emitters=2`), or a
device that cannot keep up at the chosen rate. Lower the rate or the load.

**Audio breaks up on Wi-Fi.** Try Keep listening, which buffers about 3
seconds. Live audio buffers about 80 ms and grows when it underruns.

**The installed app has a gap the height of the status bar at the bottom.**
This is the short-viewport bug: on a cold launch iOS can give an installed
app a layout viewport a status bar short. The app sizes `html` and `body` to
`100lvh` (`web/static/app.css`), which fixes it; a build without that shows
the gap. Safari's default page zoom (Settings, Apps, Safari, Page Zoom) also
applies to installed apps, and anything other than 100% scales the whole
layout. Open the app with `?insets`, or tap with three fingers inside the
installed app, for an overlay with the safe-area insets and viewport heights
and a layout ruler showing where the viewport, the app and the sheet sit.

**A phone is refused with "too many phones connected".** The gateway holds at
most 16 phone sessions. A new connection evicts the session that dropped
longest ago; it is refused only when all 16 are connected right now.

## Protocol notes

- `openSource` closes whatever source is open first, the desktop client's
  included. A phone can switch the radio out from under someone at the
  computer, and the desktop client sees its source close.
- Liveness is app-level, since kj does not expose WebSocket pings. The gateway
  sends a `ping` event every 15 seconds and the app answers with a `pong`
  command; any message from the phone counts. A socket the gateway has heard
  nothing on for 45 seconds is closed, which detaches its session.
- The engine lease. The gateway's link logs in with `observe` and holds no
  lease. Each WebSocket opens its own engine session, logging in with `claim`
  as `phone <address> via webgate`; the engine joins every claim under one
  holder's exact text to one lease, so a phone's resumes and its rebuilt
  sessions share its lease and a phone at another address does not. The first
  event on every connection is `lease`. When the claim is refused the
  connection stays open with no session: every command but `pong`, `resume`
  and `takeOver` is answered with the engine's `in use: <holder> since <time>`.
  `takeOver` logs in with `takeOver`, and the connection then gets a session
  as if it had just connected (`lease` held, `hello`, `link`) before the
  reply. A session whose lease another client takes is destroyed, its
  connection is told with `lease` `lost`, and later commands are refused with
  "Taken over by <holder>". `tools/webgate/phone_hub.h` has the whole of it.
- Every WebSocket opens its own engine session. When the socket drops, the
  session detaches and lingers so the phone can resume it:
  - for 30 seconds;
  - with Keep listening on, also until 60 seconds after the last HLS request
    for one of its receivers, whichever is later.

  `--session-linger-ms` sets the 30 second figure. The session's end closes
  its engine connections, which ends the phone's lease; the gateway itself
  removes no receivers. At most 16 sessions exist at once: a connection past
  that evicts the oldest detached session, or is closed with code 1013 and a
  reason saying too many phones are connected when every session is attached.
- `listDecoders` (no arguments) answers `{decoders, voiceModes}`. Each decoder
  is `{name, description, input, modes}`, from `Session.decoders`: `input` is
  `complexBaseband` or `realAudio`, and `modes` lists the receiver modes it
  reads (empty from an engine too old to say). `voiceModes` lists the digital
  voice modes whose voice will play: `p25p1` always, plus whatever a loaded
  vocoder plugin serves (`dstar`, `dmr`). It is `null` when the engine does not
  report plugins, which means unknown rather than none.
- `setDecoders {vrx, names}` replaces the receiver's whole decoder set; `[]`
  decodes nothing. A name the engine lacks, or one that does not read the
  receiver's mode, is refused in the reply. Until a phone sets one, a `p25p1`,
  `dstar`, `tetra` or `dmr` receiver decodes with its mode's own decoder and
  any other mode with none. The set is kept per receiver in the session, so it
  outlives a resume and is re-applied when an engine restart recreates the
  receiver. A new width keeps it; a new mode goes back to that mode's default.
  Every receiver reply carries the set in force as `decoders`. When the engine
  refuses or ends a subscription anyway, a `vrxStatus` event carries
  `{vrx, decoder, decoderError}` with the engine's sentence.
- RDS needs no command. Every WFM receiver the engine has is read about once a
  second and an `rds` event goes out when the station changes. A refusal
  ("no RDS here", no channel for the companion) sends nothing and is retried
  after 10 seconds.

### Commands

Text frames `{"id", "op", "args"}`; the reply is `{"id", "ok":true, ...}` or
`{"id", "ok":false, "error"}`. Frequencies are absolute hertz. Modes are
`raw am nfm wfm usb lsb dsb cw p25p1 dstar tetra dmr sam`. A receiver reply is
`{vrx, hz, mode, bandwidthHz, squelchDbfs, grantedLowHz, grantedHighHz,
clamped, clampReason?, transcribe, transcribing, decoders}`.
`tools/webgate/dispatch.h` is the authority.

| op | args | reply |
| --- | --- | --- |
| `addVrx` | `hz`, `mode`?, `bandwidthHz`?, `squelchDbfs`? | receiver |
| `removeVrx` | `vrx` | `{vrx}` |
| `tune` | `vrx`, `hz` | receiver |
| `setMode` | `vrx`, `mode` | receiver plus `replaced`, the old id |
| `setBandwidth` | `vrx`, `hz` (0 is the mode's default) | receiver plus `replaced` |
| `setSquelch` | `vrx`, `dbfs` | receiver |
| `setVrxTranscribe` | `vrx`, `choice` (`auto`, `on`, `off`) | receiver |
| `setCenter` | `hz` | `{grantedHz, removed}` |
| `setGain` | `stage`, `db` | `{stage, grantedDb}` |
| `setGainAuto` | `stage`, `on` | `{stage, on}` |
| `listSources` | none | `{sources}` |
| `openSource` | `uri` | `{source}` |
| `setView` | `centerHz`, `spanHz`, `widthPx` | `{centerHz, spanHz, widthPx}` |
| `audio` | `on`, optional `vrx` (the lead) | `{vrx, on}`, or `{on}` with no `vrx` |
| `keepListening` | `on` | `{on}` |
| `setTranscription` | `on` | the speech state, as `transcriptionStatus` |
| `setDetection` | `minConfidence`, 0 to below 1 | `{minConfidence}` |
| `listDecoders` | none | `{decoders, voiceModes}` |
| `setDecoders` | `vrx`, `names` | `{vrx, decoders}` |
| `resume` | `token`, only as the first command | `{resumed:true}`, then the session's state as events |
| `takeOver` | none | `{held:true}`, after the new session's `lease`, `hello` and `link` |
| `pong` | none | none; answers `ping` |
| `autoDv` | `on` | `{on}` |
| `trunkFollow` | `vrx`, `on`, `maxFollowers`? (1 to 16, the desktop's count if absent) | `{on, vrx, maxFollowers}` |
| `trunkStatus` | none | the `trunk` event's fields |
| `setTrunkRules` | `allow`?, `deny`?, `priority`?, in the desktop panel's text form; none of them goes back to the desktop's | `{allow, deny, priorities, skipped}` |
| `dvTab` | `visible` | `{visible}` |

`setDetection` is per session and kept across a resume; detections below the
bar are not sent.

### Events

`{"event": name, ...}`. `tools/webgate/protocol.h` is the authority.

| event | fields | sent |
| --- | --- | --- |
| `lease` | `state` (`held`, `inUse`, `lost`), `holder`; for `inUse` also `since`, `message` | first on every connection, and when the lease is taken over |
| `hello` | `resume`, the token | on connect, after `lease` held |
| `vrx` | a receiver reply plus `openedBy` (`this phone`, `another client`, a plugin's name, `the engine`) | for every receiver the engine has on connect and resume, and when one is added or changed |
| `link` | `state`, `detail`; after a restart `recreated` (the receivers this phone opened) and `lost` (those it could not, and those opened elsewhere) | on connect and when the engine link changes |
| `engine` | `state` ("starting") | before `lease` and `hello`, when the phone's claim is waiting for the gateway to start an engine |
| `source` | `open`; open: `rateHz`, `centerHz`, `epoch`, `realtimeFactor`, `pacedBy`; with the spectrum on, `dataLowHz`, `dataHighHz` | when the stream, centre or pace changes |
| `display` | `waterfall`, `spectrum`, `trace`, `background` | on connect, on resume, when the desktop's settings change |
| `desktop` | `confidenceBar`, `radio`: `{uri, backend, device, params}`; each absent when the desktop has not saved it | on connect, on resume, when the desktop's settings change |
| `detections` | `items`: `{id, centerHz, bandwidthHz, snrDb, confidence, state, label, labelKind, labelConfidence, lastSeen}` | every 500 ms while a view is set |
| `rds` | `vrx`, `pi`, `callSign`, `ps`, `rt`, `pty`; `tp`, `ta`, `music` when stated | when a WFM receiver's station changes, and on resume |
| `vrxStatus` | `vrx` and `audioError`, or `decoder` and `decoderError` | when audio or a decoder fails |
| `vrxRemoved` | `vrx`, `reason` | when the engine removes a receiver |
| `vrxEventsEnded` | `reason` | when removals can no longer be reported |
| `decoded` | `vrx`, `decoder`, `kind`, `text`, `sequence`, `fields` | per decoded message |
| `transcript` | `vrx`, `sequence`, `text`, `centerHz`, `lowHz`, `highHz`, `mode`, `confidence`, `sourceStart`, `sourceEnd` | per transcribed transmission |
| `transcriptionStatus` | the `setTranscription` reply | on connect, resume and change |
| `hls` | `vrx` (the receiver being listened to), `stream` (the session's stream id), `url`; or `url: null` and `error` | when Keep listening's stream starts or fails, and when the listener moves to another receiver (the URL does not change) |
| `ping` | none | every 15 seconds |
| `calls` | `items`: `{id, protocol, system, target, alias?, source, group, encrypted, emergency, frequencyHz, vrx, slot, owner, firstMs, lastMs, state, count}`, newest first, at most 100 | when a row changes, and on resume |
| `talkgroups` | `items`, the same shape, one per talkgroup or station | when a row changes, and on resume |
| `trunk` | `enabled`, `controlVrx`, `maxFollowers`, `tsbks`, `grants`, `note`, `fault`?, `calls`, `rules` | each second while the DV tab is open or following is on, when it changed |
| `dv` | `autoDv`, `follow`: `{vrx, maxFollowers}` or null, `maxFollowers` (the desktop's) | on connect, resume and change |

In `calls`, `state` is `live`, `ended` or `granted` (a control channel grant no
receiver took); `firstMs` and `lastMs` are wall clock milliseconds.
`tools/webgate/dv_feed.h` is the authority for all four.

`dataLowHz` and `dataHighHz` are the outer edges of the spectrum's first and
last bins. The spectrum starts half a coarse channel below the band, so these
are narrower than `centerHz` plus or minus half of `rateHz`, and the phone
draws nothing outside them.

`detections` ids are decimal strings, since a UInt64 does not survive a JSON
number. `state` is `live` or `held`. `labelKind` is `modulation` or `protocol`;
protocol labels (P25, POCSAG) go out because they are the more specific
answer. At most 200 tracks within a view width either side of the view are
sent, the strongest kept.

In `rds`, `pi` is null until decoded, `ps` and `rt` are null until every
segment has arrived, and `pty` is the programme type's long name. The gateway
sets each phone WFM receiver to RBDS before its first read, and again when an
engine restart recreates it, so `pty` names follow the North American table and
`callSign` is derived from the PI. One user holds the engine, so this resets
no one else's decoder.

## Cost to the engine

`scripts/frame-budget.ps1` measures the desktop client against an engine at the
full target load: a synthetic 20 MS/s scene paced to real time and eight
receivers. `-Phone` runs the same measurement with `revenant-webgate` attached
to that engine and a simulated phone connected to the gateway for the whole run
(`scripts/webgate-phone-sim.mjs`): a 1179 px view over the same 20 MHz span, one
NFM receiver, its audio on, answering the gateway's pings. The simulated phone
decodes and draws nothing, so what it adds is the gateway's and the engine's
side of the work.

Measured on 2026-11-02, 60 s each, offscreen platform (`-Seconds 60`, no
`-Visible`), build/ci engine and gateway, `ui/build/vs` client, RTX 4090. The
scene is the script's own: 20 MS/s, two emitters, 8 receivers, 65536 bins,
about 305 frames a second offered. Two runs of each, interleaved. The owner's
dev engine and gateway were running on the same machine and GPU
throughout, so the absolute figures are not the M2 numbers (those are the
`-Visible -OnTop` runs in `docs/ui-spectrum.md`); the comparison is.

| run | main frames | interval mean / p50 / p99 (ms) | over budget | engine frames in | dropped by engine | to display | realtime |
| --- | --- | --- | --- | --- | --- | --- | --- |
| no phone, 1 | 5824 | 10.471 / 10.372 / 12.656 | 5 | 7230 | 87 | 7227 | 1.000 |
| no phone, 2 | 5766 | 10.573 / 10.479 / 12.777 | 3 | 7260 | 55 | 7258 | 1.001 |
| phone, 1 | 5798 | 10.515 / 10.447 / 12.640 | 1 | 7291 | 24 | 7290 | 1.000 |
| phone, 2 | 6018 | 10.132 / 9.942 / 13.469 | 0 | 7281 | 36 | 7277 | 1.000 |

In those runs the phone was sent about 20 spectrum rows a second (1360 rows,
1.65 MB in 68 s), 50 Opus packets a second (3400 packets, 0.31 MB), the
gateway's pings answered, no errors. The engine held 1.000x real time with and
without it, the client received the same number of frames from the engine to
within 1%, and the engine's own drop count did not rise (24 to 87 over four
runs, run-to-run noise). The main window met its budget in all four.

The 20 rows a second is stale: the gateway now sends rows at up to 60 a
second. Measured again on 2026-11-03 with that rate, `-Seconds 60` each:

| run | main frames | interval p50 / p95 / p99 (ms) | over budget | engine frames in | phone rows |
| --- | --- | --- | --- | --- | --- |
| no phone | 5990 | 9.985 / 11.951 / 13.553 | 5 | 7262 | |
| phone | 5799 | 10.455 / 11.779 / 12.514 | 1 | 7273 | 4078 |

Both met the 60 Hz budget, and the engine took in the same number of frames
with the phone as without it.

What this does not show: a real phone on Wi-Fi, which can back up, and HLS
(Keep listening) encoding, which the simulated phone does not request. Both
add work in the gateway process, not the engine. Reproduce with:

```
.\scripts\build.ps1 -Preset ci -NoTest
.\scripts\frame-budget.ps1 -Seconds 60
.\scripts\frame-budget.ps1 -Seconds 60 -Phone
```

`-Phone` needs `node` on PATH and uses ports 27700 and 27701.

## Phone checklist

Not yet run on the finished build. The owner fills in the results below.
Earlier phone checks during the build (iOS 27.2 RC2, installed home screen app)
covered the install, the trace and WebGL waterfall with a synthetic engine, lock
and unlock resuming the waterfall, recovery when the engine came up, and ten
minutes of locked HLS audio. They are not a substitute for this list.

Phone, iOS version and build: (to be filled in)

Install and shell

1. Profile downloads from `http://<host>.local:17701/ca`, installs, and is
   trusted; `https://<host>.local:17700/` opens with no warning.
2. Add to Home Screen gives the Revenant icon; opening it is full screen with
   the splash matching, and Safari itself shows the install instructions.
3. Stop the gateway: the app shows "Revenant is not reachable" with a retry,
   not a Safari error. Start it again and retry.
4. No engine running: a message, not a bare black screen; when the engine
   starts the app recovers without a relaunch.

Waterfall and tuning

5. Portrait: the top bar shows "--- MHz"; the spectrum trace and WebGL
   waterfall draw. Pan, pinch zoom and rotate keep the signals where they are.
6. Tap a signal: a receiver chip appears, the top bar shows its frequency and
   NFM, audio plays, the meter moves.
7. Tap elsewhere: the same receiver retunes. Long-press another signal: a
   second chip, selected, audio follows it. Tap the first chip: audio returns.
8. Type `146.52`, `146520k`, `146.52M`: each tunes to 146.5200. Type `abc`: a
   toast says it is not a frequency. Type `14074`: a toast asks for a unit.
9. Change mode to AM and the bandwidth: audio has at most a brief gap and the
   chip shows the new mode. Move squelch.
10. Remove the receiver: the chip goes, audio stops, no toast. Retune the
    centre so a receiver falls outside: a toast names the frequency and the
    reason.
11. Find radios and open one (allow up to 30 seconds); gain sliders and Auto
    appear for its stages.
12. Landscape: waterfall full width, top bar and chips in the right rail,
    Controls opens a side panel; nothing under the notch or home indicator;
    every control is thumb sized.

Live and background audio

13. Live audio on an active NFM signal is smooth with the source at real time.
14. Tap Keep listening: the toggle turns red, the banner says "about 3 s
    behind", live continues until the stream starts (a few seconds the first
    time) and then there is no gap.
15. Lock the phone. The lock screen shows "<freq> MHz NFM", Revenant, and
    play and pause. Leave it locked for 5 minutes: the audio never stops.
16. Unlock without tapping: the stream keeps playing, then within about a
    second cuts to live, skipping about 3 seconds, and the banner goes. Listen
    for a click at the cut.
17. Lock and unlock again within 30 seconds: the stream resumes within about
    a second after locking and the same cut happens on unlock.
18. If step 16 shows a "Go live" button instead: the stream keeps playing until
    it is tapped, and the tap cuts to live.
19. Lock, press pause on the lock screen: the audio stops. Press play: it
    resumes. After pausing and then unlocking, live audio is playing or starts
    on the next tap, never silent with no message.
20. Keep listening on, screen on, turn it off: live takes over first, then the
    stream stops. No gap.
21. Restart the engine while locked with Keep listening on: the audio resumes
    on the recreated receiver, or a message says why it stopped.

Engine and source

22. Stop the engine: the "engine restarting" pill appears and the waterfall is
    dimmed and frozen. Start it: the pill clears, chips keep their frequencies,
    audio resumes, and anything lost is toasted with its reason.
23. Load the engine past real time or use a slow source: the chip "source
    behind real time" appears top left and clears when it catches up.

Speech

24. The top bar shows "Speech". Tap it: it reads "Speech: downloading N%" (or
    waiting for the model), counts up, goes through verifying and loading to
    "Speech on". The sheet narrates the same.
25. First enable with no network: "Speech failed" with the engine's sentence;
    tap again to retry.
26. On a voice signal, open Controls: the Speech select shows Auto; set it On
    and Off. The choice survives a mode change.
27. Key up on a tuned voice channel: a caption appears at the receiver's
    frequency on the rows of the transmission, scrolls down with them and
    fades, and is gone when the rows leave the bottom.
28. Pan and zoom: captions follow their frequency and clip at the edges.
    More than 20 transmissions: the oldest go first.
29. The feed ("Decoded and spoken") lists the active receiver's lines newest
    first and changes when you switch chips.
30. Change the receiver's mode or bandwidth: captions and feed lines stay with
    the receiver. Restart the engine or reopen the radio: old captions vanish
    and new ones land on the new rows.
31. Reload the page with speech already on: the top bar says "Speech on" at
    once.

Tuning

32. Drag a receiver's passband: it follows the finger and the receiver
    retunes, with audio, without the waterfall panning.
33. Drag and flick the dial strip: the receiver steps and coasts, and stops
    when the strip is touched.
34. Swipe one digit of the top bar's frequency up and down: that digit steps,
    the rest carry.
35. Deselect every receiver: the readout dims and shows the display's centre.
    The dial and a digit swipe move the radio's centre.
36. Tune a receiver past the edge of the band: the radio recentres and the
    receiver stays on screen.
37. Long-press a receiver chip: a confirmation appears; confirm, and the
    receiver goes.

Detections and decoding

38. On a live band, detection boxes sit over the signals and carry labels
    where there is room; they move with pan and zoom.
39. Open Signals and tap a row: the active receiver tunes to it at a fitting
    width and the view pans to it. With no receiver, a toast asks for one.
40. Move the confidence slider: low-confidence boxes drop out. Relaunch: the
    slider keeps its value.
41. P25 voice plays through the built-in IMBE vocoder. DMR plays with an
    AMBE+2 vocoder plugin in a `vocoders` folder beside `revenant-engine.exe`;
    without one, a DMR chip says "no vocoder". An encrypted call shows
    "encrypted" on the chip and it clears a few seconds after.
42. In the sheet's Decoders section, turn a decoder off and on: the chip and
    the feed follow, and a refusal is a short toast.
43. Tune a WFM receiver to a broadcast station with RDS: within a few seconds
    the station name shows on the chip and beside the frequency, and the
    RadioText appears in the feed once per new message.

Display

44. Change the desktop's colour theme, pins or contrast: the phone follows
    within a couple of seconds.
45. Scroll the waterfall on a busy band: smooth, at 60 rows a second, no
    stutter.
46. Open the app with `?insets`, or tap with three fingers in the installed
    app: the overlay and layout ruler appear; the app reaches the bottom edge
    with no gap.

DV tab

47. With no P25, DMR, D-STAR or TETRA receiver, there is no DV tab. Open one:
    the tab appears. Remove it while the DV tab is open: the tab goes and Tune
    opens.
48. Turn Auto DV on over a band with digital voice: a receiver opens on each
    DV signal, none doubled over one already there. With Keep listening on,
    lock the phone for a few minutes: new signals still get receivers.
    Without it, lock for a few minutes and unlock: Auto DV is still on and
    opens receivers for what is on the air. Signals that go quiet keep
    theirs. Remove one: it is not opened again until Auto DV is turned on
    afresh.
49. Select a P25 control channel receiver: Follow trunk appears with the
    desktop's follower count. Turn it on: the trunk line shows blocks and
    grants, follower receivers appear and their calls play.
50. The calls table lists the newest call first with time, talkgroup (the
    desktop's alias where it has one), radio id, receiver and live, ended or
    not followed; an encrypted call is dimmed and marked. Tap a call: its
    receiver becomes the active one. The talkgroups table counts calls.
51. Restart the engine with Auto DV and Follow on: both come back on.

Report: any silence, the click at the cut, whether the Go live button ever
appeared, whether the lock screen controls worked, and anything that was not
reachable with a thumb.

Results: (to be filled in)

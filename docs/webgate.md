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
- Live audio is open too. The HLS streams under `/hls/<vrx>/` that carry
  Keep listening audio can be fetched by any device on the LAN that can reach
  port 17700, without a WebSocket and without the phone's session, for as long
  as the receiver exists. Treat everything a receiver hears as audible to the
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
`revenant-engine.exe`. There is no Start Menu entry; start it from a terminal.
The `web` folder has to stay beside the exe, or be named with `--web-root`.

If no engine answers on `--engine`, the gateway starts `revenant-engine.exe`
(from the folder it is in) with `--no-source`, so the radio is closed until a
source is opened from the phone or the desktop client. The engine it started
exits when the gateway does. An engine that is already running is used as it is.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--port N` | 17700 | HTTPS and WebSocket port |
| `--http-port N` | 17701 | Plain HTTP port that serves the certificate profile and redirects to HTTPS |
| `--engine HOST:PORT` | 127.0.0.1:17690 | The engine's Cap'n Proto address. Only a loopback address is launched for |
| `--state-dir DIR` | `%LOCALAPPDATA%\Revenant\webgate` | Where the certificates live |
| `--web-root DIR` | `web` beside the exe | The phone app |
| `--token-file PATH` | the engine's own default | The engine's token file |
| `--session-linger-ms N` | 30000 | How long a phone's receivers survive a dropped connection (see below) |

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

Several phones can be connected at once, and with the desktop client. They share
one radio, so changing the centre frequency from any of them moves the others,
and a receiver that falls outside the new span is removed with the reason shown.
Each phone owns its own receivers and they end with its session.

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
- A phone that is abandoned leaves its receivers running for up to the linger
  time.

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
is on the same network, and the firewall.

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
- Every WebSocket opens its own engine session. When the socket drops, the
  session detaches and lingers so the phone can resume it:
  - for 30 seconds;
  - with Keep listening on, also until 60 seconds after the last HLS request
    for one of its receivers, whichever is later.

  `--session-linger-ms` sets the 30 second figure. At most 16 sessions exist at once: a connection past
  that evicts the oldest detached session, or is closed with code 1013 and a
  reason saying too many phones are connected when every session is attached.

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

The phone in both runs was connected for the whole measured window and was
receiving what a real one does: about 20 spectrum rows a second (1360 rows,
1.65 MB in 68 s), 50 Opus packets a second (3400 packets, 0.31 MB), the
gateway's pings answered, no errors. The engine held 1.000x real time with and
without it, the client received the same number of frames from the engine to
within 1%, and the engine's own drop count did not rise (it was lower with the
phone, which is run-to-run noise: 24 to 87 over four runs). The main window met
its budget in all four. The engine's numbers did not move beyond run-to-run
noise.

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

Report: any silence, the click at the cut, whether the Go live button ever
appeared, whether the lock screen controls worked, and anything that was not
reachable with a thumb.

Results: (to be filled in)

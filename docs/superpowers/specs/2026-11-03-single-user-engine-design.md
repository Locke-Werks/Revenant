# Single-user engine

Date: 2026-11-03. Status: owner decisions taken in conversation, awaiting spec review.

## Rule

The engine serves one user at a time. Once a client holds it, every other
client is refused, whatever it is: the desktop app, the phone through
webgate, a remote client, a script. Multi-user operation is not anticipated
and nothing is designed around it.

Owner, 2026-11-03: "Once a user is using the engine, it's that one user or
nothing." Refused clients are offered Take over, which ends the holder's use
and tells the holder why.

## The lease

- The engine holds at most one lease. A lease belongs to one RPC connection.
- `Authenticator.login` takes two new arguments: `holder` (who is asking, a
  short human description such as "Revenant desktop on VALKYRIE" or "phone
  10.1.10.57 via webgate") and `mode`:
  - `claim`: take the lease if it is free, else refuse.
  - `takeOver`: take the lease even if held; the previous holder is evicted.
  - `observe`: no lease; the session may only call the read-only methods
    listed below. Used by webgate's own link and by probes.
- A refused claim is an exception whose message names the holder and the
  time the lease was taken, in a fixed format the clients parse:
  `in use: <holder> since <ISO 8601 local time>`.
- Further logins on the connection that holds the lease share it (webgate's
  per-phone sessions live on one connection).
- The lease ends when its connection closes, or when the holder's last
  claiming session is dropped, or on takeover. It never ends on a timer.
- Eviction on takeover: every session on the old holder's connection gets a
  `leaseLost` notice on its VRX event stream with the new holder's
  description, then the engine removes the old holder's receivers and
  subscriptions and closes the connection.
- Observe sessions may call: version and engine info, `listSources`,
  `leaseStatus` (new: holder, since, or free). Everything else is refused
  with "observing only".

Schema changes are additive; an older client that calls `login` with no
`mode` is treated as `claim` with holder "unknown client".

## Webgate

- The engine link's own connection logs in with `observe`. An idle gateway
  holds no lease, so the desktop can start while webgate runs.
- The first phone's session logs in with `claim` and the gateway's phone
  description. While a phone holds the lease, a second phone gets the
  engine's refusal and the Take over choice, enforced in the gateway (the
  engine sees one connection).
- The lease is released when the phone's session expires (30 s after its
  socket drops, or later while keep listening fetches audio), not when the
  socket drops.
- Take over from a phone uses `takeOver`. A phone that loses the lease shows
  "Taken over by <holder>" and stops; reopening the app offers Take over back.
- Auto DV (desktop's ui/models/dv_spawn.h behaviour) and trunk following can
  then run for the phone with no coordination, since only one user exists.
  They are separate work and not part of this spec.

## Desktop

- The desktop logs in with `claim`. Refused: a full-window notice "Revenant is
  in use by <holder> since <time>" with Take over and Quit. No partial UI.
- A desktop that loses the lease shows "Taken over by <holder>" with Take
  back and Quit.

## What this removes

Per-client receiver ownership logic stays (it is harmless), but nothing new
is built for several clients at once. The session linger, resume tokens and
ownership checks in webgate remain because they serve one phone across
locks and reconnects.

## Testing

- Engine RPC tests: claim when free, refused claim message format, takeover
  evicts and notifies, observe limits, lease released on connection close and
  on last session drop, older client without mode treated as claim.
- Webgate tests: idle gateway holds no lease; first phone claims; second
  phone refused with Take over; take over evicts the first phone with
  "Taken over"; lease released after linger; engine restart re-claims for
  the attached phone.
- Desktop: the refusal screen and the taken-over screen, driven by a fake
  link in ui/tests.
- Two-process test: desktop and webgate against one engine, each refusing
  the other.

## Out of scope

Auto DV and trunk following on the phone (next), any multi-user mode,
authentication changes beyond the existing token.

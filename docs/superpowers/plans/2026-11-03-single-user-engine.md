# Single-user engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The engine serves one user at a time, with takeover, and receiver ownership is removed everywhere.

**Architecture:** A lease taken at `Authenticator.login` and tied to one RPC connection; observe sessions for probes; webgate's idle link observes and its phone sessions claim; the desktop claims and shows a refusal or taken-over screen.

**Tech Stack:** C++ (Cap'n Proto RPC, kj, Catch2), Qt/QML desktop client, TypeScript phone app.

**Spec:** `docs/superpowers/specs/2026-11-03-single-user-engine-design.md`

## Global Constraints

- Refusal message format, verbatim: `in use: <holder> since <ISO 8601 local time>`.
- Login modes: `claim`, `takeOver`, `observe`. A login with no mode is `claim` with holder "unknown client".
- Observe sessions may call only: version and engine info, `listSources`, `leaseStatus`. Anything else is refused with "observing only".
- The lease ends on connection close, on the holder's last claiming session dropping, or on takeover; never on a timer.
- On takeover the old holder's sessions get a `leaseLost` notice naming the new holder; the new holder inherits receivers as they are.
- No per-session receiver ownership anywhere: engine, webgate, desktop.
- Schema changes additive only. Build with `scripts/build.ps1` for the main tree and `cmake --build --preset vs` in `ui/` for the desktop; never bare cmake in the main tree. ci preset (`-Preset ci -NoTest`) must stay clean.
- Commits as Locke Werks, imperative subjects, no AI trailers. No em dashes. File content only through Write/Edit tools.

## Review Focus

1. The engine's own startup probes and webgate's link must observe, or the desktop can never start.
2. A crashed holder (connection gone without a clean close) releases the lease promptly.
3. Takeover while the old holder has subscriptions in flight must not crash the engine or leak subscriptions.
4. Receivers opened by the trunk follower and engine plugins are visible and controllable by the holder.
5. An older desktop build (no mode) is refused cleanly when a phone holds the lease.

---

### Task 1: Engine lease and ownership removal

**Files:** `core/rpc/revenant.capnp`, `core/rpc/server.cpp` (and headers), `core/rpc/client.{h,cpp}`, `tests/rpc/` (new `test_rpc_lease.cpp`), `docs/rpc.md`.

**Interfaces:** `login(token, holder :Text, mode :LoginMode)`; `enum LoginMode { claim takeOver observe }`; `Session.leaseStatus() -> (held :Bool, holder :Text, since :Text)`; VRX event `leaseLost(newHolder :Text)`; client: `Client::connect(..., LoginMode, holder)`, `Client::lease_status()`.

- [ ] Tests: claim when free; refused claim message format; takeover evicts, notifies and keeps receivers; observe allowed and refused calls; lease released on connection close and on last claiming session drop; no-mode login acts as claim; any session of the holder can tune, remove and subscribe to receivers opened by another session of the same lease, by plugins and by the trunk follower; receivers removed when the lease ends without takeover.
- [ ] Implement; remove per-session ownership checks in the server.
- [ ] Build and run the rpc tests and the ci preset; commit.

### Task 2: Webgate

**Files:** `tools/webgate/engine_link.*`, `session.*`, `phone_hub.*`, `phone_streams.*`, `dispatch.cpp`, `protocol.*`, `web/src/` (taken-over and in-use screens, Take over button), `tests/webgate/`, `docs/webgate.md`.

- [ ] Tests: idle gateway holds no lease (observe); first phone claims with holder "phone <ip> via webgate"; second phone refused with the engine's text and a Take over option; Take over evicts the first phone, which shows "Taken over by <holder>"; lease released when the phone session's linger ends; engine restart re-claims for the attached phone; no `owns` checks remain and a phone controls receivers it did not open.
- [ ] Implement; delete ownership code.
- [ ] Build, test, ci preset; commit.

### Task 3: Desktop

**Files:** `ui/models/engine_link.*` (login with claim and holder), new refusal and taken-over screens in `ui/qml/`, `ui/tests/`.

- [ ] Tests with a fake link: refused login shows "Revenant is in use by <holder> since <time>" with Take over and Quit; Take over logs in with takeOver; leaseLost shows "Taken over by <holder>" with Take back and Quit; the desktop lists receivers it did not open.
- [ ] Implement; build with the vs preset; run ui tests; commit.
- [ ] Two-process check: desktop refused while a phone session holds the lease, and the reverse.

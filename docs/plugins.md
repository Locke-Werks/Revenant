# Engine plugins

An engine plugin is a DLL the engine loads at startup and runs beside itself.
It observes the engine through events and acts on it through a table of host
functions: open and move receivers, retune the front end, subscribe to any
receiver's decoder. A trunk tracker is the shape it was written for.

`core/plugin/engine_plugin_abi.h` is the contract, and its header comment is
the brief for somebody writing a plugin. This page is the summary.

A plugin that only watches, and can run in another process, is usually better
as an RPC client (`docs/rpc.md`). A plugin is for the case where it is not.

Voice codecs are a separate interface, `core/decode/vocoder_abi.h`, loaded from
the `vocoders` folder. Nothing here changes it.

## Where they load from

Every `.dll` in `plugins` beside `revenant-engine.exe`, scanned once at
startup. `--plugin-dir <path>` names another folder and `--no-plugins` loads
none. The engine prints one line per file under `plugins`, loaded or refused
with the reason, and `Session.enginePlugins` carries the same report, which the
radio panel shows under "engine plugins".

The folder is as trusted as the executable. `LoadLibrary` runs a DLL's entry
point before anything can be checked, so the loader validates a contract and
does not contain a hostile file. Load-time refusals: the DLL does not load, an
entry point is missing, the ABI version differs, or `describe` fails. A plugin
whose `create` returns NULL is loaded and reported as not running.

## The contract in brief

- Plain C, `__cdecl`, five exports: `abi_version`, `describe`, `create`,
  `on_event`, `destroy`. Version 1, compared for equality; `struct_size` on
  every struct that crosses.
- Nothing is owned across the line. Event data is the host's and valid for one
  `on_event` call; strings passed to the host are copied before it returns.
  No exception may escape a plugin function.
- Each plugin gets one thread of its own, and every call into it is made there.
  Events wait in a bounded queue of 1024. A slow plugin loses its oldest
  events and is told how many in `events_dropped_before`, and it cannot hold up
  decoding, audio or a client.
- Host functions may be called from any thread and never block on the engine.
  Each command is queued for the RPC server's event loop, the one thread that
  changes the engine, and its outcome comes back as a `COMMAND_RESULT` event
  with the plugin's tag.
- Frequencies are absolute hertz. The host converts to the engine's baseband
  offsets.

## Events

On start the plugin hears the current state: `SOURCE_OPENED` when a source is
open, then `VRX_ADDED` for every existing receiver. After that, changes:

| Event | Carries |
| --- | --- |
| `SOURCE_OPENED`, `SOURCE_CLOSED` | centre |
| `SOURCE_RETUNED` | new centre, epoch (the front-end retune count), each receiver the retune removed and why |
| `VRX_ADDED`, `VRX_CHANGED` | receiver, owner, absolute centre, bandwidth, mode |
| `VRX_REMOVED` | receiver, owner, reason |
| `DECODED` | one decoded message: decoder, kind, samples, sequence, text, fields |
| `DECODED_ENDED` | why a decoder subscription stopped |
| `COMMAND_RESULT` | tag, code, receiver id, the engine's sentence on refusal |

`describe` returns an interest mask over source, receiver and decoded events.
Command results and the end of the plugin's own subscriptions are always
delivered.

Decoded fields have the same keys an RPC client sees, documented beside each
decoder in `core/rpc/decoders.h`. P25 TSBKs and DMR CSBKs arrive parsed and
with their raw octets.

## What a plugin may change

- Open receivers, and move or remove the ones it opened. A receiver somebody
  else opened is refused with `RV_ENGINE_PLUGIN_ERR_NOT_OWNER`.
- Subscribe to any receiver's decoder. A decoder runs while anything is
  subscribed, a plugin included.
- Retune the front end. Every receiver moves, and any that no longer fit are
  removed, exactly as when a client retunes.

Receivers a plugin opened are removed when the engine stops it. They are not
tied to the single-user lease (`docs/rpc.md`, "One user at a time"): the lease
ending leaves them, and a client holding the lease can see, tune and remove
them, in which case the plugin hears `VRX_REMOVED`.

## Plugins in this tree

`plugins/` builds each one into `plugins` beside `revenant-engine.exe`, so a
dev build runs with them loaded. None links `revenant_core`. No plugin is
built at present.

The P25 Phase 1 trunk tracker that used to be here, `plugins/p25trunk`, was
retired in favour of the engine's built-in follower,
`core/engine/trunk_follower.h`.

## Writing your own

The shape:

1. Copy `core/plugin/engine_plugin_abi.h` into your project. It includes
   `<stdint.h>` and nothing else. Define `RV_ENGINE_PLUGIN_BUILDING_PLUGIN`
   before including it.
2. Define `struct rv_engine_plugin` with whatever state you need. The host
   only holds a pointer to it.
3. Implement the five exports. `abi_version` returns
   `RV_ENGINE_PLUGIN_ABI_VERSION`. `describe` checks `struct_size`, sets the
   interest mask, name and version. `create` checks the host table's
   `struct_size`, allocates, and returns NULL to decline. `on_event` checks
   `struct_size` and dispatches on `type`. `destroy` frees.
4. Wrap the body of every export in `try { } catch (...) { }`. An exception
   reaching the host ends the process.
5. Copy anything you keep out of an event before `on_event` returns.
   Strings, fields and the removed list all belong to the host.
6. Pick a tag per request and match `COMMAND_RESULT` on it. A host function
   that returns anything but `RV_ENGINE_PLUGIN_OK` queued nothing, and no
   result will come, so forget the request there.
7. Ignore event types you do not recognise.

Points that catch people:

- **There is no timer.** You are called when an event arrives. If you need
  time, take it from a decoder you are subscribed to:
  `end_sample / sample_rate` advances with the signal, and each receiver's
  count starts at its own zero. Clock off one receiver and take only forward
  steps, so switching to another receiver's count cannot run the clock
  backwards.
- **Match results on the tag, not the subject.** A result names the request
  it answers. Two requests about the same frequency or receiver can be in
  flight at once, and the older result must not be taken for the newer.
- **Same key, different meaning.** A decoded field's meaning can depend on the
  kind. p25p1's `encrypted` is the voice's on an HDU or LDU2 and the Link
  Control's on an LDU1. Read the decoder's comment in `core/rpc/decoders.h`
  for each kind you use.
- **Your own receivers come back to you.** `VRX_ADDED` fires for them with
  owner `RV_ENGINE_OWNER_THIS_PLUGIN`. Learn their ids from the `add_vrx`
  result instead, and skip them when reacting to other receivers.
- **A release can overtake an add.** If you give up on something while its
  `add_vrx` is still in flight, the result still arrives with a live receiver
  in it. Remove it then.
- **Front-end retunes remove receivers.** Expect `VRX_REMOVED` for your own
  receivers at any time and clean up from it.
- **Dropped events.** `events_dropped_before` says how many you lost. Design
  state that heals from the next events rather than trusting you saw
  everything.
- **Settings.** The plugin has no config channel. A plugin can find its own path
  with `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` and
  reads an `.ini` of the same name.
- **Runtimes.** Build `/MT` or `/MD`, either works, because nothing is freed
  across the line. Never pass the host something it would have to free.

To build one in this tree, add it to `plugins/CMakeLists.txt` with
`revenant_engine_plugin(<name> <sources>)`. Outside the tree, any compiler that
produces a Windows x64 DLL with C exports will do; drop the DLL in `plugins`
beside the engine and look for its line in the engine's startup output or the
radio panel's "engine plugins" list.

To test without a radio, load the DLL and hand it a host table of your own, as
the fixtures in `tests/plugin/` do: record what it asks for, feed it events
with the field keys `core/rpc/decoders.h` documents, and check the requests.

## Not decided

Revenant is GPL-3.0-or-later. Whether the ABI header gets a separate grant so
that a closed plugin can be built against it has not been decided. The header
says the same.

## Tests

`tests/plugin/` builds one fixture plugin per defect and drives the loader and
runner against them. `tests/rpc/test_rpc_engine_plugins.cpp` runs the good
fixture against a live server: it opens a P25 receiver on a synthetic capture,
hears the decoded headers, is refused a client's receiver, and loses its own
receiver when the server stops.

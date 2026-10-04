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
| `SOURCE_RETUNED` | new centre, retune count, each receiver the retune removed and why |
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

Receivers a plugin opened are removed when the engine stops it.

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

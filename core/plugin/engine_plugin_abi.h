/* Revenant engine plugin ABI, version 1.
 *
 * This header is the whole contract between the Revenant engine and an
 * engine plugin. Like core/decode/vocoder_abi.h, it is standalone: it includes
 * <stdint.h> and nothing else, it is plain C, and it can be copied into a
 * plugin project by somebody who has never seen a Revenant checkout. If you
 * are that person, this comment is the brief.
 *
 * WHAT AN ENGINE PLUGIN IS FOR
 *
 * Work that sits above the decoders and drives the radio from what they
 * report: a trunk tracker that reads a control channel and opens a receiver on
 * each voice grant, a logger, a scanner with rules of its own. A plugin
 * observes the engine through events and acts on it through a table of host
 * functions. It never touches samples, and it is never called on a thread
 * that carries them.
 *
 * A plugin that only needs to watch, and can live in another process, is
 * usually better written as an RPC client: docs/rpc.md. This interface exists
 * for the case where that is not good enough.
 *
 * NOTHING OWNS ANYTHING ACROSS THE LINE
 *
 * The engine is built /MT, statically linked against its own C runtime, and a
 * plugin built elsewhere very likely is not. Two runtimes are two heaps, so
 * memory allocated on one side is never freed on the other:
 *
 *   - Every pointer inside an event, strings included, belongs to the host
 *     and is valid only until revenant_engine_plugin_on_event returns. Copy
 *     what you want to keep.
 *   - Every string a plugin passes to a host function is copied by the host
 *     before the function returns.
 *   - The one pointer the plugin owns is its opaque handle, created by
 *     revenant_engine_plugin_create and freed by revenant_engine_plugin_destroy,
 *     both on the plugin's side of the line.
 *
 * The same split rules C++ out: name mangling, exception propagation and
 * standard library layout differ between compilers. Do not let an exception
 * escape any function here. Unwinding into a foreign runtime is undefined, and
 * on Windows it usually ends the process without a message. The host does not
 * catch it for you.
 *
 * LICENCE, STATED RATHER THAN DECIDED
 *
 * Revenant is GPL-3.0-or-later and this file is part of it. Whether the
 * project offers this one header under a separate grant, so that a plugin
 * that is not itself GPL-3 can be built against it, is a decision nobody has
 * taken yet, and this header does not take it. Ask before shipping a closed
 * plugin built from this file.
 *
 * VERSIONING
 *
 * revenant_engine_plugin_abi_version is the first call and the first thing
 * that can refuse. Anything other than the exact version this host was built
 * against is not loaded, and the host says so by name, by number and by file.
 * struct_size on every struct that crosses is the second check, for a plugin
 * built against an edited copy of this header that still claims version 1.
 * A plugin should check struct_size on what it is handed and the host checks
 * it on what it is given.
 *
 * THREADING
 *
 * The host gives each plugin one thread of its own and makes every call into
 * that plugin on it: create, every on_event, and destroy. A plugin needs no
 * locking for its own state unless it starts threads of its own.
 *
 * The host functions in rv_engine_host may be called from any thread, the
 * plugin's own or one it started, at any time between create returning a
 * handle and destroy being called, and from inside create itself. None of them
 * blocks on the engine: each queues a request and returns. The outcome of a
 * command arrives later as an RV_ENGINE_EVENT_COMMAND_RESULT carrying the tag
 * the plugin chose.
 *
 * The plugin's event queue is bounded. A plugin that takes too long in
 * on_event loses the oldest events rather than stalling the engine, and the
 * next event it does receive says how many it missed in events_dropped_before.
 * Nothing a plugin does can hold up decoding, audio or another client.
 *
 * WHAT A PLUGIN MAY CHANGE
 *
 * It may open receivers, retune and remove the receivers it opened, subscribe
 * to any receiver's decoder, and retune the front end. It may not remove or
 * retune a receiver somebody else opened: a client's receiver belongs to the
 * person using it. Retuning the front end moves every receiver, and the
 * engine removes any that no longer fit, exactly as when a client does it;
 * those removals arrive as events like any other.
 *
 * Receivers a plugin opened are removed when the plugin is unloaded.
 *
 * WHAT A PLUGIN HEARS FIRST
 *
 * The engine may already be running when a plugin starts. So the first events
 * after create describe what is already there: RV_ENGINE_EVENT_SOURCE_OPENED
 * when a source is open, then RV_ENGINE_EVENT_VRX_ADDED for every receiver
 * that exists, each subject to the plugin's interests like any other event.
 * After that, every event is a change.
 */

#ifndef REVENANT_ENGINE_PLUGIN_ABI_H
#define REVENANT_ENGINE_PLUGIN_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RV_ENGINE_PLUGIN_ABI_VERSION 1u

/* Calling convention and export marking, on vocoder_abi.h's terms. A plugin
 * defines RV_ENGINE_PLUGIN_BUILDING_PLUGIN before including this header; the
 * host defines nothing and resolves every entry point through GetProcAddress. */
#if defined(_WIN32)
#  define RV_ENGINE_PLUGIN_CALL __cdecl
#  if defined(RV_ENGINE_PLUGIN_BUILDING_PLUGIN)
#    define RV_ENGINE_PLUGIN_EXPORT __declspec(dllexport)
#  else
#    define RV_ENGINE_PLUGIN_EXPORT
#  endif
#else
#  define RV_ENGINE_PLUGIN_CALL
#  if defined(RV_ENGINE_PLUGIN_BUILDING_PLUGIN)
#    define RV_ENGINE_PLUGIN_EXPORT __attribute__((visibility("default")))
#  else
#    define RV_ENGINE_PLUGIN_EXPORT
#  endif
#endif

/* Return codes. #defines and int32_t rather than an enum, because the size of
 * an enum is implementation defined in C. Zero is success and negative is a
 * failure. The host writes its own sentence for every code, and a command the
 * engine refused carries the engine's sentence in the result event's text. */
#define RV_ENGINE_PLUGIN_OK               0

#define RV_ENGINE_PLUGIN_ERR_ARGUMENT    (-1) /* a null pointer, a bad struct_size, an unknown name */
#define RV_ENGINE_PLUGIN_ERR_STOPPED     (-2) /* the host is unloading this plugin and takes no more requests */
#define RV_ENGINE_PLUGIN_ERR_QUEUE_FULL  (-3) /* too many requests outstanding; try again after some results */
#define RV_ENGINE_PLUGIN_ERR_REFUSED     (-4) /* the engine refused the command; the result's text says why */
#define RV_ENGINE_PLUGIN_ERR_NOT_OWNER   (-5) /* the receiver was opened by somebody other than this plugin */
#define RV_ENGINE_PLUGIN_ERR_INTERNAL    (-6) /* the plugin broke and cannot say more */

/* What a plugin wants to hear about, as a mask in rv_engine_plugin_desc.
 * Command results, and the end of a decoder subscription the plugin made, are
 * always delivered whatever the mask says. */
#define RV_ENGINE_INTEREST_SOURCE   0x1u /* SOURCE_OPENED, SOURCE_CLOSED, SOURCE_RETUNED */
#define RV_ENGINE_INTEREST_VRX      0x2u /* VRX_ADDED, VRX_REMOVED, VRX_CHANGED */
#define RV_ENGINE_INTEREST_DECODED  0x4u /* DECODED, from the subscriptions this plugin made */

#define RV_ENGINE_PLUGIN_NAME_CAPACITY    64
#define RV_ENGINE_PLUGIN_VERSION_CAPACITY 32

/* What a plugin says about itself. 104 bytes with no padding. */
typedef struct rv_engine_plugin_desc {
    /* sizeof(rv_engine_plugin_desc) as the host saw it. Set by the host
     * before the call; a plugin that does not recognise it returns
     * RV_ENGINE_PLUGIN_ERR_ARGUMENT. */
    uint32_t struct_size;

    /* RV_ENGINE_INTEREST_* bits. */
    uint32_t interests;

    /* NUL-terminated ASCII, for the operator and the log. Truncated by the
     * plugin if it does not fit, and still terminated. */
    char name[RV_ENGINE_PLUGIN_NAME_CAPACITY];
    char version[RV_ENGINE_PLUGIN_VERSION_CAPACITY];
} rv_engine_plugin_desc;

/* Event types. */
#define RV_ENGINE_EVENT_SOURCE_OPENED   1u /* center_hz, epoch */
#define RV_ENGINE_EVENT_SOURCE_CLOSED   2u
#define RV_ENGINE_EVENT_SOURCE_RETUNED  3u /* center_hz, epoch, removed[] */
#define RV_ENGINE_EVENT_VRX_ADDED       4u /* vrx, owner, vrx_center_hz, bandwidth_hz, demod */
#define RV_ENGINE_EVENT_VRX_REMOVED     5u /* vrx, owner, text = why */
#define RV_ENGINE_EVENT_VRX_CHANGED     6u /* vrx, owner, vrx_center_hz, bandwidth_hz, demod */
#define RV_ENGINE_EVENT_DECODED         7u /* vrx, decoder, kind, samples, sequence, text, fields[] */
#define RV_ENGINE_EVENT_DECODED_ENDED   8u /* vrx, decoder, text = why the subscription ended */
#define RV_ENGINE_EVENT_COMMAND_RESULT  9u /* request_tag, result_code, vrx (for add_vrx), text */

/* Who opened a receiver, relative to the plugin receiving the event. */
#define RV_ENGINE_OWNER_HOST          0u /* the engine process itself, or nobody recorded */
#define RV_ENGINE_OWNER_CLIENT        1u /* an RPC client */
#define RV_ENGINE_OWNER_THIS_PLUGIN   2u
#define RV_ENGINE_OWNER_OTHER_PLUGIN  3u

/* Why a front-end retune removed a receiver. */
#define RV_ENGINE_REMOVED_OTHER        0u
#define RV_ENGINE_REMOVED_OUTSIDE_SPAN 1u /* the receiver's frequency is no longer in the span */
#define RV_ENGINE_REMOVED_UNPLACEABLE  2u /* still in the span, but no channel can carry it */
#define RV_ENGINE_REMOVED_SHAPE        3u /* its new place needs a filter the engine will not swap in place */

typedef struct rv_engine_removed {
    uint32_t vrx;
    uint32_t cause;           /* RV_ENGINE_REMOVED_* */
    int64_t frequency_hz;     /* where the receiver was, absolute */
} rv_engine_removed;

/* Field value types in a decoded message. */
#define RV_ENGINE_FIELD_INT    1u
#define RV_ENGINE_FIELD_DOUBLE 2u
#define RV_ENGINE_FIELD_BOOL   3u
#define RV_ENGINE_FIELD_TEXT   4u /* data/size, and data is also NUL-terminated */
#define RV_ENGINE_FIELD_BYTES  5u /* data/size */

/* One key and value from a decoded message. Keys are lower snake case and
 * stable per decoder; core/rpc/decoders.h documents them beside each decoder,
 * and they are the same keys an RPC client sees. */
typedef struct rv_engine_field {
    const char* key;
    uint32_t type;            /* RV_ENGINE_FIELD_* */
    uint32_t bool_value;      /* 0 or 1, for RV_ENGINE_FIELD_BOOL */
    int64_t int_value;
    double double_value;
    const uint8_t* data;      /* RV_ENGINE_FIELD_TEXT and _BYTES, else NULL */
    uint32_t size;
    uint32_t reserved0;
} rv_engine_field;

/* One event. Which fields mean anything depends on type, and the list beside
 * each RV_ENGINE_EVENT_* says which. The rest are zero, and every string
 * pointer is non-null: an absent string is "". Frequencies are absolute, in
 * hertz, never offsets from the front end. */
typedef struct rv_engine_event {
    uint32_t struct_size;
    uint32_t type;            /* RV_ENGINE_EVENT_* */

    /* Events this plugin's queue dropped, oldest first, since the last one it
     * was given. Zero almost always. */
    uint64_t events_dropped_before;

    /* Source. epoch counts front-end retunes. */
    int64_t center_hz;
    uint64_t epoch;
    const rv_engine_removed* removed;
    uint32_t removed_count;

    /* Receiver. */
    uint32_t vrx;
    uint32_t owner;           /* RV_ENGINE_OWNER_* */
    uint32_t bandwidth_hz;
    int64_t vrx_center_hz;
    const char* demod;        /* "nfm", "p25p1", "dmr" and so on */

    /* Decoded. Samples count in the receiver's own stream at sample_rate.
     * sequence is the decoder's, and dropped_before is messages that decoder
     * queue lost for this subscription, which is separate from
     * events_dropped_before above. */
    const char* decoder;
    const char* kind;         /* "tsbk", "csbk", "hdu" and so on */
    uint64_t start_sample;
    uint64_t end_sample;
    uint64_t sequence;
    uint64_t dropped_before;
    uint32_t sample_rate;
    uint32_t field_count;
    const rv_engine_field* fields;

    /* Command result. */
    uint32_t request_tag;
    int32_t result_code;      /* RV_ENGINE_PLUGIN_OK or a negative code */

    /* Prose: a removal's reason, a decoded message's text, a refusal's
     * sentence. */
    const char* text;
} rv_engine_event;

/* Log levels for rv_engine_host::log. */
#define RV_ENGINE_LOG_DEBUG 0u
#define RV_ENGINE_LOG_INFO  1u
#define RV_ENGINE_LOG_WARN  2u
#define RV_ENGINE_LOG_ERROR 3u

/* What the host offers a plugin. Owned by the host and valid from create
 * until destroy returns. Every function takes ctx first, which the plugin
 * passes back exactly as it found it here.
 *
 * Every command returns RV_ENGINE_PLUGIN_OK when the request was queued, or a
 * negative code when it was refused before queueing, in which case no result
 * event follows. A queued request always produces exactly one
 * RV_ENGINE_EVENT_COMMAND_RESULT with the same tag. Tags are the plugin's own
 * and the host never interprets them. */
typedef struct rv_engine_host {
    uint32_t struct_size;
    uint32_t reserved0;
    void* ctx;

    /* Open a receiver at an absolute frequency. demod is a mode name as the
     * engine knows it: "nfm", "am", "usb", "raw", "p25p1", "dmr". A
     * bandwidth of 0 is the mode's own default. The result carries the new
     * receiver's id in vrx. */
    int32_t (RV_ENGINE_PLUGIN_CALL* add_vrx)(void* ctx, int64_t center_hz, uint32_t bandwidth_hz,
                                             const char* demod, uint32_t tag);

    /* Remove, or move to an absolute frequency, a receiver this plugin
     * opened. Anybody else's is refused with RV_ENGINE_PLUGIN_ERR_NOT_OWNER in
     * the result. */
    int32_t (RV_ENGINE_PLUGIN_CALL* remove_vrx)(void* ctx, uint32_t vrx, uint32_t tag);
    int32_t (RV_ENGINE_PLUGIN_CALL* set_vrx_center)(void* ctx, uint32_t vrx, int64_t center_hz,
                                                    uint32_t tag);

    /* Retune the front end. Every receiver moves with it, and any that no
     * longer fit are removed. */
    int32_t (RV_ENGINE_PLUGIN_CALL* set_source_center)(void* ctx, int64_t center_hz,
                                                       uint32_t tag);

    /* Start, or stop, receiving DECODED events from one decoder on one
     * receiver: any receiver, not only this plugin's. decoder is a registry
     * name such as "p25p1" or "dmr", or "" for the decoder named after the
     * receiver's mode. A decoder runs while anything is subscribed to it. */
    int32_t (RV_ENGINE_PLUGIN_CALL* subscribe_decoded)(void* ctx, uint32_t vrx,
                                                       const char* decoder, uint32_t tag);
    int32_t (RV_ENGINE_PLUGIN_CALL* unsubscribe_decoded)(void* ctx, uint32_t vrx,
                                                         const char* decoder, uint32_t tag);

    /* A line for the engine's log, prefixed with the plugin's file name. */
    void (RV_ENGINE_PLUGIN_CALL* log)(void* ctx, uint32_t level, const char* message);
} rv_engine_host;

/* One running plugin. Allocated and freed by the plugin; opaque to the host. */
typedef struct rv_engine_plugin rv_engine_plugin;

/* The version of this ABI the plugin was built against. Called first and from
 * any thread; must not depend on any initialisation. */
RV_ENGINE_PLUGIN_EXPORT uint32_t RV_ENGINE_PLUGIN_CALL revenant_engine_plugin_abi_version(void);

/* Describe the plugin. The host sets out_desc->struct_size and the plugin
 * fills the rest and returns RV_ENGINE_PLUGIN_OK. Called from any thread,
 * possibly before create and possibly more than once. */
RV_ENGINE_PLUGIN_EXPORT int32_t RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_describe(rv_engine_plugin_desc* out_desc);

/* Start the plugin. NULL refuses, and the host reports the plugin as loaded
 * and declined. host stays valid until destroy returns. */
RV_ENGINE_PLUGIN_EXPORT rv_engine_plugin* RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_create(const rv_engine_host* host);

/* One event. Everything ev points at is the host's and gone when this
 * returns. */
RV_ENGINE_PLUGIN_EXPORT void RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_on_event(rv_engine_plugin* self, const rv_engine_event* ev);

/* Stop the plugin. Called exactly once per successful create, on the same
 * thread as every other call. After it returns, the host table is gone and
 * calling any host function is undefined. A plugin that started threads joins
 * them here. */
RV_ENGINE_PLUGIN_EXPORT void RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_destroy(rv_engine_plugin* self);

typedef uint32_t (RV_ENGINE_PLUGIN_CALL* rv_engine_plugin_abi_version_fn)(void);
typedef int32_t (RV_ENGINE_PLUGIN_CALL* rv_engine_plugin_describe_fn)(rv_engine_plugin_desc*);
typedef rv_engine_plugin* (RV_ENGINE_PLUGIN_CALL* rv_engine_plugin_create_fn)(const rv_engine_host*);
typedef void (RV_ENGINE_PLUGIN_CALL* rv_engine_plugin_on_event_fn)(rv_engine_plugin*,
                                                                   const rv_engine_event*);
typedef void (RV_ENGINE_PLUGIN_CALL* rv_engine_plugin_destroy_fn)(rv_engine_plugin*);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* REVENANT_ENGINE_PLUGIN_ABI_H */

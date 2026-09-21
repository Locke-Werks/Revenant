/* Revenant vocoder plugin ABI, version 1.
 *
 * This header is the whole contract between Revenant and a vocoder plugin. It
 * is deliberately standalone: it includes <stdint.h> and nothing else, it is
 * plain C, and it can be copied into a plugin project by somebody who has
 * never seen a Revenant checkout. If you are that person, this comment is the
 * brief.
 *
 * WHAT A VOCODER PLUGIN IS FOR
 *
 * Revenant decodes the framing of digital voice systems in software. Some of
 * those systems carry a voice codec whose algorithm is published, and those
 * decode in software too. Some carry a codec that was never written down
 * anywhere, at any price, and cannot be implemented from a document because
 * there is no document. For those the only route to audio is a device or a
 * library somebody else owns, which is what this interface loads.
 *
 * The interface is a vocoder interface and nothing more. It names no codec,
 * it assumes no frame size and it contains no constant taken from any codec's
 * specification. The plugin declares what it does and Revenant either has a
 * caller that wants that shape or it does not.
 *
 * THE RULE THAT SHAPES EVERYTHING BELOW: NOTHING OWNS ANYTHING ACROSS THE LINE
 *
 * Revenant's engine is built /MT, statically linked against the multithreaded
 * CRT, because the whole toolchain is x64-windows-static. A plugin DLL is
 * built by somebody else, with a compiler this project has never seen, and it
 * will very likely be /MD. Two modules linked against two CRTs have two heaps.
 * A block allocated inside the plugin and freed by the host, or the reverse,
 * is freed from the wrong heap, and the result is a crash at free() with a
 * call stack pointing at the innocent party. It is not a warning, it is not
 * intermittent on a debug build, and it has nothing to do with this being a
 * vocoder.
 *
 * So the ABI is caller-allocates throughout:
 *
 *   - The host hands over a buffer and a capacity. The plugin fills what fits
 *     and reports how much it wrote. It never returns memory.
 *   - Strings are fixed arrays inside caller-owned structs, never char*.
 *   - The one pointer that does cross is the opaque handle from
 *     revenant_vocoder_create, which the host gives straight back to
 *     revenant_vocoder_destroy. The plugin allocates it and the plugin frees
 *     it, both on its own side of the line.
 *
 * The same split rules C++ out of the ABI entirely. Name mangling, exception
 * propagation, vtable layout and the layout of every standard library type
 * differ between compiler versions, let alone between compilers. Plain C,
 * an opaque handle and a table of free functions is the only shape that
 * survives it. Do not let a C++ exception escape any of these functions:
 * unwinding into a foreign runtime is undefined, and on Windows it usually
 * terminates the process without a message.
 *
 * LICENCE, STATED RATHER THAN DECIDED
 *
 * Revenant is GPL-3.0-or-later and this file is part of it. Whether the
 * project offers this one header under a separate grant, so that a plugin
 * that is not itself GPL-3 can be built against it, is a decision nobody has
 * taken yet, and this header does not take it. Ask before shipping a closed
 * plugin built from this file.
 *
 * VERSIONING, AND WHY A MISMATCH IS LOUD
 *
 * revenant_vocoder_abi_version is the first thing the host calls and the
 * first thing that can refuse. A plugin that answers with anything other than
 * the exact version this host was built against is not loaded, and the host
 * says so by name, by number and by file. The alternative is calling a
 * function whose signature changed under it, which is a crash somebody has to
 * debug from a minidump instead of a line of log.
 *
 * rv_vocoder_desc::struct_size is the second check and it covers a narrower
 * mistake: a plugin built against an edited copy of this header that still
 * claims version 1. Both sides fill in struct_size with their own sizeof
 * before any call that carries a descriptor, and a disagreement is refused
 * the same way.
 *
 * THREADING
 *
 * A handle is not thread safe. The host serialises every call on one handle,
 * so a plugin needs no locking of its own for per-handle state. Two handles
 * may be in use on two threads at once, so anything shared between handles
 * inside the plugin, a USB device among them, is the plugin's to protect.
 * revenant_vocoder_abi_version and revenant_vocoder_describe may be called
 * from any thread at any time and must be safe to call concurrently.
 */

#ifndef REVENANT_VOCODER_ABI_H
#define REVENANT_VOCODER_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version this header describes. A plugin reports it from
 * revenant_vocoder_abi_version and the host compares for exact equality. */
#define RV_VOCODER_ABI_VERSION 1u

/* Calling convention and export marking.
 *
 * __cdecl is spelled out although x64 Windows has only one convention,
 * because the declaration is what a 32-bit or non-Windows port would get
 * wrong, and a convention mismatch corrupts the stack rather than failing to
 * link.
 *
 * A plugin defines RV_VOCODER_BUILDING_PLUGIN before including this header.
 * The host defines nothing: it resolves every entry point through
 * GetProcAddress, so it needs no import declaration and must not get a
 * dllimport one. */
#if defined(_WIN32)
#  define RV_VOCODER_CALL __cdecl
#  if defined(RV_VOCODER_BUILDING_PLUGIN)
#    define RV_VOCODER_EXPORT __declspec(dllexport)
#  else
#    define RV_VOCODER_EXPORT
#  endif
#else
#  define RV_VOCODER_CALL
#  if defined(RV_VOCODER_BUILDING_PLUGIN)
#    define RV_VOCODER_EXPORT __attribute__((visibility("default")))
#  else
#    define RV_VOCODER_EXPORT
#  endif
#endif

/* Return codes.
 *
 * Every one of these is a #define rather than an enum, and the functions
 * return int32_t rather than an enum type, because the size and signedness of
 * an enum are implementation defined in C and this contract has to hold
 * across two compilers that have never met.
 *
 * Zero is success. Positive is a non-error outcome, currently only "no more"
 * from the enumerator. Negative is a failure, and the host turns the number
 * into a sentence itself rather than asking the plugin for a string, so that
 * a plugin cannot report a code it has no message for and leave the operator
 * with a bare integer. A negative code this host does not recognise is
 * reported as the raw number with the plugin's filename beside it, which is
 * still better than silence. */
#define RV_VOCODER_OK                  0
#define RV_VOCODER_NO_MORE             1

#define RV_VOCODER_ERR_ARGUMENT       (-1) /* a null pointer, or a struct_size the plugin does not accept */
#define RV_VOCODER_ERR_BIT_COUNT      (-2) /* bit_count is not the frame size this handle decodes */
#define RV_VOCODER_ERR_CAPACITY       (-3) /* out_capacity is smaller than one frame of output */
#define RV_VOCODER_ERR_FRAME_REJECTED (-4) /* the frame arrived and was not usable: FEC failed, or it was not speech */
#define RV_VOCODER_ERR_DEVICE         (-5) /* hardware behind the plugin is absent, busy or stopped answering */
#define RV_VOCODER_ERR_UNSUPPORTED    (-6) /* the plugin cannot provide what was asked for */
#define RV_VOCODER_ERR_INTERNAL       (-7) /* the plugin broke and cannot say more */

/* Codec families, as an open registry of numbers.
 *
 * A plugin that implements something with no number here reports
 * RV_VOCODER_KIND_EXTERNAL and puts a human-readable identifier in
 * rv_vocoder_desc::name. Revenant then reports what the plugin called itself
 * and claims nothing further about it. That is the honest arrangement for a
 * codec Revenant does not implement: the host has no standing to classify
 * something it cannot decode. */
#define RV_VOCODER_KIND_UNKNOWN  0u
#define RV_VOCODER_KIND_IMBE     1u
#define RV_VOCODER_KIND_CODEC2   2u
#define RV_VOCODER_KIND_EXTERNAL 3u

/* Bytes in rv_vocoder_desc::name, terminator included. A name that does not
 * fit is truncated by the plugin, which must still terminate it. */
#define RV_VOCODER_NAME_CAPACITY 32

/* What one vocoder offers.
 *
 * Every field is uint32_t and the name is a fixed array, so the layout is 56
 * bytes with no padding on every ABI this will meet. Nothing here is taken
 * from any codec's specification: the numbers are whatever the implementation
 * says they are, which is the only way a header in a clean-room project can
 * carry them at all. */
typedef struct rv_vocoder_desc {
    /* sizeof(rv_vocoder_desc) as the writer of this struct saw it. Filled in
     * by whoever allocates the struct, checked by whoever receives it. */
    uint32_t struct_size;

    /* One of the RV_VOCODER_KIND_* values. */
    uint32_t kind;

    /* Bits in one compressed frame, one per byte on the wire into decode.
     * Whether that is before or after forward error correction is the
     * plugin's choice, and the name should say which. */
    uint32_t bit_count;

    /* Audio samples one frame produces. Not bytes, and not a frame count in
     * any other sense: this is the exact number of floats decode writes. */
    uint32_t pcm_frames;

    /* Output sample rate in whole hertz. */
    uint32_t sample_rate;

    /* Must be zero. Present so the struct stays 56 bytes if a flag word is
     * ever needed, which would be an ABI version bump either way. */
    uint32_t reserved0;

    /* NUL-terminated ASCII, for the operator and the log. Something a person
     * can match against what they plugged in: "imbe-tia102", "codec2-3200",
     * "acme-dongle-fw2.1". */
    char name[RV_VOCODER_NAME_CAPACITY];
} rv_vocoder_desc;

/* One decoder instance. Allocated and freed by the plugin; opaque to the
 * host, which never dereferences it and never frees it. */
typedef struct rv_vocoder rv_vocoder;

/* The version of this ABI the plugin was built against.
 *
 * Called first, before anything else in the module is touched. Must not
 * depend on any initialisation the plugin has not already done, because a
 * host that refuses the version will call nothing else. */
RV_VOCODER_EXPORT uint32_t RV_VOCODER_CALL revenant_vocoder_abi_version(void);

/* Enumerate what this plugin provides.
 *
 * The host sets out_desc->struct_size to its own sizeof and calls with
 * index 0, 1, 2 and so on until the return is RV_VOCODER_NO_MORE. The plugin
 * fills the remaining fields and returns RV_VOCODER_OK, or returns
 * RV_VOCODER_NO_MORE when index is past the end, or RV_VOCODER_ERR_ARGUMENT
 * when out_desc is null or carries a struct_size the plugin does not accept.
 *
 * A plugin that enumerates nothing is loaded and then reported as offering
 * nothing, which is a thing the operator is told rather than a thing that
 * silently does not appear. */
RV_VOCODER_EXPORT int32_t RV_VOCODER_CALL revenant_vocoder_describe(uint32_t index,
                                                                    rv_vocoder_desc* out_desc);

/* Open one decoder.
 *
 * want is a descriptor the host got from revenant_vocoder_describe, handed
 * back unchanged. The plugin returns a handle, or NULL when it will not or
 * cannot serve that request: no device attached, device already in use by
 * another handle, a descriptor it does not recognise.
 *
 * NULL carries no reason, on purpose. A reason string would have to cross the
 * boundary with a lifetime, and the host already knows exactly what it asked
 * for and exactly what the plugin said it offers, so it can write a better
 * refusal from its own side than the plugin could hand it. */
RV_VOCODER_EXPORT rv_vocoder* RV_VOCODER_CALL revenant_vocoder_create(const rv_vocoder_desc* want);

/* Decode one compressed frame to audio.
 *
 * bits is bit_count bytes, one bit per byte, each byte 0 or 1, in
 * transmission order. Not packed. A packed representation would need an
 * agreed bit order and an agreed padding rule at the end of a frame whose
 * length is not a multiple of eight, and both are things two implementations
 * get wrong in opposite directions without either noticing.
 *
 * out is the host's buffer, out_capacity floats of it. On RV_VOCODER_OK the
 * plugin has written exactly pcm_frames floats, has set *out_written to that
 * number, and has touched nothing beyond it. Audio is normalised to the
 * closed range -1 to +1.
 *
 * On any failure the plugin sets *out_written to 0 and leaves out alone. The
 * host treats *out_written greater than out_capacity as a broken plugin,
 * refuses the result, and stops using that handle. It cannot undo a write
 * that already happened, so this detects the fault rather than preventing it;
 * preventing it is the plugin's job and honouring out_capacity is the whole
 * of that job. */
RV_VOCODER_EXPORT int32_t RV_VOCODER_CALL revenant_vocoder_decode(rv_vocoder* self,
                                                                  const uint8_t* bits,
                                                                  uint32_t bit_count,
                                                                  float* out,
                                                                  uint32_t out_capacity,
                                                                  uint32_t* out_written);

/* Drop everything carried between frames.
 *
 * Called when the host loses the signal, retunes, or starts a new
 * transmission. A vocoder with interframe state that is not reset will
 * reconstruct the first frames of a new call out of the last frames of the
 * previous one. Must leave the handle usable. */
RV_VOCODER_EXPORT void RV_VOCODER_CALL revenant_vocoder_reset(rv_vocoder* self);

/* Close one decoder. NULL is a no-op. The host calls this exactly once per
 * successful create, and never touches the handle again afterwards. */
RV_VOCODER_EXPORT void RV_VOCODER_CALL revenant_vocoder_destroy(rv_vocoder* self);

/* Pointer types for the host's GetProcAddress, and for any plugin that wants
 * to check its own definitions against the contract:
 *
 *   static const rv_vocoder_decode_fn check = revenant_vocoder_decode;
 *
 * which fails to compile if the signature has drifted. */
typedef uint32_t (RV_VOCODER_CALL* rv_vocoder_abi_version_fn)(void);
typedef int32_t (RV_VOCODER_CALL* rv_vocoder_describe_fn)(uint32_t, rv_vocoder_desc*);
typedef rv_vocoder* (RV_VOCODER_CALL* rv_vocoder_create_fn)(const rv_vocoder_desc*);
typedef int32_t (RV_VOCODER_CALL* rv_vocoder_decode_fn)(rv_vocoder*,
                                                        const uint8_t*,
                                                        uint32_t,
                                                        float*,
                                                        uint32_t,
                                                        uint32_t*);
typedef void (RV_VOCODER_CALL* rv_vocoder_reset_fn)(rv_vocoder*);
typedef void (RV_VOCODER_CALL* rv_vocoder_destroy_fn)(rv_vocoder*);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* REVENANT_VOCODER_ABI_H */

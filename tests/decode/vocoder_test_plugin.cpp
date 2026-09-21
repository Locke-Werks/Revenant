// A vocoder plugin that exists only to be loaded by the real loader.
//
// One source, five DLLs. Each target defines one macro below and gets one
// specific defect, because the loader's job is almost entirely about what it
// does with a plugin that is wrong and a test that can only exercise the happy
// path proves the least interesting third of it.
//
//   rv_test_vocoder_good      correct, and drives the decode and reset cases
//   rv_test_vocoder_nosym     exports everything except decode
//   rv_test_vocoder_oldabi    reports ABI version 0
//   rv_test_vocoder_overrun   claims to have written past the caller's buffer
//   rv_test_vocoder_refuse    enumerates a vocoder and then refuses to open it
//
// WHAT THIS FIXTURE DOES NOT PROVE
//
// It is compiled by this project's toolchain, so it is /MT against
// x64-windows-static like everything else in the tree, and both sides of the
// boundary therefore share a heap. The caller-allocates rule in
// core/decode/vocoder_abi.h exists for the /MD plugin that does not, and no
// test in this repository can demonstrate that crash without a second
// toolchain. What these cases do check is the property that makes the rule
// keepable: no call in the ABI returns memory, and the host frees nothing the
// plugin allocated. A future change that added an allocating entry point would
// have to change this file to compile, which is the alarm.
//
// THE FRAME SHAPE IS DELIBERATELY NOT ANY REAL CODEC'S. 12 bits in, 24 samples
// out. A fixture claiming a published codec's numbers would be a constant with
// no citation sitting in a test, and docs/clean-room.md does not have an
// exception for test code.

#define RV_VOCODER_BUILDING_PLUGIN 1

#include "core/decode/vocoder_abi.h"

#include <cstdint>
#include <cstring>
#include <new>

namespace {

constexpr std::uint32_t kBitCount = 12;
constexpr std::uint32_t kPcmFrames = 24;
constexpr std::uint32_t kSampleRate = 8000;

#ifndef RV_TEST_PLUGIN_ABI_VERSION
#define RV_TEST_PLUGIN_ABI_VERSION RV_VOCODER_ABI_VERSION
#endif

void fill_descriptor(rv_vocoder_desc* out)
{
    out->kind = RV_VOCODER_KIND_EXTERNAL;
    out->bit_count = kBitCount;
    out->pcm_frames = kPcmFrames;
    out->sample_rate = kSampleRate;
    out->reserved0 = 0;
    std::memset(out->name, 0, sizeof(out->name));
    std::memcpy(out->name, "fixture-a", sizeof("fixture-a"));
}

}  // namespace

// The handle. Allocated and freed entirely on this side of the line, which is
// the only pointer the ABI lets cross at all.
struct rv_vocoder {
    std::uint32_t magic;
    std::uint32_t decode_count;
};

namespace {
constexpr std::uint32_t kHandleMagic = 0x564F4358u;  // "VOCX", to catch a foreign pointer
}  // namespace

extern "C" {

RV_VOCODER_EXPORT std::uint32_t RV_VOCODER_CALL revenant_vocoder_abi_version(void)
{
    return RV_TEST_PLUGIN_ABI_VERSION;
}

RV_VOCODER_EXPORT std::int32_t RV_VOCODER_CALL revenant_vocoder_describe(std::uint32_t index,
                                                                         rv_vocoder_desc* out_desc)
{
    if (out_desc == nullptr || out_desc->struct_size != sizeof(rv_vocoder_desc)) {
        return RV_VOCODER_ERR_ARGUMENT;
    }
    if (index > 0) {
        return RV_VOCODER_NO_MORE;
    }
    fill_descriptor(out_desc);
    return RV_VOCODER_OK;
}

#ifdef RV_TEST_PLUGIN_REFUSE_CREATE
// Stands in for the dongle that is enumerated and not plugged in.
RV_VOCODER_EXPORT rv_vocoder* RV_VOCODER_CALL revenant_vocoder_create(const rv_vocoder_desc*)
{
    return nullptr;
}
#else
RV_VOCODER_EXPORT rv_vocoder* RV_VOCODER_CALL revenant_vocoder_create(const rv_vocoder_desc* want)
{
    if (want == nullptr || want->struct_size != sizeof(rv_vocoder_desc)) {
        return nullptr;
    }
    if (want->bit_count != kBitCount || want->pcm_frames != kPcmFrames
        || want->sample_rate != kSampleRate) {
        return nullptr;
    }

    rv_vocoder* self = new (std::nothrow) rv_vocoder;
    if (self == nullptr) {
        return nullptr;
    }
    self->magic = kHandleMagic;
    self->decode_count = 0;
    return self;
}
#endif  // RV_TEST_PLUGIN_REFUSE_CREATE

#ifndef RV_TEST_PLUGIN_OMIT_DECODE
RV_VOCODER_EXPORT std::int32_t RV_VOCODER_CALL revenant_vocoder_decode(rv_vocoder* self,
                                                                       const std::uint8_t* bits,
                                                                       std::uint32_t bit_count,
                                                                       float* out,
                                                                       std::uint32_t out_capacity,
                                                                       std::uint32_t* out_written)
{
    if (out_written != nullptr) {
        *out_written = 0;
    }
    if (self == nullptr || self->magic != kHandleMagic || bits == nullptr || out == nullptr
        || out_written == nullptr) {
        return RV_VOCODER_ERR_ARGUMENT;
    }
    if (bit_count != kBitCount) {
        return RV_VOCODER_ERR_BIT_COUNT;
    }
    if (out_capacity < kPcmFrames) {
        return RV_VOCODER_ERR_CAPACITY;
    }

    // An all-zero frame stands in for one the forward error correction could
    // not repair. It gives the suite a plugin-side failure to drive without
    // any hardware and without a second build of this file.
    bool any_set = false;
    for (std::uint32_t i = 0; i < bit_count; ++i) {
        if (bits[i] != 0) {
            any_set = true;
            break;
        }
    }
    if (!any_set) {
        return RV_VOCODER_ERR_FRAME_REJECTED;
    }

    // Exactly representable in binary32, so the host side can assert equality
    // rather than a tolerance and a wrong sample is a failed test rather than
    // a judgement call.
    for (std::uint32_t i = 0; i < kPcmFrames; ++i) {
        out[i] = (bits[i % bit_count] != 0) ? 0.25f : -0.25f;
    }

    // Sample 0 carries how many frames this handle has decoded since the last
    // reset, which is what makes reset() observable from outside.
    out[0] = static_cast<float>(self->decode_count);
    ++self->decode_count;

#ifdef RV_TEST_PLUGIN_OVERRUN_REPORT
    // Claims eight more samples than the host offered, without writing them.
    // The lie is the thing under test; actually writing past the buffer would
    // corrupt the test process and prove nothing the host could have caught.
    *out_written = out_capacity + 8;
#else
    *out_written = kPcmFrames;
#endif
    return RV_VOCODER_OK;
}
#endif  // RV_TEST_PLUGIN_OMIT_DECODE

RV_VOCODER_EXPORT void RV_VOCODER_CALL revenant_vocoder_reset(rv_vocoder* self)
{
    if (self != nullptr && self->magic == kHandleMagic) {
        self->decode_count = 0;
    }
}

RV_VOCODER_EXPORT void RV_VOCODER_CALL revenant_vocoder_destroy(rv_vocoder* self)
{
    delete self;
}

}  // extern "C"

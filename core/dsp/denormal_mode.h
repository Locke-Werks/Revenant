// Denormal handling, which is a project-wide numerical policy and not a detail.
//
// The first run of the reference diff suite found this the hard way, which is
// what it is for. An RTX 4090 running the complex multiply kernel returned
// exact zero where the CPU reference returned 1.17549435e-38, the smallest
// normal float, and several denormal values besides. Neither was wrong.
// Vulkan leaves the default fp32 denormal behaviour to the implementation,
// and the implementations here flush. The CPU, left alone, preserves them.
//
// WHAT THIS PARAGRAPH USED TO SAY: "GPUs flush denormals to zero in fp32
// compute by default, and Vulkan permits it: shaderDenormFlushToZeroFloat32
// is the behaviour NVIDIA advertises and shaderDenormPreserveFloat32 is not
// offered for fp32 at all, so this cannot be turned off on the device side."
//
// Three things wrong with that, and the third is the one that cost coverage.
//
// The RTX 4090 in this machine advertises NEITHER of those two members. Read
// back from VkPhysicalDeviceFloatControlsProperties on driver 580.97,
// shaderDenormFlushToZeroFloat32 and shaderDenormPreserveFloat32 are both
// false, so the sentence names the wrong one as NVIDIA's behaviour and is
// wrong about the other being unavailable in general.
//
// Neither member could settle it anyway. They say what a SPIR-V execution
// mode may REQUEST of the device, not what the device does when no kernel
// asks, and none of these kernels asks. The flush this policy rests on is an
// implementation default that Vulkan does not specify.
//
// And "GPUs" was one card generalised to every device this project will run
// on, which left the device half of the contract as the only half nothing
// checked. ScopedDenormalFlush is asserted on the CPU by
// tests/reference/test_denormal_mode.cpp. The device side was left to a
// sentence saying there was nothing to check, so nobody checked it.
// tests/reference/gpu_fixture.cpp now measures it: one dispatch of
// core/shaders/cmul.comp with operands whose product is 1e-40, and every GPU
// case fails on a device that hands that denormal back instead of zero.
//
// Two implementations that disagree about denormals cannot be compared
// bit-exactly, and the reference has to model the hardware rather than the
// other way round. So Revenant's policy is that denormals are flushed to zero
// everywhere, and the CPU is brought into line with the GPU.
//
// That is the right policy on its own merits, not only for the diff. A
// denormal float is smaller than 1.2e-38. No signal Revenant will ever receive
// carries information at that magnitude: it is thirty orders of magnitude below
// the noise floor of any real receiver. What denormals do carry is a severe
// performance penalty on x86, where a single denormal operand can cost a
// hundred cycles or more, which is why every serious audio and DSP engine turns
// them off. Flushing them costs nothing real and buys both speed and
// comparability.
//
// This is per-thread state on x86, set in the MXCSR register, so it is scoped
// rather than set once globally: a reference function must produce the same
// answer no matter what mode its caller happened to be in.

#pragma once

namespace revenant::dsp {

// Sets flush-to-zero and denormals-are-zero for the calling thread, restoring
// the previous mode on destruction.
class ScopedDenormalFlush {
public:
    ScopedDenormalFlush();
    ~ScopedDenormalFlush();

    ScopedDenormalFlush(const ScopedDenormalFlush&) = delete;
    ScopedDenormalFlush& operator=(const ScopedDenormalFlush&) = delete;
    ScopedDenormalFlush(ScopedDenormalFlush&&) = delete;
    ScopedDenormalFlush& operator=(ScopedDenormalFlush&&) = delete;

private:
    unsigned int saved_state_ = 0;
    bool restore_ = false;
};

// True when the calling thread is currently flushing denormals. Exposed so a
// test can assert the policy is actually in force rather than trusting that a
// guard somewhere up the stack did its job.
[[nodiscard]] bool denormals_are_flushed();

}  // namespace revenant::dsp

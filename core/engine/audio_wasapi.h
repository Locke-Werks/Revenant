// The sound card, as a Pull backend.
//
// core/engine/audio_egress.h put the Push/Pull split in the interface on the
// first day precisely so that this file could be added without touching it: a
// file wants to be written to, a sound device wants to read from you, and a
// naive write()-only sink has to be rewritten the day WASAPI shows up. This
// is that backend. It takes an AudioTap at attach(), runs its own
// event-driven render thread, and reads the tap on the device's clock.
//
// FOUR DECISIONS, because each one has a wrong answer that works on this
// machine and fails on the next.
//
// Shared mode, not exclusive. Exclusive mode gives lower latency and takes
// the device away from everything else on the system, which for a receiver
// somebody is monitoring alongside a browser and a mail client is the wrong
// trade. Exclusive also refuses any format the hardware does not natively
// accept, which is the second decision's problem.
//
// AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, with SRC_DEFAULT_QUALITY. The engine
// produces mono float32 at its audio rate, typically 48 kHz. A device's mix
// format is whatever the user's control panel says: 44.1 kHz, 96 kHz, two
// channels, sometimes 24-bit. Without the convert flags, shared mode accepts
// only the mix format exactly, so the backend would have to carry a
// resampler and a channel mapper, and that resampler would be a second
// implementation of something core/shaders/vrx_fine.comp already does
// correctly. With them, the engine hands WASAPI its own format and the
// audio engine converts. The conversion is not bit-exact and does not need
// to be: this path ends at a loudspeaker, and the recording path, which is
// the one that has to be exact, is the WAV backend and goes nowhere near
// here.
//
// Event-driven, not timer-driven. A polling render loop either wakes too
// often or underruns, and the sample rate it is chasing is the device's
// rather than the engine's. The event handle is the device telling us it
// wants a buffer, which is the only clock in the building that is allowed to
// set this thread's pace.
//
// The backlog is trimmed, and from this side. A monitor that has been starved
// and then over-filled is holding audio the listener will hear late, and late
// audio is worse than no audio when the point is to hear what the radio is
// doing now. AudioTap::trim_to_frames discards from the read cursor, which is
// the side this thread owns; see the note on trim_to in
// core/engine/spsc_ring.h for why the writer cannot do it.
//
// WHAT close() HAS TO GUARANTEE. The egress layer frees the ring immediately
// after close() returns, so a render callback that fires once more reads
// freed memory. close() therefore stops the stream, joins the render thread
// and only then returns, and it is safe to call twice.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/engine/audio_egress.h"
#include "core/error.h"

namespace revenant::engine {

// One render device, as the operating system describes it.
struct AudioDeviceInfo {
    // Opaque and stable across reboots. This is what goes on a command line
    // or in a session file; the friendly name is for a person and changes
    // when a driver is updated.
    std::string id;

    std::string name;

    // The device the system would use if nothing were specified. Exactly one
    // entry has this set, unless there are no devices at all.
    bool is_default = false;

    // What the audio engine is mixing at right now. Reported because it
    // explains a resampling that is otherwise invisible, not because
    // anything here has to match it.
    dsp::SampleRate mix_rate = 0;
    std::uint32_t mix_channels = 0;
};

// Active render endpoints. Opens nothing, for the same reason
// source::enumerate_sources opens nothing: it still answers on a machine
// where opening would fail, which is when somebody is trying to find out why.
[[nodiscard]] Expected<std::vector<AudioDeviceInfo>> enumerate_audio_devices();

struct WasapiOptions {
    // Empty takes the system default render endpoint, and keeps taking it:
    // see follow_default below.
    std::string device_id;

    // Buffer the device is asked for. This is the floor on monitoring
    // latency and the ceiling on how long this thread may be descheduled
    // before the listener hears it. Shared mode rounds it up to the engine's
    // own period, so asking for less than about 10 ms changes nothing.
    double buffer_ms = 50.0;

    // Audio allowed to accumulate in the ring before the oldest is dropped.
    // Must be at least buffer_ms or the trim fights the device for the
    // samples it is about to ask for. Zero never trims, which is right for a
    // recording and wrong for a monitor.
    double max_backlog_ms = 250.0;

    // Applied on this thread, to this receiver only, as a plain multiply. Not
    // an AGC and not in the DSP path: the demodulators put a fully modulated
    // signal at exactly full scale by convention, and a monitor that wants it
    // quieter should not be changing what the recording contains.
    float gain = 1.0F;

    // When the default endpoint changes, which is what unplugging a headset
    // does, reopen against the new one instead of going silent. Only
    // meaningful when device_id is empty, because a named device that
    // disappears has not been replaced by anything.
    bool follow_default = true;
};

// A backend that plays one receiver.
//
// Fails at open() rather than degrading if the device cannot be had. A
// monitor that silently plays nothing is worse than one that says why,
// because the operator's next move is to start debugging the radio.
[[nodiscard]] Expected<std::unique_ptr<AudioBackend>> make_wasapi_backend(
    const WasapiOptions& options = {});

}  // namespace revenant::engine

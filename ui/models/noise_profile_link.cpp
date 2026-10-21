// EngineLink's side of the adaptive noise profile, core/dsp/noise_profile.h.
// Each setter is a retune in place, the same as the other noise controls in
// models/noise_link.cpp; kept apart because learn and reset are presses, not
// settings, and travel as counters the engine compares with the last it saw.

#include "models/engine_link.h"

#include <algorithm>

#include "core/dsp/noise_profile.h"
#include "models/noise_controls.h"

namespace revenant::ui {

namespace {

[[nodiscard]] bool profile_same(const rpc::VrxParams& a, const rpc::VrxParams& b)
{
    return a.profile_enabled == b.profile_enabled &&
           a.profile_strength_db == b.profile_strength_db &&
           a.profile_auto_learn == b.profile_auto_learn &&
           a.profile_learn_count == b.profile_learn_count &&
           a.profile_reset_count == b.profile_reset_count;
}

}  // namespace

void EngineLink::change_noise_profile(const rpc::VrxParams& next)
{
    if (profile_same(next, wanted_)) {
        return;
    }
    wanted_ = next;
    if (receiverId() == 0) {
        emit receiverChanged();
        return;
    }
    post_receiver_request(false);
}

void EngineLink::setNoiseProfile(bool on)
{
    rpc::VrxParams next = wanted_;
    next.profile_enabled = on && noise_offer(next.demod).reduction;
    change_noise_profile(next);
}

void EngineLink::setNoiseProfileStrengthDb(double db)
{
    rpc::VrxParams next = wanted_;
    next.profile_strength_db = std::clamp(db, 0.0, dsp::kNoiseProfileMaxStrengthDb);
    change_noise_profile(next);
}

void EngineLink::setNoiseProfileAutoLearn(bool on)
{
    rpc::VrxParams next = wanted_;
    next.profile_auto_learn = on;
    change_noise_profile(next);
}

void EngineLink::learnNoiseProfile()
{
    rpc::VrxParams next = wanted_;
    ++next.profile_learn_count;
    change_noise_profile(next);
}

void EngineLink::resetNoiseProfile()
{
    rpc::VrxParams next = wanted_;
    ++next.profile_reset_count;
    change_noise_profile(next);
}

}  // namespace revenant::ui

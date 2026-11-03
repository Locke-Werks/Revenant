#include "tools/webgate/session.h"

#include <utility>

#include "tools/webgate/dispatch.h"

namespace revenant::webgate {

PhoneSession::PhoneSession(std::unique_ptr<rpc::Client> client) : client_(std::move(client)) {}

Status PhoneSession::remove_receiver(std::uint64_t vrx)
{
    return hooks_.remove_vrx ? hooks_.remove_vrx(vrx) : client_->remove_vrx(vrx);
}

Expected<rpc::VrxStatus> PhoneSession::receiver_status(std::uint64_t vrx)
{
    return hooks_.vrx_status ? hooks_.vrx_status(vrx) : client_->vrx_status(vrx);
}

std::vector<std::uint64_t> PhoneSession::receiver_ids() const
{
    std::vector<std::uint64_t> ids;
    ids.reserve(receivers_.size());
    for (const auto& [id, known] : receivers_) {
        ids.push_back(id);
    }
    return ids;
}

const rpc::VrxParams* PhoneSession::last_params(std::uint64_t vrx) const
{
    const auto it = receivers_.find(vrx);
    return it == receivers_.end() ? nullptr : &it->second.params;
}

void PhoneSession::learn(std::uint64_t vrx, const rpc::VrxParams& params, bool opened_here)
{
    receivers_[vrx] = Known{params, opened_here};
}

bool PhoneSession::recreatable(std::uint64_t vrx) const
{
    const auto it = receivers_.find(vrx);
    return it != receivers_.end() && it->second.opened_here;
}

void PhoneSession::remember(std::uint64_t vrx, const rpc::VrxParams& params)
{
    const auto it = receivers_.find(vrx);
    if (it != receivers_.end()) {
        it->second.params = params;
    }
}

void PhoneSession::forget(std::uint64_t vrx)
{
    receivers_.erase(vrx);
    transcribe_.erase(vrx);
    decoders_.erase(vrx);
    if (audio_vrx_ == vrx) {
        audio_vrx_.reset();
    }
}

rpc::TranscribeChoice PhoneSession::transcribe_choice(std::uint64_t vrx) const
{
    const auto it = transcribe_.find(vrx);
    return it == transcribe_.end() ? rpc::TranscribeChoice::Auto : it->second;
}

std::vector<std::string> PhoneSession::decoders(std::uint64_t vrx) const
{
    if (const auto* chosen = decoder_choice(vrx)) {
        return *chosen;
    }
    const auto* params = last_params(vrx);
    if (params == nullptr) {
        return {};
    }
    return default_decoders(params->demod);
}

std::vector<std::string> PhoneSession::default_decoders(rpc::Demod demod) const
{
    // The decoder named after the mode whenever the engine has one (cw,
    // p25p1, dstar, tetra, dmr, and any added later), else none. The engine's
    // list is read once and kept; a failed read is tried again next time.
    if (!engine_decoders_) {
        const auto known = client_->decoders();
        if (!known) {
            return {};
        }
        std::vector<std::string> names;
        for (const auto& d : *known) {
            names.push_back(d.name);
        }
        engine_decoders_ = std::move(names);
    }
    const std::string mode(demod_name(demod));
    for (const auto& name : *engine_decoders_) {
        if (name == mode) {
            return {mode};
        }
    }
    return {};
}

const std::vector<std::string>* PhoneSession::decoder_choice(std::uint64_t vrx) const
{
    const auto it = decoders_.find(vrx);
    return it == decoders_.end() ? nullptr : &it->second;
}

void PhoneSession::set_decoders(std::uint64_t vrx, std::vector<std::string> names)
{
    if (receivers_.contains(vrx)) {
        decoders_[vrx] = std::move(names);
    }
}

void PhoneSession::set_transcribe_choice(std::uint64_t vrx, rpc::TranscribeChoice choice)
{
    if (receivers_.contains(vrx)) {
        transcribe_[vrx] = choice;
    }
}

}  // namespace revenant::webgate

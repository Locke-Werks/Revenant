#include "tools/webgate/session.h"

#include <utility>

namespace revenant::webgate {

PhoneSession::PhoneSession(std::unique_ptr<rpc::Client> client) : client_(std::move(client)) {}

PhoneSession::~PhoneSession()
{
    if (!client_) {
        return;
    }
    for (const auto& [id, params] : owned_) {
        (void)client_->remove_vrx(id);
    }
}

Status PhoneSession::remove_receiver(std::uint64_t vrx)
{
    return hooks_.remove_vrx ? hooks_.remove_vrx(vrx) : client_->remove_vrx(vrx);
}

Expected<rpc::VrxStatus> PhoneSession::receiver_status(std::uint64_t vrx)
{
    return hooks_.vrx_status ? hooks_.vrx_status(vrx) : client_->vrx_status(vrx);
}

std::vector<std::uint64_t> PhoneSession::owned_ids() const
{
    std::vector<std::uint64_t> ids;
    ids.reserve(owned_.size());
    for (const auto& [id, params] : owned_) {
        ids.push_back(id);
    }
    return ids;
}

const rpc::VrxParams* PhoneSession::last_params(std::uint64_t vrx) const
{
    const auto it = owned_.find(vrx);
    return it == owned_.end() ? nullptr : &it->second;
}

void PhoneSession::adopt(std::uint64_t vrx, const rpc::VrxParams& params)
{
    owned_[vrx] = params;
}

void PhoneSession::remember(std::uint64_t vrx, const rpc::VrxParams& params)
{
    const auto it = owned_.find(vrx);
    if (it != owned_.end()) {
        it->second = params;
    }
}

void PhoneSession::forget(std::uint64_t vrx)
{
    owned_.erase(vrx);
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
    // Each digital voice mode has a decoder of the same name in the engine.
    switch (params->demod) {
    case rpc::Demod::P25p1:
        return {"p25p1"};
    case rpc::Demod::Dstar:
        return {"dstar"};
    case rpc::Demod::Tetra:
        return {"tetra"};
    case rpc::Demod::Dmr:
        return {"dmr"};
    default:
        return {};
    }
}

const std::vector<std::string>* PhoneSession::decoder_choice(std::uint64_t vrx) const
{
    const auto it = decoders_.find(vrx);
    return it == decoders_.end() ? nullptr : &it->second;
}

void PhoneSession::set_decoders(std::uint64_t vrx, std::vector<std::string> names)
{
    if (owned_.contains(vrx)) {
        decoders_[vrx] = std::move(names);
    }
}

void PhoneSession::set_transcribe_choice(std::uint64_t vrx, rpc::TranscribeChoice choice)
{
    if (owned_.contains(vrx)) {
        transcribe_[vrx] = choice;
    }
}

}  // namespace revenant::webgate

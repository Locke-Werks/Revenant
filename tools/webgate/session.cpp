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
    if (audio_vrx_ == vrx) {
        audio_vrx_.reset();
    }
}

}  // namespace revenant::webgate

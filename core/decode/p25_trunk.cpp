#include "core/decode/p25_trunk.h"

#include <variant>

namespace revenant::decode {

std::optional<std::uint64_t> P25ChannelTable::frequency_hz(const P25Channel& channel) const {
    const auto& iden = entries_[channel.identifier & 0x0FU];
    if (!iden) {
        return std::nullopt;
    }
    return p25_channel_frequency_hz(*iden, channel);
}

std::vector<P25TrunkGrant> P25TrunkReader::read(const P25Tsbk& tsbk) {
    std::vector<P25TrunkGrant> out;
    const P25TsbkMessage parsed = p25_parse_tsbk(tsbk);

    const auto with_options = [](P25TrunkGrant& grant, const P25ServiceOptions& options) {
        grant.options_known = true;
        grant.encrypted = options.protected_mode();
        grant.emergency = options.emergency();
        grant.priority = options.priority();
    };

    if (const auto* iden = std::get_if<P25IdentifierUpdate>(&parsed)) {
        table_.learn(*iden);
    } else if (const auto* grant = std::get_if<P25GroupVoiceGrant>(&parsed)) {
        P25TrunkGrant g;
        g.group = grant->group_address;
        g.source = grant->source_address;
        g.channel = grant->channel;
        g.frequency_hz = table_.frequency_hz(grant->channel);
        g.initial = true;
        with_options(g, grant->options);
        out.push_back(g);
    } else if (const auto* update = std::get_if<P25GroupVoiceGrantUpdate>(&parsed)) {
        for (std::size_t i = 0; i < 2; ++i) {
            P25TrunkGrant g;
            g.group = update->group_address[i];
            g.channel = update->channel[i];
            g.frequency_hz = table_.frequency_hz(update->channel[i]);
            // A control channel with one call to update can send the same
            // pair twice; one grant is enough. An engineering choice.
            if (i == 1 && g.group == out.front().group && g.channel == out.front().channel) {
                break;
            }
            out.push_back(g);
        }
    } else if (const auto* exp = std::get_if<P25GroupVoiceGrantUpdateExplicit>(&parsed)) {
        P25TrunkGrant g;
        g.group = exp->group_address;
        g.channel = exp->receive;
        g.frequency_hz = table_.frequency_hz(exp->receive);
        with_options(g, exp->options);
        out.push_back(g);
    }
    return out;
}

}  // namespace revenant::decode

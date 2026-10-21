// P25 Phase 1 trunking: the voice grants a control channel sends, resolved to
// the frequencies a follower tunes.
//
// SPECIFICATION
//
// TIA-102.AABC-B, "Trunking Control Channel Messages", the edition
// core/decode/p25_tsbk.h cites and from the same archive: Table 4.2-1 and
// clauses 4.2.1 to 4.2.3 (GRP_V_CH_GRANT, GRP_V_CH_GRANT_UPDT and
// GRP_V_CH_GRANT_UPDT_EXP), clause 2.3.9.2 (a channel's frequency from its
// identifier's base and spacing), and clauses 6.2.9 and 6.2.29 (IDEN_UP and
// IDEN_UP_VU, which carry those). p25_tsbk.h parses every one of these; this
// file adds the table that remembers identifiers across blocks and the one
// shape a grant of any of the three opcodes is reduced to.
//
// WHICH CHANNEL OF AN EXPLICIT UPDATE. GRP_V_CH_GRANT_UPDT_EXP carries
// Channel (T) and Channel (R), the subscriber's transmit and receive
// channels (AABC-B clause 4.2.3). A receiver listens where the subscriber
// does, so the grant here is Channel (R), resolved with clause 2.3.9.2 like
// any other. plugins/p25trunk follows Channel (T) and says it never checked
// that reading; this file reads the figure.
//
// WHAT THIS DOES NOT DO
//
// It does not decrypt, and it does not read past a protected block or a
// non-standard MFID, for the reasons p25_tsbk.h gives. A grant's encrypted
// flag is Service Options bit 6 (AABC-B clause 2.3.24), reported and never
// acted on beyond declining to play the call.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/decode/p25_tsbk.h"

namespace revenant::decode {

// The sixteen identifiers a control channel may define, AABC-B clause
// 2.3.19, as the most recent IDEN_UP or IDEN_UP_VU for each.
class P25ChannelTable {
public:
    void learn(const P25IdentifierUpdate& iden) { entries_[iden.identifier & 0x0FU] = iden; }
    void clear() { entries_ = {}; }

    [[nodiscard]] bool knows(std::uint8_t identifier) const {
        return entries_[identifier & 0x0FU].has_value();
    }
    [[nodiscard]] std::optional<std::uint64_t> frequency_hz(const P25Channel& channel) const;

private:
    std::array<std::optional<P25IdentifierUpdate>, 16> entries_{};
};

// One talkgroup on one channel, from whichever of the three opcodes said so.
struct P25TrunkGrant {
    std::uint16_t group = 0;
    // GRP_V_CH_GRANT names the talker (AABC-B clause 2.3.27); the updates do
    // not.
    std::optional<std::uint32_t> source;
    P25Channel channel;
    // Empty until the control channel has sent the channel's identifier.
    std::optional<std::uint64_t> frequency_hz;
    // Service Options, clause 2.3.24. Only GRP_V_CH_GRANT and the explicit
    // update carry them; for GRP_V_CH_GRANT_UPDT options_known is false and
    // the three below are false and zero, not a claim the call is clear.
    bool options_known = false;
    bool encrypted = false;
    bool emergency = false;
    std::uint8_t priority = 0;
    // Opcode $00, the grant that starts a call, as against an update
    // repeating one already in progress.
    bool initial = false;
};

// Reads one control channel's TSBKs in order: identifier updates go into
// the table, voice grants come out. Anything else, a failed CRC included,
// yields nothing.
class P25TrunkReader {
public:
    [[nodiscard]] std::vector<P25TrunkGrant> read(const P25Tsbk& tsbk);

    // A different control channel, or the same one after its decoder
    // restarted: identifiers learned before may not hold.
    void reset() { table_.clear(); }

    [[nodiscard]] const P25ChannelTable& table() const { return table_; }

private:
    P25ChannelTable table_;
};

}  // namespace revenant::decode

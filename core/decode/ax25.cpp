#include "core/decode/ax25.h"

#include <algorithm>

namespace revenant::decode {

namespace {

// Finnegan and Benson 2014, Figure 5: the CRC-CCITT generator 0x1021 with
// its bits reversed, because the register shifts right and takes each octet
// least significant bit first.
constexpr std::uint16_t kReflectedCcittGenerator = 0x8408;

bool call_sign_character(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

Expected<Ax25Address> decode_address(std::span<const std::uint8_t> octets) {
    // Clause 3.12: the call sign occupies the left-most seven bits of each of
    // the first six octets, bit 0 being the extension bit.
    Ax25Address address;
    bool padding = false;
    for (std::size_t i = 0; i < 6; ++i) {
        const auto c = static_cast<char>(octets[i] >> 1U);
        if (c == ' ') {
            padding = true;
            continue;
        }
        if (padding || !call_sign_character(c)) {
            return fail("an AX.25 call sign holds a character clause 3.12 does not allow");
        }
        address.callsign.push_back(c);
    }
    if (address.callsign.empty()) {
        return fail("an AX.25 address subfield holds no call sign");
    }
    // Figures 3.5 and 3.7: the SSID octet is CRRSSSS0, or HRRSSSS1 on the
    // last subfield.
    const std::uint8_t s = octets[6];
    address.ssid = static_cast<std::uint8_t>((s >> 1U) & 0x0FU);
    address.reserved = static_cast<std::uint8_t>((s >> 5U) & 0x03U);
    address.command_or_repeated = (s & 0x80U) != 0U;
    return address;
}

}  // namespace

std::uint16_t ax25_fcs(std::span<const std::uint8_t> octets) {
    std::uint16_t crc = 0xFFFF;
    for (const std::uint8_t octet : octets) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const unsigned in = (octet >> bit) & 1U;
            if ((crc & 1U) != in) {
                crc = static_cast<std::uint16_t>((crc >> 1U) ^ kReflectedCcittGenerator);
            } else {
                crc = static_cast<std::uint16_t>(crc >> 1U);
            }
        }
    }
    return static_cast<std::uint16_t>(crc ^ 0xFFFFU);
}

Expected<std::array<std::uint8_t, kAx25AddressOctets>> ax25_encode_address(
    const Ax25Address& address, bool last) {
    if (address.callsign.empty() || address.callsign.size() > 6) {
        return fail("an AX.25 call sign is one to six characters");
    }
    for (const char c : address.callsign) {
        if (!call_sign_character(c)) {
            return fail("an AX.25 call sign is upper-case letters and digits only");
        }
    }
    if (address.ssid > 15 || address.reserved > 3) {
        return fail("an AX.25 SSID is four bits and the reserved field two");
    }

    std::array<std::uint8_t, kAx25AddressOctets> out{};
    for (std::size_t i = 0; i < 6; ++i) {
        const char c = (i < address.callsign.size()) ? address.callsign[i] : ' ';
        out[i] = static_cast<std::uint8_t>(static_cast<unsigned>(c) << 1U);
    }
    out[6] =
        static_cast<std::uint8_t>((address.command_or_repeated ? 0x80U : 0U) |
                                  (static_cast<unsigned>(address.reserved) << 5U) |
                                  (static_cast<unsigned>(address.ssid) << 1U) | (last ? 1U : 0U));
    return out;
}

Expected<Ax25Frame> ax25_parse(std::span<const std::uint8_t> octets) {
    // Destination and source, clause 3.12.1, then the control octet.
    if (octets.size() < 2 * kAx25AddressOctets + 1) {
        return fail("an AX.25 frame is shorter than two address subfields and a control field");
    }

    Ax25Frame frame;
    std::size_t at = 0;
    std::size_t subfield = 0;
    for (;;) {
        if (at + kAx25AddressOctets > octets.size()) {
            return fail("an AX.25 address field runs off the end of the frame");
        }
        const auto chunk = octets.subspan(at, kAx25AddressOctets);
        auto address = decode_address(chunk);
        if (!address) {
            return std::unexpected(address.error());
        }
        if (subfield == 0) {
            frame.destination = std::move(*address);
        } else if (subfield == 1) {
            frame.source = std::move(*address);
        } else {
            frame.repeaters.push_back(std::move(*address));
        }
        at += kAx25AddressOctets;
        ++subfield;
        const bool last = (chunk[6] & 1U) != 0U;
        if (last) {
            break;
        }
        if (subfield == 2 + kAx25MaximumRepeaters) {
            return fail("an AX.25 address field holds more repeaters than this decoder accepts");
        }
    }
    if (subfield < 2) {
        return fail("an AX.25 address field ends before the source subfield");
    }
    if (at >= octets.size()) {
        return fail("an AX.25 frame has no control field");
    }

    // Figure 4.1a.
    frame.control = octets[at++];
    if ((frame.control & 0x01U) == 0U) {
        frame.kind = Ax25FrameKind::Information;
    } else if ((frame.control & 0x03U) == 0x01U) {
        frame.kind = Ax25FrameKind::Supervisory;
    } else {
        frame.kind = Ax25FrameKind::Unnumbered;
    }
    frame.is_ui = (frame.control & static_cast<std::uint8_t>(~kAx25PollFinalBit)) == kAx25ControlUi;

    // Clause 3.4: the PID appears in I and UI frames only.
    if (frame.kind == Ax25FrameKind::Information || frame.is_ui) {
        if (at >= octets.size()) {
            return fail("an AX.25 I or UI frame has no PID field");
        }
        frame.pid = octets[at++];
    }

    frame.information.assign(octets.begin() + static_cast<std::ptrdiff_t>(at), octets.end());
    frame.octets.assign(octets.begin(), octets.end());
    return frame;
}

std::string ax25_address_text(const Ax25Address& address) {
    std::string text = address.callsign;
    if (address.ssid != 0) {
        text += '-';
        text += std::to_string(address.ssid);
    }
    return text;
}

// ---------------------------------------------------------------------------
// HdlcDeframer
// ---------------------------------------------------------------------------

void HdlcDeframer::reset() {
    bits_.clear();
    first_sample_ = 0;
    ones_ = 0;
    in_frame_ = false;
    run_start_ = 0;
    awaiting_first_ = false;
}

void HdlcDeframer::push(std::uint8_t bit, SampleIndex sample, std::vector<HdlcFrame>& out) {
    if (bit != 0U) {
        ++ones_;
        if (ones_ == kAx25AbortOnes) {
            // Clause 3.10. Nothing that follows belongs to the frame that
            // was in progress; the next flag starts afresh.
            in_frame_ = false;
            bits_.clear();
        }
        if (in_frame_) {
            bits_.push_back(1);
            if (awaiting_first_) {
                first_sample_ = sample;
                awaiting_first_ = false;
            }
        }
        return;
    }

    const int run = ones_;
    ones_ = 0;

    if (run == kAx25StuffAfterOnes + 1) {
        // Six ones and this zero complete a flag, clause 3.1. The six ones
        // and the zero before them went into bits_ as data; take them back
        // out. The zero before them is the flag's own leading zero, never a
        // stuffed one, because a transmitter stuffs after five ones of data
        // and then sends the whole flag.
        if (in_frame_) {
            bits_.resize(run_start_ > 0 ? run_start_ - 1 : 0);
            // Clause 3.9: whole octets, and at least the minimum length.
            if (bits_.size() % 8 == 0 && bits_.size() / 8 >= kAx25MinimumContentOctets) {
                HdlcFrame frame;
                frame.octets.assign(bits_.size() / 8, 0);
                for (std::size_t i = 0; i < bits_.size(); ++i) {
                    // Clause 3.8: least significant bit first.
                    frame.octets[i / 8] =
                        static_cast<std::uint8_t>(frame.octets[i / 8] | (bits_[i] << (i % 8)));
                }
                frame.first_sample = first_sample_;
                frame.last_sample = sample;
                out.push_back(std::move(frame));
            }
        }
        in_frame_ = true;
        bits_.clear();
        run_start_ = 0;
        awaiting_first_ = true;
        return;
    }

    if (run >= kAx25AbortOnes) {
        // The zero that ends an abort or an idle run of ones. Still out of a
        // frame until a flag.
        return;
    }

    if (run == kAx25StuffAfterOnes) {
        // Clause 3.6: the zero after five ones is discarded.
        run_start_ = bits_.size();
        return;
    }

    if (in_frame_) {
        bits_.push_back(0);
        if (awaiting_first_) {
            first_sample_ = sample;
            awaiting_first_ = false;
        }
    }
    run_start_ = bits_.size();
}

// ---------------------------------------------------------------------------
// Ax25Decoder
// ---------------------------------------------------------------------------

Expected<Ax25Decoder> Ax25Decoder::create(const Ax25Config& config) {
    ToneDiscriminatorConfig tones;
    tones.rate = config.rate;
    tones.mark_hz = config.mark_hz;
    tones.space_hz = config.space_hz;
    tones.symbol_rate = config.baud;
    auto discriminator = ToneDiscriminator::create(tones);
    if (!discriminator) {
        return std::unexpected(with_context(discriminator.error(), "AX.25"));
    }

    BitClockConfig clock;
    clock.rate = config.rate;
    clock.symbol_rate = config.baud;
    auto bit_clock = BitClock::create(clock);
    if (!bit_clock) {
        return std::unexpected(with_context(bit_clock.error(), "AX.25"));
    }

    Ax25Decoder d;
    d.discriminator_ = std::move(*discriminator);
    d.clock_ = std::move(*bit_clock);
    return d;
}

void Ax25Decoder::reset() {
    discriminator_.reset();
    clock_.reset();
    deframer_.reset();
    previous_level_ = false;
    stats_ = {};
}

void Ax25Decoder::process(ConstRealSpan audio, std::vector<Ax25Frame>& out) {
    soft_.clear();
    bits_.clear();
    frames_.clear();
    discriminator_.process(audio, soft_);
    clock_.process(soft_, bits_);

    const auto delay = static_cast<SampleIndex>(discriminator_.group_delay());
    for (const SoftBit& b : bits_) {
        // NRZI, Finnegan and Benson section 2: no change of tone is a one.
        const bool level = b.value >= 0.0F;
        const std::uint8_t bit = (level == previous_level_) ? 1U : 0U;
        previous_level_ = level;
        const SampleIndex sample = b.position > delay ? b.position - delay : 0;
        deframer_.push(bit, sample, frames_);
    }

    for (const HdlcFrame& f : frames_) {
        ++stats_.candidates;
        const std::size_t n = f.octets.size();
        const auto content = std::span<const std::uint8_t>(f.octets).first(n - 2);
        const auto received = static_cast<std::uint16_t>(
            f.octets[n - 2] | (static_cast<unsigned>(f.octets[n - 1]) << 8U));
        if (ax25_fcs(content) != received) {
            ++stats_.fcs_failures;
            continue;
        }
        auto frame = ax25_parse(content);
        if (!frame) {
            ++stats_.malformed;
            continue;
        }
        frame->first_sample = f.first_sample;
        frame->last_sample = f.last_sample;
        out.push_back(std::move(*frame));
    }
}

}  // namespace revenant::decode

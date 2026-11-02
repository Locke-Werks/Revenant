#include "core/dsp/synth/gk2a_lrit_mod.h"

namespace revenant::siggen {

namespace {

decode::XritHeaderRecord text_record(std::uint8_t type, const std::string& s) {
    return {type, std::vector<std::uint8_t>(s.begin(), s.end())};
}

}  // namespace

std::vector<std::uint8_t> gk2a_image_file(const std::string& annotation, std::uint16_t columns, std::uint16_t lines,
                                          const std::vector<std::uint8_t>& pixels, decode::Gk2aSegment segment) {
    std::vector<decode::XritHeaderRecord> r;
    r.push_back({1,
                 {8, static_cast<std::uint8_t>(columns >> 8U), static_cast<std::uint8_t>(columns),
                  static_cast<std::uint8_t>(lines >> 8U), static_cast<std::uint8_t>(lines), 0}});
    r.push_back(text_record(4, annotation));
    r.push_back({128,
                 {segment.sequence, segment.total, static_cast<std::uint8_t>(segment.first_line >> 8U),
                  static_cast<std::uint8_t>(segment.first_line)}});
    return xrit_file_bytes(0, r, pixels);
}

std::vector<std::uint8_t> gk2a_text_file(const std::string& annotation, const std::string& text) {
    return xrit_file_bytes(2, {text_record(4, annotation)}, std::vector<std::uint8_t>(text.begin(), text.end()));
}

XritBpskSpec gk2a_bpsk_spec(double sample_rate) {
    XritBpskSpec s;
    s.sample_rate = sample_rate;
    s.symbol_rate = decode::kGk2aLritSymbolRate;
    s.roll_off = decode::kGk2aLritRollOff;
    s.spacecraft_id = 0x2A;  // arbitrary: GK2A table 8.4 gives "XXh"
    return s;
}

}  // namespace revenant::siggen

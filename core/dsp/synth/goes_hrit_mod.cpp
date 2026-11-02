#include "core/dsp/synth/goes_hrit_mod.h"

namespace revenant::siggen {

decode::XritHeaderRecord noaa_record(const decode::NoaaHeader& h) {
    decode::XritHeaderRecord r{129, {}};
    for (std::size_t i = 0; i < 4; ++i) {
        r.data.push_back(i < h.agency.size() ? static_cast<std::uint8_t>(h.agency[i]) : 0);
    }
    for (const std::uint16_t v : {h.product_id, h.subproduct_id, h.parameter}) {
        r.data.push_back(static_cast<std::uint8_t>(v >> 8U));
        r.data.push_back(static_cast<std::uint8_t>(v));
    }
    r.data.push_back(h.compression);
    return r;
}

std::vector<std::uint8_t> emwin_text_file(const std::string& filename, const std::string& bulletin) {
    decode::NoaaHeader n;
    n.agency = "NOAA";
    n.product_id = decode::kNoaaProductEmwin;
    return xrit_file_bytes(2, {{4, std::vector<std::uint8_t>(filename.begin(), filename.end())}, noaa_record(n)},
                           std::vector<std::uint8_t>(bulletin.begin(), bulletin.end()));
}

std::vector<std::uint8_t> goes_image_file(const std::string& annotation, std::uint16_t columns, std::uint16_t lines,
                                          const std::vector<std::uint8_t>& pixels) {
    return xrit_file_bytes(0,
                           {{1,
                             {8, static_cast<std::uint8_t>(columns >> 8U), static_cast<std::uint8_t>(columns),
                              static_cast<std::uint8_t>(lines >> 8U), static_cast<std::uint8_t>(lines), 0}},
                            {4, std::vector<std::uint8_t>(annotation.begin(), annotation.end())}},
                           pixels);
}

XritBpskSpec goes_hrit_bpsk_spec(double sample_rate) {
    XritBpskSpec s;
    s.sample_rate = sample_rate;
    s.symbol_rate = decode::kGoesHritSymbolRate;
    s.roll_off = decode::kGoesHritRollOff;
    s.spacecraft_id = 0x10;  // arbitrary
    return s;
}

}  // namespace revenant::siggen

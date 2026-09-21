// EN 50067 Annex E bytes turned into UTF-8, and the placeholders that keep
// a partly received message from reading as a complete one.
//
// THE MAPPING, DECIDED HERE AND STATED HERE
//
// core/rpc/types.h is explicit that RdsStation::ps and ::rt are Annex E code
// points, that the repertoire is not ASCII above 0x7F and is not UTF-8
// anywhere, and that transcoding belongs where a font is being chosen. This
// is that place. What was decided:
//
//   0x00..0x1F  RENDERED AS NOTHING. These are not characters. 0x0D is the
//               RadioText terminator and RdsStation::rt_length already says
//               where it is; 0x0A and 0x0B are used by some encoders as
//               line break and end-of-headline. Dropping them is the only
//               answer that cannot put a control code into a QString.
//
//   0x20..0x7E  ASCII, one for one. The Annex E G0 columns 2 to 7 hold the
//               ASCII graphic set at the ASCII positions.
//
//   0x7F        RENDERED AS NOTHING, on the same terms as the C0 set.
//
//   0x80..0xFF  The Annex E table below, to the Unicode code point for each
//               glyph. This is the half that is not Latin-1 and is the
//               reason this file exists: 0xE1 is á in Latin-1 and Ã in
//               Annex E, so a byte handed to QString::fromLatin1 renders a
//               different letter rather than a broken one, and nothing
//               faults.
//
//   unmapped    U+FFFD REPLACEMENT CHARACTER. Every position in the table
//               below is filled, so this is unreachable today. It is the
//               behaviour a gap would get rather than a silent space,
//               because a missing glyph an operator can see is worth more
//               than a message that looks complete.
//
// HOW THE RESULT REACHES Qt. As UTF-8, through QString::fromStdString,
// which is a UTF-8 conversion. Nothing on this path calls fromLatin1 or
// assigns a char to a QChar, which are the two ways the accident happens.
//
// THE PLACEHOLDER, WHICH IS NOT COSMETIC
//
// PS arrives as four two-character segments and RadioText as sixteen
// segments of four, or of two on a 2B station. A segment that has not
// arrived holds nothing, and nothing is indistinguishable from a
// transmitted space: "KKFM" half received is "KK" followed by two bytes
// that were never sent, and a display that renders them as spaces shows
// "KK" as a complete call sign. So an unreceived character is drawn as
// U+00B7 MIDDLE DOT, which is not in the Annex E repertoire anywhere and
// therefore cannot be a transmitted character, and the count of segments
// received is reported beside the text.
//
// WHY THIS HOLDS NO Qt. ui/tests links it. The table, the segment masks and
// the trimming are the parts that can be wrong, and all three are pure
// functions of bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace revenant::ui {

// U+00B7. Not in Annex E, so it cannot collide with a transmitted glyph.
inline constexpr char32_t kRdsPlaceholder = 0x00B7;

// The upper half of the Annex E repertoire, 0x80 to 0xFF, as Unicode code
// points. Index is byte - 0x80.
//
// Zero means no glyph at that position and renders as U+FFFD. There are
// none today; the branch is kept because a table that silently rendered a
// gap as a space is the failure this whole file is about.
inline constexpr char32_t kAnnexEUpper[128] = {
    // 0x80  á à é è í ì ó ò ú ù Ñ Ç Ş β ¡ Ĳ
    0x00E1, 0x00E0, 0x00E9, 0x00E8, 0x00ED, 0x00EC, 0x00F3, 0x00F2,
    0x00FA, 0x00F9, 0x00D1, 0x00C7, 0x015E, 0x00DF, 0x00A1, 0x0132,
    // 0x90  â ä ê ë î ï ô ö û ü ñ ç ş ğ ı ĳ
    0x00E2, 0x00E4, 0x00EA, 0x00EB, 0x00EE, 0x00EF, 0x00F4, 0x00F6,
    0x00FB, 0x00FC, 0x00F1, 0x00E7, 0x015F, 0x011F, 0x0131, 0x0133,
    // 0xA0  ª α © ‰ Ğ ě ň ő π € £ $ ← ↑ → ↓
    0x00AA, 0x03B1, 0x00A9, 0x2030, 0x011E, 0x011B, 0x0148, 0x0151,
    0x03C0, 0x20AC, 0x00A3, 0x0024, 0x2190, 0x2191, 0x2192, 0x2193,
    // 0xB0  º ¹ ² ³ ± İ ń ű µ ¿ ÷ ° ¼ ½ ¾ §
    0x00BA, 0x00B9, 0x00B2, 0x00B3, 0x00B1, 0x0130, 0x0144, 0x0171,
    0x00B5, 0x00BF, 0x00F7, 0x00B0, 0x00BC, 0x00BD, 0x00BE, 0x00A7,
    // 0xC0  Á À É È Í Ì Ó Ò Ú Ù Ř Č Š Ž Ð Ŀ
    0x00C1, 0x00C0, 0x00C9, 0x00C8, 0x00CD, 0x00CC, 0x00D3, 0x00D2,
    0x00DA, 0x00D9, 0x0158, 0x010C, 0x0160, 0x017D, 0x00D0, 0x013F,
    // 0xD0  Â Ä Ê Ë Î Ï Ô Ö Û Ü ř č š ž đ ŀ
    0x00C2, 0x00C4, 0x00CA, 0x00CB, 0x00CE, 0x00CF, 0x00D4, 0x00D6,
    0x00DB, 0x00DC, 0x0159, 0x010D, 0x0161, 0x017E, 0x0111, 0x0140,
    // 0xE0  Ã Å Æ Œ ŷ Ý Õ Ø Þ Ŋ Ŕ Ć Ś Ź Ŧ ð
    0x00C3, 0x00C5, 0x00C6, 0x0152, 0x0177, 0x00DD, 0x00D5, 0x00D8,
    0x00DE, 0x014A, 0x0154, 0x0106, 0x015A, 0x0179, 0x0166, 0x00F0,
    // 0xF0  ã å æ œ ŵ ý õ ø þ ŋ ŕ ć ś ź ŧ  (0xFF: no break space)
    0x00E3, 0x00E5, 0x00E6, 0x0153, 0x0175, 0x00FD, 0x00F5, 0x00F8,
    0x00FE, 0x014B, 0x0155, 0x0107, 0x015B, 0x017A, 0x0167, 0x00A0,
};

// One code point as UTF-8, appended. Written out rather than pulled from a
// library because this file deliberately links nothing: the three lengths
// it can produce are all that Annex E reaches.
inline void append_utf8(std::string& out, char32_t code_point)
{
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

// One Annex E byte to a code point, or zero for the positions that render
// as nothing at all.
[[nodiscard]] inline char32_t annex_e_code_point(std::uint8_t byte)
{
    if (byte < 0x20 || byte == 0x7F) {
        return 0;
    }
    if (byte < 0x80) {
        return static_cast<char32_t>(byte);
    }
    const char32_t mapped = kAnnexEUpper[byte - 0x80];
    return mapped == 0 ? 0xFFFD : mapped;
}

// One byte to UTF-8, or an empty string for a control code.
[[nodiscard]] inline std::string annex_e_to_utf8(std::uint8_t byte)
{
    std::string out;
    const char32_t code_point = annex_e_code_point(byte);
    if (code_point != 0) {
        append_utf8(out, code_point);
    }
    return out;
}

// What a rendered field is, beyond its text.
struct RdsText {
    // UTF-8, with unreceived characters shown as the placeholder and
    // trailing transmitted spaces removed.
    std::string text;

    // Segments the mask says have arrived, and how many there are in
    // total. A display prints these when they differ, because "KKFM" and
    // "KK··" are not the same claim and the second one is easy to read
    // past on a narrow strip.
    int segments_received = 0;
    int segments_total = 0;

    // Nothing at all has arrived. Different from an empty text, which a
    // station transmitting eight spaces also produces.
    [[nodiscard]] bool empty() const { return segments_received == 0; }
};

// Bytes plus the segment mask, rendered.
//
// length is how many characters of `bytes` are part of the message: eight
// for PS, and RdsStation::rt_length for RadioText, which is where the
// terminator was or how far the message has been seen to reach. Anything
// past it is not drawn.
//
// chars_per_segment is two for PS, four for RadioText from 2A groups and
// two for RadioText from 2B groups. RdsStation::rt_version_b is what says
// which, and getting it wrong misplaces every placeholder rather than
// failing, which is why it is a parameter and not a constant.
//
// TRAILING SPACES GO AND PLACEHOLDERS STAY. A station pads PS with spaces
// to eight characters and pads RadioText the same way, so the trailing run
// is padding rather than content. An unreceived tail is not padding: it is
// the part that has not arrived, and trimming it would turn a half-received
// name into a complete short one, which is the exact failure the
// placeholder exists to prevent.
[[nodiscard]] inline RdsText render_rds_text(std::span<const std::uint8_t> bytes,
                                             std::uint32_t received_mask,
                                             std::size_t chars_per_segment,
                                             std::size_t length)
{
    RdsText out;
    if (chars_per_segment == 0) {
        return out;
    }
    if (length > bytes.size()) {
        length = bytes.size();
    }

    out.segments_total = static_cast<int>((length + chars_per_segment - 1) /
                                          chars_per_segment);
    for (int segment = 0; segment < out.segments_total; ++segment) {
        if ((received_mask & (1U << segment)) != 0) {
            ++out.segments_received;
        }
    }

    // Built as code points first, so the trailing trim counts characters
    // and not UTF-8 bytes. A trim over bytes would cut a multi-byte glyph
    // in half and hand Qt an invalid sequence.
    std::string rendered;
    std::size_t trailing_spaces = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const auto segment = static_cast<unsigned>(i / chars_per_segment);
        const bool received = segment < 32 && (received_mask & (1U << segment)) != 0;

        if (!received) {
            // A placeholder is content, so anything before it stays.
            rendered.append(trailing_spaces, ' ');
            trailing_spaces = 0;
            append_utf8(rendered, kRdsPlaceholder);
            continue;
        }

        const char32_t code_point = annex_e_code_point(bytes[i]);
        if (code_point == 0) {
            continue;
        }
        if (code_point == U' ') {
            // Held back until something non-space follows, which is what
            // trims the padding without losing a space inside the name.
            ++trailing_spaces;
            continue;
        }
        rendered.append(trailing_spaces, ' ');
        trailing_spaces = 0;
        append_utf8(rendered, code_point);
    }

    out.text = rendered;
    return out;
}

}  // namespace revenant::ui

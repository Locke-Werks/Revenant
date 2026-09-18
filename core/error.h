// Error handling for the engine.
//
// std::expected throughout, per the conventions. Exceptions only cross an API
// boundary where the standard library itself throws.

#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace revenant {

// A failure carries a message meant for a person: what was being attempted and
// what the underlying layer said about it. Codes belong beside the message, not
// instead of it. A bare VkResult in a log tells nobody which allocation failed.
struct Error {
    std::string message;

    // The originating API's code where there is one. Zero means the failure was
    // ours rather than a call's, so do not print it as though a driver said it.
    long long code = 0;

    Error() = default;

    explicit Error(std::string text, long long api_code = 0)
        : message(std::move(text)), code(api_code) {}
};

template <class T>
using Expected = std::expected<T, Error>;

using Status = std::expected<void, Error>;

inline std::unexpected<Error> fail(std::string message, long long code = 0) {
    return std::unexpected(Error{std::move(message), code});
}

// Prefixes an error as it moves up a layer, so the final message reads as a
// chain rather than as whichever frame happened to notice.
inline Error with_context(Error inner, std::string_view context) {
    Error out;
    out.code = inner.code;
    out.message.reserve(context.size() + 2 + inner.message.size());
    out.message.append(context);
    out.message.append(": ");
    out.message.append(inner.message);
    return out;
}

}  // namespace revenant

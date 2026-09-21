// Error handling for the engine.
//
// std::expected throughout, per the conventions. Exceptions only cross an API
// boundary where the standard library itself throws.

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace revenant {

// WHAT A CALLER CAN DO ABOUT A FAILURE, WHICH IS THE ONLY REASON THIS EXISTS.
//
// Not what went wrong. The message says that, and it says it better than any
// enumeration will. This answers the one question a message cannot: is asking
// again worth anything.
//
// The case that forced it was a loop that could not stop. ui/models/
// engine_link.cpp reconnects once a second, and a wrong token is refused once
// a second forever, because a refused token and an engine that has not started
// yet arrived as the same thing: an Error carrying a sentence. The supervisor
// had nothing to branch on but the message text, which breaks the first time
// the wording improves.
//
// SO EVERY ENUMERATOR HERE HAS A CALLER. There is no Internal and no generic
// Refused, not because neither happens but because nothing would read them. A
// category with no reader is a field that goes stale without anybody noticing,
// which is the failure this whole file is written against. Add one when
// something needs to branch on it.
enum class ErrorCategory : std::uint8_t {
    // Nobody said. Most of the tree, and deliberately so: about eleven hundred
    // fail() sites carry a sentence that is already the whole of what a person
    // needs, and retro-fitting a category onto each would be eleven hundred
    // guesses about what a future caller wants.
    //
    // A caller that branches treats this as the general case. It is not a
    // fifth meaning and it is not a claim that the failure is transient.
    Unclassified = 0,

    // Nothing answered where we asked. The engine has not started, the port is
    // wrong, or the address does not resolve.
    //
    // ORDINARY AT BOTH ENDS OF AN ENGINE'S LIFE, which is what separates it
    // from every other category here. A client started before its engine is
    // not a fault and should not read as one; it is waiting.
    Unreachable,

    // A connection that was good is gone. Release what was held and start
    // over: that is what Cap'n Proto's own DISCONNECTED means and this is the
    // same verdict on our side of the translation.
    Disconnected,

    // The credential was refused. Retrying with the same one is pointless, and
    // a loop that retries anyway writes the same line until somebody stops it.
    //
    // The only one of these a client can be certain of by position rather than
    // by text: core/rpc/client.cpp raises it from the login handshake alone,
    // where the schema documents a refused token as the single refusal.
    Unauthenticated,

    // The peer does not implement the call, so it is older than this caller.
    // Try an older method if there is one; there is nothing else to try.
    Unimplemented,

    // Out of resources now. Worth asking again, later rather than at once,
    // because asking again immediately is what caused it.
    Overloaded,
};

// A failure carries a message meant for a person: what was being attempted and
// what the underlying layer said about it. Codes belong beside the message, not
// instead of it. A bare VkResult in a log tells nobody which allocation failed.
struct Error {
    std::string message;

    // The originating API's code where there is one. Zero means the failure was
    // ours rather than a call's, so do not print it as though a driver said it.
    long long code = 0;

    // What a caller can do about it. See ErrorCategory above for why this is
    // not a taxonomy of causes.
    //
    // WHAT SURVIVES A CAP'N PROTO ROUND TRIP IS NARROWER THAN THIS, and the
    // narrowing is in the protocol rather than in our translation of it.
    // capnp::rpc::Exception carries a reason, a four-value type and a trace,
    // and nothing else: there is no detail blob on the wire, so kj::Exception
    // detail set on one side does not reach the other. core/rpc/server.cpp's
    // to_exception maps this onto that type where one fits and core/rpc/
    // client.cpp's translate maps it back, which carries Disconnected,
    // Unimplemented and Overloaded exactly and flattens everything else to
    // Unclassified.
    //
    // That is why an application condition a client has to handle specifically
    // belongs in the schema rather than in this field. rpc.capnp says so in as
    // many words, under its own Exception struct: exceptions should not be used
    // to flag conditions a client is expected to handle in an
    // application-specific way. A refusal a client acts on is a result field.
    ErrorCategory category = ErrorCategory::Unclassified;

    Error() = default;

    explicit Error(std::string text, long long api_code = 0,
                   ErrorCategory failure_category = ErrorCategory::Unclassified)
        : message(std::move(text)), code(api_code), category(failure_category) {}

    explicit Error(std::string text, ErrorCategory failure_category)
        : message(std::move(text)), category(failure_category) {}
};

template <class T>
using Expected = std::expected<T, Error>;

using Status = std::expected<void, Error>;

inline std::unexpected<Error> fail(std::string message, long long code = 0) {
    return std::unexpected(Error{std::move(message), code});
}

// The category overload, for the failures something branches on. Separate
// rather than a third defaulted parameter on the one above, so a call site
// naming a category does not also have to name a code it does not have.
inline std::unexpected<Error> fail(std::string message, ErrorCategory category,
                                   long long code = 0) {
    return std::unexpected(Error{std::move(message), code, category});
}

// Prefixes an error as it moves up a layer, so the final message reads as a
// chain rather than as whichever frame happened to notice.
//
// THE CATEGORY TRAVELS WITH IT. An error that crossed a layer boundary is the
// same failure with more of its story told, and a caller two frames up wants
// the verdict of the frame that actually knew. Dropping it here would make
// every classified failure read as Unclassified the moment somebody added
// context to it, which is a bug that would look like the category never having
// been set.
inline Error with_context(Error inner, std::string_view context) {
    Error out;
    out.code = inner.code;
    out.category = inner.category;
    out.message.reserve(context.size() + 2 + inner.message.size());
    out.message.append(context);
    out.message.append(": ");
    out.message.append(inner.message);
    return out;
}

// What to print when a category is worth naming beside the message, and empty
// for the case where it is not. Unclassified has nothing to add: the sentence
// is the whole of what was known.
[[nodiscard]] inline std::string_view category_name(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::Unreachable:
            return "unreachable";
        case ErrorCategory::Disconnected:
            return "disconnected";
        case ErrorCategory::Unauthenticated:
            return "unauthenticated";
        case ErrorCategory::Unimplemented:
            return "unimplemented";
        case ErrorCategory::Overloaded:
            return "overloaded";
        case ErrorCategory::Unclassified:
            break;
    }
    return {};
}

}  // namespace revenant

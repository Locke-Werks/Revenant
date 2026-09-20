// revenant-ui: the window that draws what the engine is hearing.
//
// It is a client and nothing else. It holds no DSP, opens no device and
// never sees a sample: everything on screen arrives through
// core/rpc/client.h over a socket, which is the split core/rpc/types.h
// explains and the reason this is a second process rather than a second
// window in revenant-cli.
//
// USAGE
//
//   revenant-ui [address] [port] [--every-nth N]
//
// Loopback and a default port when nothing is given, because the ordinary
// case is an engine on the same machine and a remote engine is a decision
// somebody makes on purpose. core/rpc/server.h binds 127.0.0.1 by default
// for the same reason: there is no authentication on this interface yet.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>

#include "models/engine_link.h"

namespace {

// The engine publishes no default port. ServerOptions::port is zero, which
// binds an ephemeral one so two test runs on a machine do not collide, and
// nothing in the tree names a number for a daemon to use. This is therefore
// this client's own choice and not a contract: when a daemon names one, that
// number replaces this and this constant goes away.
constexpr std::uint16_t kDefaultPort = 17690;

// Thirty a second from an engine making three hundred. The engine drops the
// rest before copying them, so asking for fewer costs it less rather than
// more, and thirty is already past what a waterfall shows a person.
constexpr std::uint32_t kDefaultEveryNth = 10;

[[nodiscard]] bool parse_u32(std::string_view text, std::uint32_t& out)
{
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

}  // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Revenant"));
    QGuiApplication::setOrganizationName(QStringLiteral("Locke Werks"));

    // Basic rather than the platform style. The platform styles refuse to be
    // customised at all, and every colour here is tied to the colour map the
    // spectrum draws with.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QString address = QStringLiteral("127.0.0.1");
    std::uint16_t port = kDefaultPort;
    std::uint32_t every_nth = kDefaultEveryNth;

    const QStringList args = QGuiApplication::arguments();
    QStringList positional;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--every-nth") && i + 1 < args.size()) {
            const std::string value = args[++i].toStdString();
            if (!parse_u32(value, every_nth)) {
                std::fputs("--every-nth wants a whole number\n", stderr);
                return 2;
            }
            continue;
        }
        positional.append(args[i]);
    }

    if (!positional.isEmpty()) {
        address = positional.at(0);
    }
    if (positional.size() > 1) {
        std::uint32_t parsed = 0;
        const std::string value = positional.at(1).toStdString();
        if (!parse_u32(value, parsed) || parsed == 0 || parsed > 65535) {
            std::fputs("the port must be between 1 and 65535\n", stderr);
            return 2;
        }
        port = static_cast<std::uint16_t>(parsed);
    }

    // Constructed here rather than by QML so the address from argv reaches
    // it, and parented to nothing so its destructor runs before the Qt
    // event loop is gone: it ends the subscription and joins the Cap'n
    // Proto loop thread, and both want a live process around them.
    revenant::ui::EngineLink link;

    // A refused connection is not a startup failure. The engine is often
    // not running yet, and a window that comes up and says so is more use
    // than an exit code nobody sees: this is a GUI subsystem binary.
    static_cast<void>(link.open(address, port, every_nth));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("engineLink"), &link);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);

    engine.loadFromModule("Revenant", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return QGuiApplication::exec();
}

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
//   revenant-ui [address] [port] [--every-nth N] [--smoke-seconds N]
//               [--receiver FREQ:MODE] [--decode NAME] [--grab-receivers FILE]
//
// --smoke-seconds is for CI, which has no screen and no one to close the
// window: it runs on the offscreen platform unless QT_QPA_PLATFORM names
// another, loads the QML, runs for N seconds and exits, 0 if the QML loaded
// and logged no warning and 1 otherwise. It writes no settings, so a run on a
// developer's machine leaves their remembered engine and windows alone. The
// engine need not be running: a window waiting for one is a state the QML
// has to draw too.
//
// --receiver opens a receiver at FREQ in MODE once a source is open that
// reaches it, FREQ in the frequency box's grammar with bare numbers in hertz,
// "14005000:usb" or "145.005M:nfm". --decode names a decoder to attach to it,
// or auto, and switches decoding on. --grab-receivers writes the receiver
// window to FILE as a PNG when a smoke run ends. Together they drive and
// photograph the decode section with nobody at the mouse, which is what they
// are for: a window on the offscreen platform takes no input from, and puts
// nothing on, the desktop it runs beside.
//
// Loopback and a default port when nothing is given, because the ordinary
// case is an engine on the same machine and a remote engine is a decision
// somebody makes on purpose. core/rpc/server.h binds 127.0.0.1 by default,
// for a reason that is close to this one and not the same.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// It ended "for the same reason: there is no authentication on this
// interface yet". There is. Authenticator.login takes a pre-shared token
// before a caller holds a Session at all, this process reads that token in
// resolve_token below, and ServerOptions::token is a create() failure when
// empty rather than an off switch. The sentence stopped being true on
// 2026-09-20 and is recorded rather than swapped, because a reader deciding
// whether to expose the port was being told the control was absence.
//
// What the loopback default is for NOW is narrower and still load-bearing:
// the wire is plaintext, so a token crossing a routable interface is
// readable and replayable by anything on the path. Off loopback still means
// a tunnel. core/rpc/server.h carries the full record.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QWindow>

#include "audio/audio_player.h"
#include "models/engine_link.h"
#include "models/frequency_entry.h"
#include "models/settings.h"

// main() is at global scope, unlike everything it constructs. An alias
// rather than a using-directive, so the keys still read as settings::
// here exactly as they do inside the client.
namespace settings = revenant::ui::settings;

namespace {

// The port the engine and this window agree on without either naming one.
// revenant-engine binds 17690 by default and says why in tools/engined/main.cpp,
// which cites this constant; --port 0 there still binds whatever is free, for
// a supervisor running two engines on one machine.
//
// WHAT THIS PARAGRAPH USED TO SAY. It began "The engine publishes no default
// port", and said the number was this client's own choice, to be replaced
// "when a daemon names one". The engine named this one on 2026-09-22, so the
// two now have to change together.
constexpr std::uint16_t kDefaultPort = 17690;

// Every frame.
//
// This was ten, on the belief that the engine makes three hundred frames a
// second and a waterfall wants about thirty. It does not. A frame covers one
// uploaded block, and at the defaults revenant-engine runs, 2.4 MS/s in
// 65536-sample blocks, that is 36.6 a second: measured on an RTL-SDR v3 at
// 98.1 MHz as 633 frames sent over 172 s with this client asking for one in
// ten, which is 3.7 a second reaching the display. A waterfall at 3.7 rows a
// second takes over two minutes to fill a 600-row item and does not read as
// a live radio while it does.
//
// So one, and the engine's own block size is what sets the rate. A client on
// a slower machine, or one watching a much faster source, turns it down with
// --every-nth; the engine drops what is not asked for before copying it, so
// that costs the engine less rather than more.
constexpr std::uint32_t kDefaultEveryNth = 1;

[[nodiscard]] bool parse_u32(std::string_view text, std::uint32_t& out)
{
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

// Where a window was, and for a window that can be closed without quitting,
// whether it was open. One of these per top-level window, living in main's
// frame, which outlives exec() and therefore every lambda that writes it.
struct RememberedWindow {
    QRect windowed;
    QWindow::Visibility visibility = QWindow::Windowed;
    bool open = true;
};

struct WindowKeys {
    QLatin1StringView geometry;
    QLatin1StringView visibility;

    // Empty for the main window, which is never closed without quitting.
    QLatin1StringView open;
};

// WHERE A WINDOW WAS, RESTORED HERE AND NOT IN THE QML.
//
// QtCore's QML Settings type would do this in four lines, and it
// would add a QML module to what windeployqt has to find and to what
// the Forge installer has to carry. This is the same four lines in
// C++ against the QSettings the rest of the client already uses, and
// it keeps every key in ui/models/settings.h where a typo is a
// compile error.
//
// It was written inline for the one window there was, and became a function
// when the receivers moved into a window of their own on 2026-09-22.
//
// THE GEOMETRY IS CHECKED AGAINST A SCREEN BEFORE IT IS APPLIED. A
// window restored onto a monitor that has since been unplugged comes
// up off-screen, with no title bar to drag it back by, and the only
// repair is editing the registry. So a remembered rectangle that
// intersects no available screen is discarded and the window opens
// where QML put it.
void remember_window(QGuiApplication& app, QWindow* window, const QSettings& store,
                     const WindowKeys& keys, RememberedWindow& remembered)
{
    const QRect saved = store.value(keys.geometry).toRect();
    if (saved.isValid()) {
        bool on_a_screen = false;
        for (const QScreen* screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(saved)) {
                on_a_screen = true;
                break;
            }
        }
        if (on_a_screen) {
            window->setGeometry(saved);
        }
    }

    // Open or closed, for a window that can be one without the other. Open is
    // the default, so the first launch shows every window there is.
    if (!keys.open.isEmpty()) {
        remembered.open = store.value(keys.open, true).toBool();
        window->setVisible(remembered.open);
    }

    // Maximised comes back maximised. Without this a window that was
    // maximised is restored to whatever size it had before it was,
    // which is not where the operator left it.
    //
    // Only these two are honoured. Minimised, hidden and full screen
    // are all states a window can be left in by something other than
    // a preference, and coming up in any of them looks like the
    // client failed to start.
    const auto visibility = static_cast<QWindow::Visibility>(
        store.value(keys.visibility, static_cast<int>(QWindow::Windowed)).toInt());
    if (visibility == QWindow::Maximized && remembered.open) {
        window->setVisibility(QWindow::Maximized);
    }

    // CACHED WHILE THE WINDOW IS ALIVE AND WRITTEN ONCE ON THE WAY
    // OUT, WHICH IS NOT WHERE THIS STARTED.
    //
    // The obvious arrangement is to read window->geometry() from
    // aboutToQuit. Measured on this Qt: it does not work, and it
    // fails silently. By the time aboutToQuit runs the window has
    // already been closed, so visibility() is Hidden and geometry()
    // is not the rectangle anybody left it at. The first version of
    // this wrote "visibility 0" and no geometry at all, on a window
    // that had been moved to a new position seconds earlier, and
    // nothing about that looks wrong until the next launch comes up
    // in the old place.
    //
    // So the last WINDOWED rectangle and the last real visibility are
    // cached as they change, and the quit handler only writes what
    // was cached.
    //
    // Caching and not writing, because the two settings have opposite
    // costs. A volume change is one event an operator made and is
    // written as it happens. A geometry change is a continuous stream
    // of them during a drag, and writing each would be a registry
    // write per frame of the resize.
    //
    // geometry() and setGeometry() are the pair, deliberately: both
    // are the CLIENT area. Mixing them with frameGeometry walks the
    // window down and right by the title bar height on every launch,
    // because what was saved includes the frame and what is restored
    // does not.
    //
    // Seeded from what was just restored, so a session in which the
    // operator moves nothing writes back what they left rather than
    // a default. The visibility in particular has to be seeded here:
    // setVisibility above already fired visibilityChanged, before
    // the handler below existed to hear it.
    remembered.windowed = window->geometry();
    remembered.visibility =
        visibility == QWindow::Maximized ? QWindow::Maximized : QWindow::Windowed;

    const auto note_geometry = [window, &remembered] {
        // A maximised window's geometry is the screen, which is not
        // where it would go if it were unmaximised, and Qt does not
        // publish the restore rectangle. So only the windowed
        // rectangle is ever recorded and the last one survives being
        // maximised.
        if (window->visibility() == QWindow::Windowed) {
            remembered.windowed = window->geometry();
        }
    };
    QObject::connect(window, &QWindow::xChanged, window, note_geometry);
    QObject::connect(window, &QWindow::yChanged, window, note_geometry);
    QObject::connect(window, &QWindow::widthChanged, window, note_geometry);
    QObject::connect(window, &QWindow::heightChanged, window, note_geometry);

    QObject::connect(window, &QWindow::visibilityChanged, window,
                     [&remembered](QWindow::Visibility now) {
                         // Only the two states that are a preference.
                         // Hidden is what a close looks like, and
                         // minimised and full screen are things that
                         // happen to a window rather than choices
                         // about where it lives.
                         if (now == QWindow::Windowed || now == QWindow::Maximized) {
                             remembered.visibility = now;
                         }
                     });

    // Whether a closable window is open is the one thing a close IS a
    // preference about, so here Hidden counts.
    if (!keys.open.isEmpty()) {
        QObject::connect(window, &QWindow::visibleChanged, window,
                         [&remembered](bool visible) { remembered.open = visible; });
    }

    QObject::connect(&app, &QGuiApplication::aboutToQuit, window, [keys, &remembered] {
        QSettings out;
        out.setValue(keys.visibility, static_cast<int>(remembered.visibility));
        out.setValue(keys.geometry, remembered.windowed);
        if (!keys.open.isEmpty()) {
            out.setValue(keys.open, remembered.open);
        }
    });
}

}  // namespace

int main(int argc, char* argv[])
{
    // Looked for before the application exists, because the platform is
    // chosen when it is constructed. The value is parsed properly below.
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--smoke-seconds") {
            smoke = true;
        }
    }
    if (smoke && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Revenant"));
    QGuiApplication::setOrganizationName(QStringLiteral("Locke Werks"));

    // Basic rather than the platform style. The platform styles refuse to be
    // customised at all, and every colour here is tied to the colour map the
    // spectrum draws with.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Segoe UI Variable, which ships with Windows 11 and is the one family
    // the interface is drawn in. Set on the application rather than per
    // control so a Text with no font of its own gets it too, and qml/Theme.qml
    // names the same family for the controls that set one.
    //
    // A LIST AND NOT ONE NAME. With the single name, the QML text resolved it
    // and the detection labels, which render/spectrum_item.cpp paints through
    // QPainter from this same application font, came out in a serif: the
    // painter's lookup did not find the variable font's instance name and
    // fell back to the GDI default rather than to anything sans. Plain Segoe
    // UI second means a lookup that misses the first lands on the older cut
    // of the same design.
    {
        QFont interface_font;
        interface_font.setFamilies(
            {QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI")});
        interface_font.setPixelSize(12);
        QGuiApplication::setFont(interface_font);
    }

    // WHERE THE ENGINE WAS LAST TIME, UNDER WHAT ARGV SAYS AND OVER THE
    // COMPILED DEFAULT.
    //
    // Three layers and the order is deliberate. A remembered value is a
    // convenience and a command line is an instruction: a shortcut or a
    // script that names an address is naming it for a reason, and a
    // remembered one that overrode it would be unexplainable from outside
    // the window. So argv wins, the remembered value fills in what argv
    // leaves out, and the loopback default is what a first run gets.
    //
    // Read before the parse and written after it, so a rejected port does
    // not get remembered.
    const QSettings store;
    QString address =
        store.value(settings::kEngineAddress, QStringLiteral("127.0.0.1")).toString();
    auto port = static_cast<std::uint16_t>(
        store.value(settings::kEnginePort, kDefaultPort).toUInt());
    if (port == 0) {
        // A stored zero is a settings file somebody edited. Zero binds an
        // ephemeral port on the server side and is not a thing to connect
        // to, so it falls back rather than being tried.
        port = kDefaultPort;
    }
    std::uint32_t every_nth = kDefaultEveryNth;
    std::uint32_t smoke_seconds = 0;
    QString startup_receiver;
    QString startup_decoder;
    QString grab_receivers;

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
        if (args[i] == QStringLiteral("--receiver") && i + 1 < args.size()) {
            startup_receiver = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--decode") && i + 1 < args.size()) {
            startup_decoder = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--grab-receivers") && i + 1 < args.size()) {
            grab_receivers = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--smoke-seconds")) {
            const std::string value = i + 1 < args.size() ? args[++i].toStdString() : "";
            if (!parse_u32(value, smoke_seconds) || smoke_seconds == 0) {
                std::fputs("--smoke-seconds wants a whole number of seconds\n", stderr);
                return 2;
            }
            continue;
        }
        positional.append(args[i]);
    }

    // FREQ:MODE, split at the last colon so a frequency never has to avoid
    // one. The mode is checked by EngineLink against its own list when it is
    // placed, and refused there in the receiver's fault line.
    double startup_hz = 0.0;
    QString startup_mode;
    if (!startup_receiver.isEmpty()) {
        const qsizetype colon = startup_receiver.lastIndexOf(QLatin1Char(':'));
        const QString frequency = colon < 0 ? startup_receiver : startup_receiver.left(colon);
        startup_mode = colon < 0 ? QString() : startup_receiver.mid(colon + 1);
        const auto parsed = revenant::ui::parse_frequency(frequency.toStdString(),
                                                          revenant::ui::BareNumber::Hertz);
        if (!parsed.has_value()) {
            std::fputs("--receiver wants FREQ:MODE, as 14005000:usb or 145.005M:nfm\n", stderr);
            return 2;
        }
        startup_hz = static_cast<double>(parsed->hertz);
    }
    if (!grab_receivers.isEmpty() && !smoke) {
        std::fputs("--grab-receivers needs --smoke-seconds, which is the run it ends\n", stderr);
        return 2;
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

    // Written after the parse, so a rejected port is never stored, and
    // written whether or not argv supplied one: rewriting the remembered
    // value with itself costs nothing and keeps this to one line. Not in a
    // smoke run, whose address is CI's and not the operator's.
    if (!smoke) {
        QSettings out;
        out.setValue(settings::kEngineAddress, address);
        out.setValue(settings::kEnginePort, static_cast<unsigned>(port));
    }

    // Constructed here rather than by QML so the address from argv reaches
    // it, and parented to nothing so its destructor runs before the Qt
    // event loop is gone: it stops the supervisor, ends the subscription and
    // joins the Cap'n Proto loop thread, and all three want a live process
    // around them.
    revenant::ui::EngineLink link;

    // A refused connection is not a startup failure and never was. The
    // engine is often not running yet, and this returns either way: the
    // supervisor goes on trying, and the window comes up saying what it is
    // waiting for. Starting the engine second is a supported order, and so
    // is stopping and restarting it under a running window.
    link.start(address, port, every_nth);
    if (!startup_receiver.isEmpty()) {
        link.setStartupReceiver(startup_hz, startup_mode, startup_decoder);
    }

    // DECLARED AFTER THE LINK ON PURPOSE, so it is destroyed BEFORE it.
    //
    // The player owns the QAudioSink, and stopping a sink is what joins the
    // thread Qt has pulling through the ring. The ring belongs to the link.
    // The other order leaves that thread reading a ring whose owner is
    // being destroyed, which is a crash on exit that only happens while
    // audio is actually playing.
    revenant::ui::AudioPlayer player(link);
    player.start();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("engineLink"), &link);
    engine.rootContext()->setContextProperty(QStringLiteral("audioPlayer"), &player);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);

    // A smoke run fails on any QML warning, since a binding that throws or a
    // property that does not exist is a window that loads and is wrong. The
    // engine still prints them itself.
    int qml_warnings = 0;
    if (smoke) {
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                         [&qml_warnings](const QList<QQmlError>& errors) {
                             qml_warnings += static_cast<int>(errors.size());
                         });
    }

    engine.loadFromModule("Revenant", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    if (smoke) {
        // The receiver window is shown for a grab, since nothing else in a
        // smoke run shows it, and grabbed as the run ends so what it holds is
        // what the whole run decoded.
        QQuickWindow* receivers = nullptr;
        if (!grab_receivers.isEmpty()) {
            if (auto* root = qobject_cast<QWindow*>(engine.rootObjects().constFirst())) {
                receivers = root->findChild<QQuickWindow*>(QStringLiteral("vrxWindow"));
            }
            if (receivers == nullptr) {
                std::fputs("--grab-receivers: this build of the QML has no receiver window\n",
                           stderr);
                return 1;
            }
            receivers->setVisible(true);
        }
        QTimer::singleShot(std::chrono::seconds(smoke_seconds), &app, [receivers, grab_receivers] {
            if (receivers != nullptr) {
                const QImage shot = receivers->grabWindow();
                if (shot.isNull() || !shot.save(grab_receivers)) {
                    std::fprintf(stderr, "--grab-receivers: could not write %s\n",
                                 qPrintable(grab_receivers));
                    QCoreApplication::exit(1);
                    return;
                }
            }
            QCoreApplication::exit(0);
        });
        const int code = QGuiApplication::exec();
        if (code != 0) {
            return code;
        }
        if (qml_warnings > 0) {
            std::fprintf(stderr, "smoke: %d QML warning(s)\n", qml_warnings);
            return 1;
        }
        std::fputs("smoke: loaded and ran without a QML warning\n", stderr);
        return 0;
    }

    // Both windows' places, remembered between launches. See remember_window.
    // The main window is the root object and the receiver window is found by
    // name inside it; a build of the QML without one is simply not restored.
    static RememberedWindow main_window;
    static RememberedWindow receiver_window;
    if (auto* window = qobject_cast<QWindow*>(engine.rootObjects().constFirst())) {
        remember_window(app, window, store,
                        WindowKeys{settings::kWindowGeometry, settings::kWindowVisibility, {}},
                        main_window);
        if (auto* receivers = window->findChild<QWindow*>(QStringLiteral("vrxWindow"))) {
            remember_window(app, receivers, store,
                            WindowKeys{settings::kVrxWindowGeometry,
                                       settings::kVrxWindowVisibility, settings::kVrxWindowOpen},
                            receiver_window);
        }
    }

    return QGuiApplication::exec();
}

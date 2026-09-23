// revenant-ui: the window that draws what the engine is hearing.
//
// It is a client and nothing else. It opens no radio and never sees an
// I/Q sample: everything on screen arrives through core/rpc/client.h over
// a socket, which is the split core/rpc/types.h explains and the reason
// this is a second process rather than a second window in revenant-cli.
// The one signal processing it does is on the audio it plays: the rack's
// mix in audio/audio_mix.h resamples, levels and limits what the engine
// sends. WHAT THIS PARAGRAPH USED TO SAY: "It holds no DSP, opens no
// device and never sees a sample", which the mix made false on 2026-09-23.
//
// USAGE
//
//   revenant-ui [address] [port] [--every-nth N] [--smoke-seconds N]
//               [--receiver FREQ:MODE ...] [--decode NAME] [--rds]
//               [--grab-receivers FILE] [--palette QUERY] [--keymap]
//               [--grab-main FILE] [--frame-stats FILE] [--maximise]
//               [--on-top] [--memories FILE] [--panel NAME]
//               [--preview-import FILE] [--open-recording PATH[:CENTER] ...]
//
// --smoke-seconds is for CI, which has no screen and no one to close the
// window: it runs on the offscreen platform unless QT_QPA_PLATFORM names
// another, loads the QML, runs for N seconds and exits, 0 if the QML loaded
// and logged no warning and 1 otherwise. It writes no settings, so a run on a
// developer's machine leaves their remembered engine and windows alone, and
// it opens no sound card, so it makes no noise on one either. The
// engine need not be running: a window waiting for one is a state the QML
// has to draw too.
//
// --receiver opens a receiver at FREQ in MODE once a source is open that
// reaches it, FREQ in the frequency box's grammar with bare numbers in hertz,
// "14005000:usb" or "145.005M:nfm". It may be given up to eight times: the first
// is the focused receiver and the rest are held in the rack, which is how the
// rack is photographed. --decode names a decoder to attach to the first,
// or auto, and switches decoding on. --rds switches the RDS section on for
// the first, which on a wfm one raises it to the composite rate as the switch
// in the window does. --grab-receivers writes the receiver window to FILE as
// a PNG when a smoke run ends. Together they drive and photograph the rack and
// the decode and RDS sections with nobody at the mouse, which is what they are
// for: a window on the offscreen platform takes no input from, and puts
// nothing on, the desktop it runs beside.
//
// --palette opens the main window's command palette with QUERY typed into it,
// and --keymap opens its key map; --grab-main writes the main window to FILE
// as a PNG when a smoke run ends. They photograph the keyboard's two overlays
// the same way, without a key being pressed on anybody's desktop. A smoke run
// also fails when ui/qml/Commands.qml lacks a handler the key table names,
// which is the one check on that table that has to run the QML to be made.
//
// --frame-stats times every frame each window draws and what the display
// items spend on it, and on the way out writes the figures to FILE as JSON
// and one line to stderr; render/frame_probe.h and models/frame_stats.h are
// the instrument and docs/ui-spectrum.md, "Frame budget", says what it is
// for. It works with or without --smoke-seconds, and a smoke run with
// QT_QPA_PLATFORM=windows is a timed run in a real window that still opens
// no sound card. --maximise opens the main window maximised, because a smoke
// run restores no geometry and the frame budget is about the size the span is
// actually used at, not the QML's default.
// --memories reads the frequency manager's list from FILE instead of the
// user's own, and never writes it; a smoke run without it starts with an empty
// list and no file at all. --panel opens one of the top bar's panels by the
// name the key table uses, "memories", "radio" or "detections", and
// --preview-import reads FILE into the frequency manager's import preview.
// With --grab-main they photograph the frequency manager on sample memories.
// --open-recording PATH[:CENTER] opens a recording at startup through the
// picker's own path: the header is read, the centre goes in its box, and the
// open is sent if nothing is missing, otherwise the section shows what is and
// the reason goes to stderr. CENTER takes the frequency box's grammar, and is
// needed for a file that records none, which is every KF4FIC WAV. Given more
// than once, each earlier one goes on the recent list as though it had been
// opened and the last is the one opened. --open-panel NAME is the same flag as
// --panel, kept because the recording lane's scripts name it, so a smoke run
// can photograph the picker with --panel radio; a native file dialog cannot be
// driven offscreen, and these are how the recording path is exercised without
// one.
//
// --on-top keeps both windows above every other window, for a measurement.
// A window started from a background process opens behind whatever is in
// front, and a window covered by others is timed as something else: DWM does
// not compose it, so its swap chain never waits for the display, and Windows
// 11 serves an occluded process's timers at the 15.6 ms tick. Measured on
// 2026-09-23 with a one-rectangle Qt Quick window at 120 Hz: 64.0 frames a
// second behind two maximised windows, 119.3 kept on top. It takes the screen
// from whoever is at it, so scripts/frame-budget.ps1 passes it only when
// asked to with -OnTop.
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include "models/frequency_manager.h"
#include "models/recording_link.h"
#include "models/settings.h"
#include "render/frame_probe.h"
#include "render/window_pacer.h"

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
        if (std::string_view(argv[i]) == "--frame-stats") {
            revenant::ui::FrameProbe::prepare();
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
    QStringList startup_receivers;
    QString startup_decoder;
    bool startup_rds = false;
    QString grab_receivers;
    QString grab_main;
    QString palette_query;
    bool palette_wanted = false;
    bool keymap_wanted = false;
    QString frame_stats;
    bool maximise = false;
    bool on_top = false;
    QString memories_file;
    QString open_panel;
    QString preview_import;
    QStringList startup_recordings;

    const QStringList args = QGuiApplication::arguments();
    QStringList positional;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--open-recording") && i + 1 < args.size()) {
            startup_recordings.append(args[++i]);
            continue;
        }
        if (args[i] == QStringLiteral("--open-panel") && i + 1 < args.size()) {
            open_panel = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--every-nth") && i + 1 < args.size()) {
            const std::string value = args[++i].toStdString();
            if (!parse_u32(value, every_nth)) {
                std::fputs("--every-nth wants a whole number\n", stderr);
                return 2;
            }
            continue;
        }
        if (args[i] == QStringLiteral("--receiver") && i + 1 < args.size()) {
            startup_receivers.append(args[++i]);
            continue;
        }
        if (args[i] == QStringLiteral("--decode") && i + 1 < args.size()) {
            startup_decoder = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--rds")) {
            startup_rds = true;
            continue;
        }
        if (args[i] == QStringLiteral("--grab-receivers") && i + 1 < args.size()) {
            grab_receivers = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--grab-main") && i + 1 < args.size()) {
            grab_main = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--palette") && i + 1 < args.size()) {
            palette_query = args[++i];
            palette_wanted = true;
            continue;
        }
        if (args[i] == QStringLiteral("--memories") && i + 1 < args.size()) {
            memories_file = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--panel") && i + 1 < args.size()) {
            open_panel = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--preview-import") && i + 1 < args.size()) {
            preview_import = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--keymap")) {
            keymap_wanted = true;
            continue;
        }
        if (args[i] == QStringLiteral("--frame-stats") && i + 1 < args.size()) {
            frame_stats = args[++i];
            continue;
        }
        if (args[i] == QStringLiteral("--maximise")) {
            maximise = true;
            continue;
        }
        if (args[i] == QStringLiteral("--on-top")) {
            on_top = true;
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
    // placed, and refused there in the receiver's fault line. Repeatable: the
    // first is the focused receiver and the rest are held in the rack.
    std::vector<std::pair<double, QString>> startup;
    for (const QString& one : startup_receivers) {
        const qsizetype colon = one.lastIndexOf(QLatin1Char(':'));
        const QString frequency = colon < 0 ? one : one.left(colon);
        const QString mode = colon < 0 ? QString() : one.mid(colon + 1);
        const auto parsed = revenant::ui::parse_frequency(frequency.toStdString(),
                                                          revenant::ui::BareNumber::Hertz);
        if (!parsed.has_value()) {
            std::fputs("--receiver wants FREQ:MODE, as 14005000:usb or 145.005M:nfm\n", stderr);
            return 2;
        }
        startup.emplace_back(static_cast<double>(parsed->hertz), mode);
    }
    if (startup_rds && startup_receivers.isEmpty()) {
        std::fputs("--rds needs --receiver, which is the receiver it switches RDS on for\n",
                   stderr);
        return 2;
    }
    if (!grab_receivers.isEmpty() && !smoke) {
        std::fputs("--grab-receivers needs --smoke-seconds, which is the run it ends\n", stderr);
        return 2;
    }
    if (!grab_main.isEmpty() && !smoke) {
        std::fputs("--grab-main needs --smoke-seconds, which is the run it ends\n", stderr);
        return 2;
    }
    if (!memories_file.isEmpty() && !smoke) {
        std::fputs("--memories needs --smoke-seconds; a normal run keeps its own list\n", stderr);
        return 2;
    }
    if (palette_wanted && keymap_wanted) {
        std::fputs("--palette and --keymap are two overlays in one place; ask for one\n", stderr);
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
    if (!startup.empty()) {
        for (std::size_t i = 1; i < startup.size(); ++i) {
            link.addStartupReceiver(startup[i].first, startup[i].second);
        }
        link.setStartupReceiver(startup.front().first, startup.front().second,
                                startup_decoder);
    }
    // The switch is sticky and asks nothing until the pane holds a receiver,
    // so setting it before the receiver is placed is the order the window's
    // own switch would take too.
    if (startup_rds) {
        link.setRdsWanted(true);
    }

    // DECLARED AFTER THE LINK ON PURPOSE, so it is destroyed BEFORE it.
    //
    // The player owns the QAudioSink, and stopping a sink is what joins the
    // thread Qt has pulling through the ring. The ring belongs to the link.
    // The other order leaves that thread reading a ring whose owner is
    // being destroyed, which is a crash on exit that only happens while
    // audio is actually playing.
    revenant::ui::AudioPlayer player(link);

    // Not in a smoke run, which opens no sound card. The listen switch is
    // remembered, so a smoke run against a live engine on a machine whose
    // operator had it on played that engine's receiver through their
    // speakers: found on 2026-09-23 photographing the decode section.
    if (!smoke) {
        player.start();
    }

    // Declared before the QML engine so it is destroyed after it: every
    // display item reports to it, and a render thread can still be finishing
    // a frame while the engine tears the windows down.
    std::unique_ptr<revenant::ui::FrameProbe> frame_probe;
    if (!frame_stats.isEmpty()) {
        frame_probe = std::make_unique<revenant::ui::FrameProbe>(frame_stats, link);
        QObject::connect(&app, &QGuiApplication::aboutToQuit, frame_probe.get(),
                         [probe = frame_probe.get()] { probe->finish(); });
    }

    // The memory file: the user's own in a normal run, migrating the old
    // bookmark list into it the first time. A smoke run reads only what
    // --memories names and writes nothing, on the rule the usage above gives
    // for every other setting. Declared after the link, so it is destroyed
    // first and never holds a reference to a link that has gone.
    revenant::ui::FrequencyManager::Storage memory_storage;
    if (smoke) {
        memory_storage.path = memories_file;
        memory_storage.writable = false;
        memory_storage.migrate = false;
    } else {
        memory_storage.path = revenant::ui::FrequencyManager::defaultPath();
    }
    revenant::ui::FrequencyManager memories(link, memory_storage);
    if (!preview_import.isEmpty()) {
        memories.previewImport(preview_import);
    }

    // The recording section and strip. Keeps its recent list in memory in a
    // smoke run, which writes no settings.
    revenant::ui::RecordingLink recordings(link, address, !smoke);
    if (const QString refused = recordings.openAtStartup(startup_recordings);
        !refused.isEmpty()) {
        std::fprintf(stderr, "%s\n", refused.toLocal8Bit().constData());
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("engineLink"), &link);
    engine.rootContext()->setContextProperty(QStringLiteral("audioPlayer"), &player);
    engine.rootContext()->setContextProperty(QStringLiteral("frequencyManager"), &memories);
    engine.rootContext()->setContextProperty(QStringLiteral("recordingLink"), &recordings);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);

    // A smoke run fails on any QML warning, since a binding that throws or a
    // property that does not exist is a window that loads and is wrong. Each
    // one is printed here, because a connection to warnings() takes them over
    // and the engine then stops printing them, which left a failing run
    // saying only how many there were.
    int qml_warnings = 0;
    if (smoke) {
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                         [&qml_warnings](const QList<QQmlError>& errors) {
                             qml_warnings += static_cast<int>(errors.size());
                             for (const QQmlError& error : errors) {
                                 std::fprintf(stderr, "smoke: %s\n",
                                              error.toString().toLocal8Bit().constData());
                             }
                         });
    }

    engine.loadFromModule("Revenant", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    auto* root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    QObject* commands =
        root_window != nullptr ? root_window->findChild<QObject*>(QStringLiteral("commands"))
                               : nullptr;

    QQuickWindow* receiver_window_item =
        root_window != nullptr ? root_window->findChild<QQuickWindow*>(QStringLiteral("vrxWindow"))
                               : nullptr;

    // The receiver window is presented without waiting for vsync and drawn
    // straight after each main-window frame, because two windows that each
    // wait for vsync hold the one GUI thread through both waits, and the main
    // window missed 16.9% of its frames that way. render/window_pacer.h has
    // the measurements. Here and not later, because the swap interval is read
    // when the window is first shown, which remember_window and the smoke run
    // below both do.
    std::unique_ptr<revenant::ui::WindowPacer> pacer;
    if (receiver_window_item != nullptr) {
        if (!revenant::ui::WindowPacer::unthrottle(receiver_window_item)) {
            std::fputs("revenant-ui: the receiver window was shown before its swap interval "
                       "was set, so it waits for vsync\n",
                       stderr);
        }
        pacer = std::make_unique<revenant::ui::WindowPacer>(root_window, receiver_window_item);
    }

    if (frame_probe != nullptr && root_window != nullptr) {
        frame_probe->watch(root_window, QStringLiteral("main"));
        frame_probe->watch(receiver_window_item, QStringLiteral("receivers"));
    }
    if (on_top) {
        for (QWindow* window : {static_cast<QWindow*>(root_window),
                                static_cast<QWindow*>(receiver_window_item)}) {
            if (window != nullptr) {
                window->setFlag(Qt::WindowStaysOnTopHint, true);
            }
        }
    }
    if (maximise && root_window != nullptr) {
        root_window->showMaximized();
    }

    // Every handler the key table names has to be in Commands.qml, and only
    // the QML can say whether it is. Checked on every smoke run, which is
    // every CI run, so a key added without its handler fails there.
    if (smoke) {
        QVariant missing;
        if (commands == nullptr ||
            !QMetaObject::invokeMethod(commands, "missingText", Q_RETURN_ARG(QVariant, missing))) {
            std::fputs("smoke: this build of the QML has no Commands to check\n", stderr);
            return 1;
        }
        if (!missing.toString().isEmpty()) {
            std::fprintf(stderr, "smoke: the key table names handlers Commands.qml lacks: %s\n",
                         qPrintable(missing.toString()));
            return 1;
        }
    }

    if ((palette_wanted || keymap_wanted) && commands != nullptr) {
        if (palette_wanted) {
            QMetaObject::invokeMethod(commands, "showPalette",
                                      Q_ARG(QVariant, QVariant(palette_query)));
        } else {
            QMetaObject::invokeMethod(commands, "showKeyMap");
        }
    }

    if (!open_panel.isEmpty() && commands != nullptr) {
        QMetaObject::invokeMethod(commands, "showPanel", Q_ARG(QVariant, QVariant(open_panel)));
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
        QTimer::singleShot(std::chrono::seconds(smoke_seconds), &app,
                           [receivers, grab_receivers, root_window, grab_main] {
            if (receivers != nullptr) {
                const QImage shot = receivers->grabWindow();
                if (shot.isNull() || !shot.save(grab_receivers)) {
                    std::fprintf(stderr, "--grab-receivers: could not write %s\n",
                                 qPrintable(grab_receivers));
                    QCoreApplication::exit(1);
                    return;
                }
            }
            if (!grab_main.isEmpty()) {
                const QImage shot =
                    root_window != nullptr ? root_window->grabWindow() : QImage();
                if (shot.isNull() || !shot.save(grab_main)) {
                    std::fprintf(stderr, "--grab-main: could not write %s\n",
                                 qPrintable(grab_main));
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
        if (frame_probe != nullptr && !frame_probe->written()) {
            return 1;
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

    const int code = QGuiApplication::exec();
    if (code == 0 && frame_probe != nullptr && !frame_probe->written()) {
        return 1;
    }
    return code;
}

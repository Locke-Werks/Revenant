// The keys this client remembers between launches, in one place.
//
// WHY A HEADER OF CONSTANTS AND NOT A STRING AT EACH SITE
//
// A QSettings key is a string and a misspelled one does not fail: it
// writes a setting nothing reads and reads a default nothing wrote, and
// the symptom is a control that quietly stops remembering. The three
// objects that persist anything are in three files, so the names live
// here where a typo is a compile error.
//
// WHERE THIS ENDS UP. QSettings with no arguments uses the organisation
// and application names main() sets before anything here is constructed,
// which on Windows is HKCU\Software\Locke Werks\Revenant. Nothing in this
// client writes a file beside the binary; an installed copy and a copy run
// out of a build tree share one set of settings, which is what an operator
// expects when they replace the binary.
//
// WHAT IS DELIBERATELY NOT REMEMBERED. The receiver, its mode and its
// passband. A window that came up already tuned to last night's frequency
// would be making a claim about a band it has not looked at, and on a
// source that cannot retune it would be tuned somewhere the span does not
// even reach. The frequency box is seeded from where the radio actually
// is, which is the honest version of the same convenience.
//
// The RDS switch is not remembered either, and for a sharper reason: the
// first poll BUILDS a decoder on the engine, which is shared state on a
// machine other clients may be using. A switch that turned itself on at
// launch would stand a decoder up behind a receiver nobody asked about.

#pragma once

#include <QLatin1StringView>

namespace revenant::ui::settings {

// The audio output the operator chose, by device id rather than by index.
// QMediaDevices reorders the list when a device appears or goes, so an
// index remembered from last night names whichever device took the slot.
// Empty means the system default, which is index 0 and is not a device but
// a promise to follow whatever Windows is currently defaulting to.
inline constexpr QLatin1StringView kAudioDeviceId{"audio/deviceId"};

inline constexpr QLatin1StringView kAudioVolume{"audio/volume"};
inline constexpr QLatin1StringView kAudioMuted{"audio/muted"};

// The listen switch. It is a switch and not a state: it survives a
// retune, a mode change and a reconnect within a session, and this is the
// same fact across a restart.
inline constexpr QLatin1StringView kAudioListen{"audio/listen"};

// The detection confidence bar, which is this window's own filter and
// changes nothing about the engine.
inline constexpr QLatin1StringView kConfidenceBar{"detections/confidenceBar"};

// Where the window was. Saved as a rectangle plus the visibility, so a
// window that was maximised comes back maximised rather than at whatever
// size it happened to be restored to last.
inline constexpr QLatin1StringView kWindowGeometry{"window/geometry"};
inline constexpr QLatin1StringView kWindowVisibility{"window/visibility"};

// The last engine this client reached, used only when argv names none.
// An address on the command line always wins: a shortcut or a script that
// states one is stating it for a reason, and a remembered value that
// overrode it would be unexplainable from outside the window.
inline constexpr QLatin1StringView kEngineAddress{"engine/address"};
inline constexpr QLatin1StringView kEnginePort{"engine/port"};

}  // namespace revenant::ui::settings

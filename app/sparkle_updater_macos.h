// Sparkle auto-updater shim — a C++-only interface over Sparkle 2's
// SPUStandardUpdaterController (the standard update UI: check / download
// progress / release notes / install + relaunch).
//
// The macOS implementation lives in sparkle_updater_macos.mm and is compiled
// only when Sparkle is vendored (CMake defines MINNOTES_HAVE_SPARKLE when
// external/Sparkle/Sparkle.framework exists). Otherwise — and on every other
// platform — the calls compile to inline no-ops so callers can invoke them
// unconditionally.
#pragma once

namespace mn {

#if defined(__APPLE__) && defined(MINNOTES_HAVE_SPARKLE)

// Construct + start the shared updater. Call once at launch after the UI is
// up. Honors the Info.plist SU* keys (SUFeedURL, SUEnableAutomaticChecks,
// SUScheduledCheckInterval, SUPublicEDKey). Idempotent.
void startSparkleUpdater();

// User-initiated "Check for Updates…" — shows Sparkle's standard UI.
void checkForUpdatesNow();

#else  // no Sparkle — Windows ships its own updater; dev builds without it.

inline void startSparkleUpdater() {}
inline void checkForUpdatesNow() {}

#endif

} // namespace mn

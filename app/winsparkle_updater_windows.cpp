// WinSparkle auto-updater shim (Windows) — the counterpart to
// sparkle_updater_macos.mm. Implements the same mn:: free-function seam declared
// in sparkle_updater_macos.h, so callers (AppUpdater, main.cpp) invoke it
// identically on every platform. Compiled + linked only when WinSparkle is
// vendored (CMake defines MINNOTES_HAVE_WINSPARKLE when external/winsparkle
// exists); otherwise the header's inline no-ops apply.
//
// Unlike macOS Sparkle (which reads the Info.plist SU* keys), WinSparkle is
// configured imperatively here: the appcast feed, app details (the version is
// what the updater compares against the feed — minNotes.exe also carries a
// VS_VERSION_INFO resource, but set_app_details is authoritative for the check),
// and the SAME Ed25519 public key the macOS build verifies against
// (MINNOTES_UPDATE_PUBLIC_KEY, from the root CMakeLists).
#include "sparkle_updater_macos.h"

#if defined(_WIN32) && defined(MINNOTES_HAVE_WINSPARKLE)

#include <winsparkle.h>

#include <QCoreApplication>   // qAddPostRoutine — cleanup at app shutdown
#include <QtGlobal>

#ifndef MINNOTES_APP_VERSION
#  define MINNOTES_APP_VERSION "0.0.0"
#endif
#ifndef MINNOTES_UPDATE_PUBLIC_KEY
#  define MINNOTES_UPDATE_PUBLIC_KEY ""
#endif

// Two-step stringize so the MINNOTES_APP_VERSION macro is expanded before being
// widened to the wchar_t literal win_sparkle_set_app_details wants.
#define MN_WIDEN_INNER(x) L##x
#define MN_WIDEN(x)       MN_WIDEN_INNER(x)

namespace mn {
namespace {

bool g_started = false;

// The Windows update feed (mirrors the macOS feed at minnotes.app/appcast.xml).
// Signed with the shared Ed25519 key on macOS at release time (sign_update).
constexpr char kAppcastUrl[] = "https://minnotes.app/appcast-win.xml";

} // namespace

void startSparkleUpdater()
{
    if (g_started) return;

    win_sparkle_set_appcast_url(kAppcastUrl);
    win_sparkle_set_app_details(L"cbkow", L"minNotes",
                                MN_WIDEN(MINNOTES_APP_VERSION));
    if (MINNOTES_UPDATE_PUBLIC_KEY[0] != '\0')
        win_sparkle_set_eddsa_public_key(MINNOTES_UPDATE_PUBLIC_KEY);

    win_sparkle_set_automatic_check_for_updates(1);
    win_sparkle_set_update_check_interval(86400);   // daily background check

    win_sparkle_init();
    // WinSparkle must be cleanly shut down on its main thread before exit;
    // qAddPostRoutine fires during QCoreApplication teardown. (Signature matches
    // QtCleanUpFunction: void(*)().)
    qAddPostRoutine(win_sparkle_cleanup);

    g_started = true;
    qInfo("WinSparkle: updater started (feed=%s, version=%s)",
          kAppcastUrl, MINNOTES_APP_VERSION);
}

void checkForUpdatesNow()
{
    if (!g_started) startSparkleUpdater();
    win_sparkle_check_update_with_ui();
}

} // namespace mn

#endif // _WIN32 && MINNOTES_HAVE_WINSPARKLE

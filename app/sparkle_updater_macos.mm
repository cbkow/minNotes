#include "sparkle_updater_macos.h"

#if defined(__APPLE__) && defined(MINNOTES_HAVE_SPARKLE)

#import <Sparkle/Sparkle.h>

#include <QtGlobal>

namespace mn {
namespace {

// App-lifetime singleton. Retained for the process lifetime by design (a
// deliberate, harmless "leak"); the updater must outlive every check.
SPUStandardUpdaterController *g_updaterController = nil;

} // namespace

void startSparkleUpdater()
{
    if (g_updaterController) return;
    // startingUpdater:YES starts the updater immediately, honoring the
    // Info.plist SU* keys (feed URL, EdDSA public key, automatic-check policy
    // + interval). nil delegates → Sparkle's standard behavior.
    g_updaterController =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                      updaterDelegate:nil
                                                   userDriverDelegate:nil];
    qInfo("Sparkle: updater started (auto-checks=%d, interval=%.0fs)",
          g_updaterController.updater.automaticallyChecksForUpdates ? 1 : 0,
          g_updaterController.updater.updateCheckInterval);
}

void checkForUpdatesNow()
{
    if (!g_updaterController) startSparkleUpdater();
    [g_updaterController checkForUpdates:nil];
}

} // namespace mn

#endif // __APPLE__ && MINNOTES_HAVE_SPARKLE

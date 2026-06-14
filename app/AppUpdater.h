#pragma once

#include <QObject>
#include "sparkle_updater_macos.h"

// Thin QML bridge to the Sparkle updater. Exposed as the `appUpdater` context
// property so the native menu's "Check for Updates…" item can trigger a
// user-initiated check. On non-macOS / when Sparkle isn't vendored, the
// underlying calls are inline no-ops.
class AppUpdater : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    Q_INVOKABLE void checkForUpdates() { mn::checkForUpdatesNow(); }
};

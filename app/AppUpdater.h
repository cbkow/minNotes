#pragma once

#include <QObject>
#include "sparkle_updater_macos.h"

// Thin QML bridge to the Sparkle updater. Exposed as the `appUpdater` context
// property so the native menu's "Check for Updates…" item can trigger a
// user-initiated check. On non-macOS / when Sparkle isn't vendored, the
// underlying calls are inline no-ops. A Microsoft Store build has no updater at all (the Store delivers updates):
// `available` is false there and the app menu drops the item.
class AppUpdater : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    using QObject::QObject;

#ifdef MINNOTES_STORE_BUILD
    bool available() const { return false; }
#else
    bool available() const { return true; }
#endif

    Q_INVOKABLE void checkForUpdates() { mn::checkForUpdatesNow(); }
};

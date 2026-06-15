#pragma once
#include <QObject>
#include <QList>
#include <QVariantList>
#include <QVariantMap>
#include "BlockModel.h"

// Owns the set of open documents — one BlockModel per tab — and the active
// selection. Exposed to QML as the `docs` context property; main.cpp keeps the
// `blockModel` context property pointed at activeModel() (re-pointed on
// activeChanged) so the existing ~530 QML bindings keep working unchanged.
//
// Why one model per tab (not one model swapping documents in place): each tab
// must keep its own data, undo stack and dirty state, and switching must be
// instant (no reload). Background documents have no QML attached to them, so
// they render no media and play no video — the "don't render media in other
// tabs" requirement is satisfied for free, and the active tab's existing
// videoVisible→stopVideo() hook tears any player down when you switch away.
//
// A persistent empty fallback model backs the no-tabs welcome state, so the
// `blockModel` context property is never null (its documentOpen is false →
// the welcome overlay shows).
class DocumentManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY tabsChanged)
    Q_PROPERTY(int activeIndex READ activeIndex WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(BlockModel* activeModel READ activeModel NOTIFY activeChanged)
    // The open models in tab order — the tab strip's Repeater reads each one's
    // documentName / untitled directly.
    Q_PROPERTY(QVariantList models READ models NOTIFY tabsChanged)
public:
    explicit DocumentManager(QObject* parent = nullptr);

    int count() const { return static_cast<int>(tabs_.size()); }
    int activeIndex() const { return active_; }
    BlockModel* activeModel() const;
    QVariantList models() const;

    // Stable tab id at index `i` (survives index shifts when other tabs close),
    // or -1 if out of range. The per-tab view state is keyed by this id.
    Q_INVOKABLE int tabIdAt(int i) const;

    // Re-resolve media in every open tab (path mappings changed).
    Q_INVOKABLE void refreshMedia();

    Q_INVOKABLE void newTab();                            // fresh untitled scratch doc
    Q_INVOKABLE bool openTab(const QString& pathOrUrl);   // dedupe by canonical path
    Q_INVOKABLE void closeTab(int i);
    Q_INVOKABLE void setActive(int i);

    // Opaque per-tab QML view state (scroll offset, cursor, active sub-tab),
    // keyed by the stable tab id so it survives other tabs closing.
    Q_INVOKABLE QVariantMap viewState(int tabId) const;
    Q_INVOKABLE void setViewState(int tabId, const QVariantMap& m);

signals:
    void tabsChanged();    // a tab opened/closed (the strip + count rebuild)
    void activeChanged();  // the active tab changed (main.cpp re-points blockModel)

private:
    struct Tab { BlockModel* model = nullptr; int id = 0; QVariantMap view; };
    QList<Tab> tabs_;
    int active_ = -1;
    int nextId_ = 1;
    BlockModel* empty_ = nullptr;   // persistent no-document model (welcome state)

    // Canonical absolute path for an existing file (for openTab dedupe).
    static QString canonical(const QString& pathOrUrl);
    int indexOfPath(const QString& canonicalPath) const;
};

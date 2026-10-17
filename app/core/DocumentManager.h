#pragma once
#include <QObject>
#include <QList>
#include <QVariantList>
#include <QVariantMap>
#include "BlockModel.h"
#include "CanvasDocument.h"

// Owns the set of open FILES — one CanvasDocument per top tab — and the active
// selection. Exposed to QML as the `docs` context property; main.cpp keeps the
// `blockModel` context property pointed at activeModel() — the active canvas's
// FOCUSED PAGE model — and `canvas` at activeCanvas(), re-pointed on
// activeModelChanged / activeChanged, so the existing ~530 QML bindings keep
// working unchanged (2.0 stage 2: the file is a canvas, pages are its models).
//
// Why one canvas per tab (not one canvas swapping files in place): each tab
// must keep its own data, undo stacks and dirty state, and switching must be
// instant (no reload). Background canvases have no QML attached to them, so
// they render no media and play no video.
//
// Persistent empty fallbacks back the no-tabs welcome state, so neither context
// property is ever null (their documentOpen is false → the welcome overlay shows).
class DocumentManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY tabsChanged)
    Q_PROPERTY(int activeIndex READ activeIndex WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(BlockModel* activeModel READ activeModel NOTIFY activeModelChanged)
    Q_PROPERTY(CanvasDocument* activeCanvas READ activeCanvas NOTIFY activeChanged)
    // The open canvases in tab order — the tab strip's Repeater reads each one's
    // documentName / untitled / dirty directly (the names BlockModel exposed in 1.0).
    Q_PROPERTY(QVariantList models READ models NOTIFY tabsChanged)
    // Number of tabs with unsaved edits — drives the app-close guard.
    Q_PROPERTY(int dirtyCount READ dirtyCount NOTIFY dirtyCountChanged)
public:
    explicit DocumentManager(QObject* parent = nullptr);

    int count() const { return static_cast<int>(tabs_.size()); }
    int activeIndex() const { return active_; }
    // The active canvas's focused page model (never null: the empty model when no tab is open).
    BlockModel* activeModel() const;
    // The active canvas (never null: a never-opened canvas when no tab is open).
    CanvasDocument* activeCanvas() const;
    QVariantList models() const;

    int dirtyCount() const;
    // Index of the first tab with unsaved edits, or -1 if none (close guard focus).
    Q_INVOKABLE int firstDirtyIndex() const;
    // Save every titled dirty tab in place; returns true if no unsaved tab remains
    // (untitled docs and save conflicts can't be resolved here → false).
    Q_INVOKABLE bool saveAllTitled();

    // Stable tab id at index `i` (survives index shifts when other tabs close),
    // or -1 if out of range. The per-tab view state is keyed by this id.
    Q_INVOKABLE int tabIdAt(int i) const;
    // The tab's FOCUSED PAGE model by stable id (nullptr if the tab closed) — the
    // merge drag holds ids, not indices, so it survives tabs closing mid-drag.
    Q_INVOKABLE BlockModel* modelForTabId(int tabId) const;
    Q_INVOKABLE CanvasDocument* canvasForTabId(int tabId) const;
    // Tab i's focused page model (the merge drop target) / its canvas.
    Q_INVOKABLE BlockModel* modelAt(int i) const;
    Q_INVOKABLE CanvasDocument* canvasAt(int i) const;

    // Re-resolve media in every open tab (path mappings changed).
    Q_INVOKABLE void refreshMedia();

    Q_INVOKABLE void newTab();                            // fresh untitled canvas, one page, focused
    Q_INVOKABLE bool openTab(const QString& pathOrUrl);   // dedupe by canonical path
    // A QCView stream-notes folder (<project>/.qcview/streams/<name>__<id8>/,
    // holding notes.json + images/): opens <folder>/<name>.mnd, creating it
    // with every note inserted as blocks when it does not exist yet. The
    // tab remembers the folder (activeStreamDir) so the editor can offer the
    // notes QCView adds later.
    Q_INVOKABLE bool openStreamFolder(const QString& dir);
    Q_INVOKABLE QString streamDirAt(int i) const;
    Q_PROPERTY(QString activeStreamDir READ activeStreamDir NOTIFY activeChanged)
    QString activeStreamDir() const { return streamDirAt(active_); }
    // Why the last openTab failed ("" = no reason known) — the failed canvas is
    // gone by then, so its lastOpenError is kept here for QML.
    Q_INVOKABLE QString lastOpenError() const { return lastOpenError_; }
    Q_INVOKABLE void closeTab(int i);
    Q_INVOKABLE void setActive(int i);

    // Opaque per-tab QML view state (scroll offset, cursor, active sub-tab),
    // keyed by the stable tab id so it survives other tabs closing.
    Q_INVOKABLE QVariantMap viewState(int tabId) const;
    Q_INVOKABLE void setViewState(int tabId, const QVariantMap& m);

signals:
    void tabsChanged();    // a tab opened/closed (the strip + count rebuild)
    void activeChanged();  // the active TAB changed
    // activeModel() points at a different page model than before — a tab switch, a close, or a
    // focus change inside the active canvas (main.cpp re-points blockModel + followers).
    void activeModelChanged();
    // A focus change INSIDE the active canvas only (not a tab switch): the editor resets its page view.
    void activeFocusChanged();
    void dirtyCountChanged();  // any tab's unsaved-edits flag flipped

private:
    struct Tab { CanvasDocument* canvas = nullptr; int id = 0; QVariantMap view; QString streamDir; };
    QList<Tab> tabs_;
    int active_ = -1;
    int nextId_ = 1;
    QString lastOpenError_;
    BlockModel* empty_ = nullptr;           // persistent no-document model (welcome state)
    CanvasDocument* emptyCanvas_ = nullptr; // persistent never-opened canvas (welcome state)
    BlockModel* lastActive_ = nullptr;      // what activeModel() returned when activeModelChanged last fired

    CanvasDocument* makeCanvas();           // a wired-up canvas (dirty + focus signals)
    void syncActiveModel();                 // emit activeModelChanged if the pointer moved
    // Canonical absolute path for an existing file (for openTab dedupe).
    static QString canonical(const QString& pathOrUrl);
    int indexOfPath(const QString& canonicalPath) const;
};

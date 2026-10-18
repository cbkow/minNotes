#include "DocumentManager.h"
#include "../notes/VideoNotesModel.h"

#include <QFileInfo>
#include <QUrl>

DocumentManager::DocumentManager(QObject* parent) : QObject(parent) {
    // The no-document fallbacks: back the welcome state so `blockModel` and `canvas`
    // (which point here when there are no tabs) are never null.
    empty_ = new BlockModel(this);
    emptyCanvas_ = new CanvasDocument(this);
    lastActive_ = empty_;
}

CanvasDocument* DocumentManager::activeCanvas() const {
    if (active_ >= 0 && active_ < tabs_.size()) return tabs_[active_].canvas;
    return emptyCanvas_;
}

BlockModel* DocumentManager::activeModel() const {
    if (active_ >= 0 && active_ < tabs_.size())
        if (BlockModel* m = tabs_[active_].canvas->focusedModel()) return m;
    return empty_;
}

QVariantList DocumentManager::models() const {
    QVariantList out;
    out.reserve(tabs_.size());
    for (const Tab& t : tabs_)
        out << QVariant::fromValue(static_cast<QObject*>(t.canvas));
    return out;
}

int DocumentManager::tabIdAt(int i) const {
    return (i >= 0 && i < tabs_.size()) ? tabs_[i].id : -1;
}

CanvasDocument* DocumentManager::canvasForTabId(int tabId) const {
    for (const Tab& t : tabs_)
        if (t.id == tabId) return t.canvas;
    return nullptr;
}

BlockModel* DocumentManager::modelForTabId(int tabId) const {
    CanvasDocument* c = canvasForTabId(tabId);
    return c ? c->focusedModel() : nullptr;
}

CanvasDocument* DocumentManager::canvasAt(int i) const {
    return (i >= 0 && i < tabs_.size()) ? tabs_[i].canvas : nullptr;
}

BlockModel* DocumentManager::modelAt(int i) const {
    CanvasDocument* c = canvasAt(i);
    return c ? c->focusedModel() : nullptr;
}

int DocumentManager::dirtyCount() const {
    int n = 0;
    for (const Tab& t : tabs_) if (t.canvas->dirty()) ++n;
    return n;
}

int DocumentManager::firstDirtyIndex() const {
    for (int i = 0; i < tabs_.size(); ++i) if (tabs_[i].canvas->dirty()) return i;
    return -1;
}

bool DocumentManager::saveAllTitled() {
    for (const Tab& t : tabs_)
        if (t.canvas->dirty() && !t.canvas->untitled()) t.canvas->save();
    return dirtyCount() == 0;   // untitled docs + unresolved conflicts can't be saved here
}

QString DocumentManager::canonical(const QString& pathOrUrl) {
    QString path = pathOrUrl.startsWith(QLatin1String("file:"))
                 ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
    if (path.isEmpty()) return path;
    const QString c = QFileInfo(path).canonicalFilePath();   // "" if it doesn't exist
    return c.isEmpty() ? QFileInfo(path).absoluteFilePath() : c;
}

int DocumentManager::indexOfPath(const QString& canonicalPath) const {
    if (canonicalPath.isEmpty()) return -1;
    for (int i = 0; i < tabs_.size(); ++i) {
        const QString p = tabs_[i].canvas->documentPath();
        if (!p.isEmpty() && canonical(p) == canonicalPath) return i;
    }
    return -1;
}

void DocumentManager::refreshMedia() {
    for (const Tab& t : tabs_) t.canvas->refreshMedia();
}

void DocumentManager::syncActiveModel() {
    BlockModel* now = activeModel();
    if (now == lastActive_) return;
    lastActive_ = now;
    emit activeModelChanged();
}

CanvasDocument* DocumentManager::makeCanvas() {
    auto* c = new CanvasDocument(this);
    connect(c, &CanvasDocument::dirtyChanged, this, &DocumentManager::dirtyCountChanged);
    // A focus change inside the ACTIVE canvas re-points blockModel (page ↔ canvas, page → page);
    // background canvases change focus silently.
    connect(c, &CanvasDocument::focusChanged, this, [this, c] {
        if (activeCanvas() != c) return;
        const BlockModel* before = lastActive_;
        syncActiveModel();
        if (lastActive_ != before) emit activeFocusChanged();
    });
    // A LIVE page (a card edited in place) is the active model too — the same re-point (2.0 SPIKE S2).
    connect(c, &CanvasDocument::liveChanged, this, [this, c] {
        if (activeCanvas() != c) return;
        const BlockModel* before = lastActive_;
        syncActiveModel();
        if (lastActive_ != before) emit activeFocusChanged();
    });
    // Pages added / removed can move the focused model too (a removed focus page falls back).
    connect(c, &CanvasDocument::pagesChanged, this, [this, c] { if (activeCanvas() == c) syncActiveModel(); });
    return c;
}

void DocumentManager::newTab() {
    CanvasDocument* c = makeCanvas();
    if (!c->newCanvas()) { delete c; return; }
    tabs_.append({ c, nextId_++, {}, {} });
    emit tabsChanged();
    emit dirtyCountChanged();
    setActive(static_cast<int>(tabs_.size()) - 1);
}

bool DocumentManager::openTab(const QString& pathOrUrl) {
    lastOpenError_.clear();
    // Already open → just focus that tab (no duplicate window on the same DB).
    const int existing = indexOfPath(canonical(pathOrUrl));
    if (existing >= 0) { setActive(existing); return true; }

    CanvasDocument* c = makeCanvas();
    if (!c->open(pathOrUrl)) {
        lastOpenError_ = c->lastOpenError();
        delete c;
        return false;
    }
    tabs_.append({ c, nextId_++, {}, {} });
    emit tabsChanged();
    emit dirtyCountChanged();
    setActive(static_cast<int>(tabs_.size()) - 1);
    return true;
}

bool DocumentManager::openStreamFolder(const QString& dir) {
    lastOpenError_.clear();
    const QFileInfo fi(dir);
    if (!fi.isDir() || !QFileInfo::exists(dir + QStringLiteral("/notes.json"))) {
        lastOpenError_ = tr("Not a stream notes folder (no notes.json): %1").arg(dir);
        return false;
    }
    QString name = fi.fileName();
    const int sep = name.lastIndexOf(QStringLiteral("__"));
    if (sep > 0) name = name.left(sep);               // "<name>__<id8>" → "<name>"
    const QString docPath = dir + QLatin1Char('/') + name + QStringLiteral(".mnd");
    const bool fresh = !QFileInfo::exists(docPath);
    if (fresh) {
        newTab();
        BlockModel* m = activeModel();
        CanvasDocument* c = activeCanvas();
        if (!m || !c->documentOpen()) return false;
        const int n = VideoNotesModel::insertNotesFromDir(m, dir, 0);
        if (!c->saveAs(docPath)) {
            lastOpenError_ = tr("Could not create %1").arg(docPath);
            return false;
        }
        qInfo("DocumentManager: stream folder %s → new %s with %d note(s)", qPrintable(dir), qPrintable(docPath), n);
    } else if (!openTab(docPath)) {
        return false;
    }
    if (active_ >= 0 && active_ < tabs_.size()) tabs_[active_].streamDir = dir;
    emit activeChanged();
    return true;
}

QString DocumentManager::streamDirAt(int i) const {
    return (i >= 0 && i < tabs_.size()) ? tabs_[i].streamDir : QString();
}

void DocumentManager::closeTab(int i) {
    if (i < 0 || i >= tabs_.size()) return;
    Tab t = tabs_.takeAt(i);
    t.canvas->closeCanvas();
    t.canvas->deleteLater();

    // Keep the active tab pointed at a sensible neighbour (or the welcome state).
    if (tabs_.isEmpty())      active_ = -1;
    else if (active_ > i)     --active_;            // indices above shifted down
    else if (active_ == i)    active_ = qMin(i, static_cast<int>(tabs_.size()) - 1);
    // (active_ < i is unaffected)

    emit tabsChanged();
    emit dirtyCountChanged();
    emit activeChanged();
    syncActiveModel();
}

void DocumentManager::setActive(int i) {
    // Clamp to a valid tab, or -1 (welcome state) when there are no tabs.
    const int next = tabs_.isEmpty() ? -1
                   : qBound(0, i, static_cast<int>(tabs_.size()) - 1);
    if (next == active_) return;
    active_ = next;
    emit activeChanged();
    syncActiveModel();
}

QVariantMap DocumentManager::viewState(int tabId) const {
    for (const Tab& t : tabs_)
        if (t.id == tabId) return t.view;
    return {};
}

void DocumentManager::setViewState(int tabId, const QVariantMap& m) {
    for (Tab& t : tabs_)
        if (t.id == tabId) { t.view = m; return; }
}

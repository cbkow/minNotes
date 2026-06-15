#include "DocumentManager.h"
#include <QFileInfo>
#include <QUrl>

DocumentManager::DocumentManager(QObject* parent) : QObject(parent) {
    // The no-document fallback: backs the welcome state so `blockModel` (which
    // points here when there are no tabs) is never null.
    empty_ = new BlockModel(this);
}

BlockModel* DocumentManager::activeModel() const {
    if (active_ >= 0 && active_ < tabs_.size()) return tabs_[active_].model;
    return empty_;
}

QVariantList DocumentManager::models() const {
    QVariantList out;
    out.reserve(tabs_.size());
    for (const Tab& t : tabs_)
        out << QVariant::fromValue(static_cast<QObject*>(t.model));
    return out;
}

int DocumentManager::tabIdAt(int i) const {
    return (i >= 0 && i < tabs_.size()) ? tabs_[i].id : -1;
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
        const QString p = tabs_[i].model->documentPath();
        if (!p.isEmpty() && canonical(p) == canonicalPath) return i;
    }
    return -1;
}

void DocumentManager::newTab() {
    auto* m = new BlockModel(this);
    m->newDocument();
    tabs_.append({ m, nextId_++, {} });
    emit tabsChanged();
    setActive(static_cast<int>(tabs_.size()) - 1);
}

bool DocumentManager::openTab(const QString& pathOrUrl) {
    // Already open → just focus that tab (no duplicate window on the same DB).
    const int existing = indexOfPath(canonical(pathOrUrl));
    if (existing >= 0) { setActive(existing); return true; }

    auto* m = new BlockModel(this);
    if (!m->openDocument(pathOrUrl)) {
        delete m;
        return false;
    }
    tabs_.append({ m, nextId_++, {} });
    emit tabsChanged();
    setActive(static_cast<int>(tabs_.size()) - 1);
    return true;
}

void DocumentManager::closeTab(int i) {
    if (i < 0 || i >= tabs_.size()) return;
    Tab t = tabs_.takeAt(i);
    t.model->closeDocument();
    t.model->deleteLater();

    // Keep the active tab pointed at a sensible neighbour (or the welcome state).
    if (tabs_.isEmpty())      active_ = -1;
    else if (active_ > i)     --active_;            // indices above shifted down
    else if (active_ == i)    active_ = qMin(i, static_cast<int>(tabs_.size()) - 1);
    // (active_ < i is unaffected)

    emit tabsChanged();
    emit activeChanged();
}

void DocumentManager::setActive(int i) {
    // Clamp to a valid tab, or -1 (welcome state) when there are no tabs.
    const int next = tabs_.isEmpty() ? -1
                   : qBound(0, i, static_cast<int>(tabs_.size()) - 1);
    if (next == active_) return;
    active_ = next;
    emit activeChanged();
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

#include "PageContextLoader.h"
#include <QQmlEngine>

PageContextLoader::PageContextLoader(QQuickItem* parent) : QQuickItem(parent) {}

PageContextLoader::~PageContextLoader() { clear(); }

void PageContextLoader::setPageModel(QObject* m) {
    if (m == pageModel_) return;
    pageModel_ = m;
    emit pageModelChanged();
    rebuild();
}

void PageContextLoader::setSourceComponent(QQmlComponent* c) {
    if (c == component_) return;
    component_ = c;
    emit sourceComponentChanged();
    rebuild();
}

void PageContextLoader::setExtraContext(const QVariantMap& m) {
    extra_ = m;
    emit extraContextChanged();
    if (ctx_)
        for (auto it = extra_.constBegin(); it != extra_.constEnd(); ++it)
            ctx_->setContextProperty(it.key(), it.value());
}

void PageContextLoader::setActive(bool a) {
    if (a == active_) return;
    active_ = a;
    emit activeChanged();
    rebuild();
}

void PageContextLoader::componentComplete() {
    QQuickItem::componentComplete();
    rebuild();
}

void PageContextLoader::clear() {
    if (item_) { item_->setParentItem(nullptr); delete item_; item_ = nullptr; }
    if (ctx_) { delete ctx_; ctx_ = nullptr; }
}

void PageContextLoader::rebuild() {
    if (!isComponentComplete()) return;
    const bool had = item_ != nullptr;
    clear();
    if (active_ && pageModel_ && component_) {
        QQmlContext* parentCtx = qmlContext(this);
        if (parentCtx) {
            ctx_ = new QQmlContext(parentCtx, this);
            ctx_->setContextProperty(QStringLiteral("blockModel"), pageModel_);
            for (auto it = extra_.constBegin(); it != extra_.constEnd(); ++it)
                ctx_->setContextProperty(it.key(), it.value());
            QObject* o = component_->create(ctx_);
            item_ = qobject_cast<QQuickItem*>(o);
            if (item_) {
                item_->setParent(this);
                item_->setParentItem(this);
            } else {
                delete o;
            }
        }
    }
    if (had || item_) emit itemChanged();
}

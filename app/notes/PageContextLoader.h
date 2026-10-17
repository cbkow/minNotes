#pragma once
// A page card's context (2.0 stage 2 step 7). BlockView and MediaBlock read the `blockModel`
// context property directly in ~50 places; a NON-FOCUSED page on the canvas must render its OWN
// model without touching them. This loader instantiates `sourceComponent` in a child QQmlContext
// whose `blockModel` is the page's model (plus whatever `extraContext` names — a spell stub, so a
// background page never reads the focused page's issues), so every delegate inside resolves to
// the page it belongs to. Rebuilt when the model, the component or `active` change.
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class PageContextLoader : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* pageModel READ pageModel WRITE setPageModel NOTIFY pageModelChanged)
    Q_PROPERTY(QQmlComponent* sourceComponent READ sourceComponent WRITE setSourceComponent NOTIFY sourceComponentChanged)
    Q_PROPERTY(QVariantMap extraContext READ extraContext WRITE setExtraContext NOTIFY extraContextChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QQuickItem* item READ item NOTIFY itemChanged)
public:
    explicit PageContextLoader(QQuickItem* parent = nullptr);
    ~PageContextLoader() override;
    QObject* pageModel() const { return pageModel_; }
    void setPageModel(QObject* m);
    QQmlComponent* sourceComponent() const { return component_; }
    void setSourceComponent(QQmlComponent* c);
    QVariantMap extraContext() const { return extra_; }
    void setExtraContext(const QVariantMap& m);
    bool active() const { return active_; }
    void setActive(bool a);
    QQuickItem* item() const { return item_; }
signals:
    void pageModelChanged();
    void sourceComponentChanged();
    void extraContextChanged();
    void activeChanged();
    void itemChanged();
protected:
    void componentComplete() override;
private:
    void rebuild();
    void clear();
    QObject* pageModel_ = nullptr;
    QQmlComponent* component_ = nullptr;
    QVariantMap extra_;
    bool active_ = true;
    QQmlContext* ctx_ = nullptr;
    QQuickItem* item_ = nullptr;
};

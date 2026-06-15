#include "PathMapController.h"
#include "Document.h"          // makeUlid()
#include <QSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>

namespace {
constexpr auto kSettingsKey = "pathMappings/json";

QVector<mn::PathMapping> parseMappings(const QString& json) {
    QVector<mn::PathMapping> out;
    const QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        mn::PathMapping m;
        m.id      = o.value(QStringLiteral("id")).toString();
        m.label   = o.value(QStringLiteral("label")).toString();
        m.win     = o.value(QStringLiteral("win")).toString();
        m.mac     = o.value(QStringLiteral("mac")).toString();
        m.lin     = o.value(QStringLiteral("lin")).toString();
        m.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        if (m.id.isEmpty()) m.id = makeUlid();   // tolerate hand-edited / legacy rows
        out.push_back(m);
    }
    return out;
}

QString serializeMappings(const QVector<mn::PathMapping>& v) {
    QJsonArray arr;
    for (const mn::PathMapping& m : v) {
        QJsonObject o;
        o.insert(QStringLiteral("id"),      m.id);
        o.insert(QStringLiteral("label"),   m.label);
        o.insert(QStringLiteral("win"),     m.win);
        o.insert(QStringLiteral("mac"),     m.mac);
        o.insert(QStringLiteral("lin"),     m.lin);
        o.insert(QStringLiteral("enabled"), m.enabled);
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}
} // namespace

PathMapController::PathMapController(QObject* parent) : QObject(parent) {
    load();
}

void PathMapController::load() {
    QSettings s;
    const QString json = s.value(QLatin1String(kSettingsKey)).toString();
    mn::setActiveMappings(parseMappings(json));
}

void PathMapController::persist() {
    QSettings s;
    s.setValue(QLatin1String(kSettingsKey), serializeMappings(mn::activeMappings()));
}

QString PathMapController::mappingsJson() const {
    return serializeMappings(mn::activeMappings());
}

void PathMapController::setMappingsJson(const QString& json) {
    mn::setActiveMappings(parseMappings(json));
    persist();
    emit mappingsChanged();
}

QString PathMapController::newId() const { return makeUlid(); }

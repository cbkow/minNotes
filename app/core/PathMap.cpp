#include "PathMap.h"
#include "MinPaths.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// The rules live in min-paths `map` (minplatform canon §22, row 121): list order, disabled rows skipped, a root matches
// only on a component boundary, `\` compares as a separator on every OS (the older apps' rule) and the stored path is
// never rewritten, resolution joins with '/' on every OS. This file keeps minNotes' API and feeds the crate's
// process-global table.
namespace mn {

QString currentOsTag() { return mp::take(mp_os_tag()); }

namespace {
QVector<PathMapping> g_mappings;

QString serialize(const QVector<PathMapping>& v) {
    QJsonArray arr;
    for (const PathMapping& m : v) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), m.id); o.insert(QStringLiteral("label"), m.label);
        o.insert(QStringLiteral("win"), m.win); o.insert(QStringLiteral("mac"), m.mac); o.insert(QStringLiteral("lin"), m.lin);
        o.insert(QStringLiteral("enabled"), m.enabled);
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}
// A JSON value as text (QJsonDocument only speaks objects/arrays at the top level, so a string rides in a one-item array).
QString valueText(const QJsonValue& v) {
    if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    const QByteArray a = QJsonDocument(QJsonArray{v.toString()}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(a.mid(1, a.size() - 2));
}
QJsonValue textValue(const QString& t) {
    if (t.startsWith(QLatin1Char('{'))) return QJsonDocument::fromJson(t.toUtf8()).object();
    return QJsonDocument::fromJson((QStringLiteral("[") + t + QStringLiteral("]")).toUtf8()).array().at(0);
}
} // namespace

const QVector<PathMapping>& activeMappings() { return g_mappings; }
void setActiveMappings(QVector<PathMapping> mappings) {
    g_mappings = std::move(mappings);
    mp_mappings_set(mp::u8(serialize(g_mappings)).constData());
}

QJsonValue toRef(const QString& absPath) {
    if (absPath.isEmpty()) return QJsonValue(absPath);
    return textValue(mp::take(mp_to_ref(mp::u8(absPath).constData())));
}

QString resolveRef(const QJsonValue& src) {
    if (!src.isObject()) { const QString s = src.toString(); if (s.isEmpty()) return s; }
    return mp::take(mp_resolve_ref(mp::u8(valueText(src)).constData()));
}

} // namespace mn

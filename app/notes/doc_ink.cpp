#include "doc_ink.h"
#include "annotation_serializer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace mn {

QString docInkToJson(const DocInkAnchor& anchor)
{
    // "" is the delete-the-row signal: only when the anchor holds NOTHING —
    // a text-only anchor must not self-delete.
    if (anchor.strokes.empty() && anchor.texts.empty()) return {};
    QJsonObject root;
    root.insert(QStringLiteral("version"), QStringLiteral("2.0"));
    root.insert(QStringLiteral("coordinate_system"), QStringLiteral("block-local"));
    root.insert(QStringLiteral("space"),
                anchor.space == DocInkAnchor::Frame ? QStringLiteral("frame")
                                                    : QStringLiteral("px"));
    QJsonArray shapes;
    for (const qcv::ActiveStroke& s : anchor.strokes)
        shapes.append(qcv::AnnotationSerializer::strokeToJson(s));
    root.insert(QStringLiteral("shapes"), shapes);
    if (!anchor.texts.empty()) {   // omitted when empty: old blobs stay byte-identical
        QJsonArray texts;
        for (const mn::SketchTextSpec& t : anchor.texts)
            texts.append(mn::sketchTextToJson(t));
        root.insert(QStringLiteral("texts"), texts);
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool docInkFromJson(const QString& json, DocInkAnchor& out)
{
    out.space = DocInkAnchor::Px;
    out.strokes.clear();
    out.texts.clear();
    if (json.isEmpty()) return true;

    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toString() != QLatin1String("2.0"))
        return false;
    if (root.value(QStringLiteral("coordinate_system")).toString()
        != QLatin1String("block-local"))
        return false;

    const QString space = root.value(QStringLiteral("space")).toString();
    out.space = (space == QLatin1String("frame")) ? DocInkAnchor::Frame
                                                  : DocInkAnchor::Px;
    const QJsonArray shapes = root.value(QStringLiteral("shapes")).toArray();
    out.strokes.reserve(size_t(shapes.size()));
    for (const QJsonValue& v : shapes) {
        qcv::ActiveStroke s;
        if (qcv::AnnotationSerializer::jsonToStroke(v.toObject(), s))
            out.strokes.push_back(std::move(s));
    }
    out.texts = mn::parseSketchTexts(root);
    return true;
}

bool docInkHasStrokes(const QString& json)
{
    DocInkAnchor a;
    return docInkFromJson(json, a) && !a.strokes.empty();
}

bool docInkHasContent(const QString& json)
{
    DocInkAnchor a;
    return docInkFromJson(json, a) && (!a.strokes.empty() || !a.texts.empty());
}

} // namespace mn

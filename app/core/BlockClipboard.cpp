#include "BlockClipboard.h"
#include "PackageFormat.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace BlockClipboard {

namespace {

// Embed a JSON-string field as an object when it parses (no double-escaped
// strings on the wire); otherwise keep the raw string under `<key>Raw`.
void putJsonOrRaw(QJsonObject& o, const QString& key, const QString& json) {
    if (json.isEmpty()) return;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    if (d.isObject()) o.insert(key, d.object());
    else if (d.isArray()) o.insert(key, d.array());
    else o.insert(key + QStringLiteral("Raw"), json);
}

QString takeJsonOrRaw(const QJsonObject& o, const QString& key) {
    const QJsonValue v = o.value(key);
    if (v.isObject())
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isArray())
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    return o.value(key + QStringLiteral("Raw")).toString();
}

} // namespace

QJsonObject specToJson(const BlockModel::BlockSpec& sp, const QString& ink) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), int(sp.type));
    if (sp.level) o.insert(QStringLiteral("level"), int(sp.level));
    if (sp.taskState) o.insert(QStringLiteral("task"), int(sp.taskState));
    if (sp.depth) o.insert(QStringLiteral("depth"), int(sp.depth));
    if (!sp.lang.isEmpty()) o.insert(QStringLiteral("lang"), sp.lang);
    if (!sp.mediaJson.isEmpty()) {
        putJsonOrRaw(o, QStringLiteral("media"), sp.mediaJson);
    } else if (sp.type == BlockModel::Table) {
        putJsonOrRaw(o, QStringLiteral("table"), sp.tableJson);
    } else {
        o.insert(QStringLiteral("text"), sp.text);
        if (!sp.spans.empty()) {
            QJsonArray spans;
            for (const BlockModel::Span& s : sp.spans) {
                QJsonObject so;
                so.insert(QStringLiteral("s"), s.s);
                so.insert(QStringLiteral("e"), s.e);
                so.insert(QStringLiteral("k"), QString::fromLatin1(BlockModel::spanKindToString(s.kind)));
                if (BlockModel::spanHasPayloadKind(s.kind) && !s.href.isEmpty())
                    so.insert(QStringLiteral("u"), s.href);
                spans.push_back(so);
            }
            o.insert(QStringLiteral("spans"), spans);
        }
    }
    if (!ink.isEmpty()) putJsonOrRaw(o, QStringLiteral("ink"), ink);
    return o;
}

BlockModel::BlockSpec specFromJson(const QJsonObject& o, QString* ink) {
    BlockModel::BlockSpec sp;
    sp.type = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("type")).toInt(), 0, 255));
    sp.level = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("level")).toInt(), 0, 255));
    sp.taskState = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("task")).toInt(), 0, 255));
    sp.depth = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("depth")).toInt(), 0, 255));
    sp.lang = o.value(QStringLiteral("lang")).toString();
    const QString media = takeJsonOrRaw(o, QStringLiteral("media"));
    const QString table = takeJsonOrRaw(o, QStringLiteral("table"));
    if (!media.isEmpty()) {
        sp.type = BlockModel::Media;
        sp.mediaJson = media;
    } else if (sp.type == BlockModel::Table || !table.isEmpty()) {
        sp.type = BlockModel::Table;
        sp.tableJson = table;
    } else {
        sp.text = o.value(QStringLiteral("text")).toString();
        const int len = sp.text.size();
        for (const QJsonValue& v : o.value(QStringLiteral("spans")).toArray()) {
            const QJsonObject so = v.toObject();
            const uint8_t k = BlockModel::spanKindFromString(so.value(QStringLiteral("k")).toString());
            if (!k) continue;                                  // unknown kind → dropped
            const int s = std::clamp(so.value(QStringLiteral("s")).toInt(), 0, len);
            const int e = std::clamp(so.value(QStringLiteral("e")).toInt(), 0, len);
            if (s >= e) continue;
            sp.spans.push_back({s, e, k, so.value(QStringLiteral("u")).toString()});
        }
    }
    if (ink) *ink = takeJsonOrRaw(o, QStringLiteral("ink"));
    return sp;
}

QByteArray encode(const Payload& p) {
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("minnotes-blocks"));
    root.insert(QStringLiteral("version"), p.version);
    QJsonObject source;
    source.insert(QStringLiteral("docPath"), p.docPath);
    source.insert(QStringLiteral("docDir"), p.docDir);
    source.insert(QStringLiteral("package"), p.package);
    source.insert(QStringLiteral("pageWidth"), p.pageWidth);
    root.insert(QStringLiteral("source"), source);
    QJsonArray blocks;
    for (size_t i = 0; i < p.specs.size(); ++i)
        blocks.push_back(specToJson(p.specs[i], i < p.ink.size() ? p.ink[i] : QString()));
    root.insert(QStringLiteral("blocks"), blocks);
    QJsonArray threads;
    for (const BlockModel::ThreadImport& t : p.threads) {
        QJsonObject to;
        to.insert(QStringLiteral("id"), t.id);
        to.insert(QStringLiteral("created"), double(t.created));
        to.insert(QStringLiteral("resolved"), t.resolved);
        QJsonArray msgs;
        for (const BlockModel::ThreadMessage& m : t.messages) {
            QJsonObject mo;
            mo.insert(QStringLiteral("body"), m.body);
            mo.insert(QStringLiteral("created"), double(m.created));
            mo.insert(QStringLiteral("modified"), double(m.modified));
            msgs.push_back(mo);
        }
        to.insert(QStringLiteral("messages"), msgs);
        threads.push_back(to);
    }
    root.insert(QStringLiteral("threads"), threads);
    QJsonArray assets;
    for (const Asset& a : p.assets) {
        QJsonObject ao;
        ao.insert(QStringLiteral("rel"), a.rel);
        ao.insert(QStringLiteral("abs"), a.abs);
        ao.insert(QStringLiteral("entry"), a.entry);
        ao.insert(QStringLiteral("bytes"), double(a.bytes));
        ao.insert(QStringLiteral("video"), a.video);
        assets.push_back(ao);
    }
    root.insert(QStringLiteral("assets"), assets);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool decode(const QByteArray& json, Payload* out, QString* error) {
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    if (!out) return fail(QStringLiteral("no output"));
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject())
        return fail(QStringLiteral("malformed payload"));
    const QJsonObject root = d.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("minnotes-blocks"))
        return fail(QStringLiteral("not a minNotes blocks payload"));
    const int version = root.value(QStringLiteral("version")).toInt();
    if (version < 1 || version > kVersion)
        return fail(QStringLiteral("unsupported payload version %1").arg(version));
    Payload p;
    p.version = version;
    const QJsonObject source = root.value(QStringLiteral("source")).toObject();
    p.docPath = source.value(QStringLiteral("docPath")).toString();
    p.docDir = source.value(QStringLiteral("docDir")).toString();
    p.package = source.value(QStringLiteral("package")).toString();
    p.pageWidth = source.value(QStringLiteral("pageWidth")).toDouble(760);
    if (p.pageWidth <= 0) p.pageWidth = 760;
    for (const QJsonValue& v : root.value(QStringLiteral("blocks")).toArray()) {
        QString ink;
        p.specs.push_back(specFromJson(v.toObject(), &ink));
        p.ink.push_back(ink);
    }
    for (const QJsonValue& v : root.value(QStringLiteral("threads")).toArray()) {
        const QJsonObject to = v.toObject();
        BlockModel::ThreadImport t;
        t.id = to.value(QStringLiteral("id")).toString();
        t.created = qint64(to.value(QStringLiteral("created")).toDouble());
        t.resolved = to.value(QStringLiteral("resolved")).toBool();
        for (const QJsonValue& mv : to.value(QStringLiteral("messages")).toArray()) {
            const QJsonObject mo = mv.toObject();
            t.messages.push_back({QString(), mo.value(QStringLiteral("body")).toString(),
                                  qint64(mo.value(QStringLiteral("created")).toDouble()),
                                  qint64(mo.value(QStringLiteral("modified")).toDouble())});
        }
        if (!t.id.isEmpty()) p.threads.push_back(std::move(t));
    }
    for (const QJsonValue& v : root.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject ao = v.toObject();
        Asset a;
        a.rel = ao.value(QStringLiteral("rel")).toString();
        a.abs = ao.value(QStringLiteral("abs")).toString();
        a.entry = ao.value(QStringLiteral("entry")).toString();
        a.bytes = qint64(ao.value(QStringLiteral("bytes")).toDouble());
        a.video = ao.value(QStringLiteral("video")).toBool();
        if (!a.rel.isEmpty()) p.assets.push_back(std::move(a));
    }
    *out = std::move(p);
    return true;
}

AssetTransfer::Source assetSource(const Payload& p) {
    AssetTransfer::Source s;
    s.package = p.package;
    QHash<QString, Asset> byRel;
    for (const Asset& a : p.assets) byRel.insert(a.rel, a);
    const QHash<QString, qint64> entries =
        (s.package.isEmpty() || !QFileInfo::exists(s.package))
            ? QHash<QString, qint64>() : mnpkg::entrySizes(s.package);
    s.resolve = [byRel, entries](const QString& rel) -> PackageExporter::Resolved {
        PackageExporter::Resolved r;
        const auto it = byRel.constFind(rel);
        if (it == byRel.constEnd()) return r;
        r.absPath = it->abs;
        if (!it->entry.isEmpty()) {                    // still sealed in the source package
            const auto e = entries.constFind(it->entry);
            if (e == entries.constEnd()) return r;
            r.packageEntry = it->entry;
            r.bytes = e.value();
            r.ok = true;
            return r;
        }
        const QFileInfo fi(it->abs);
        if (!fi.isFile()) return r;                    // gone since copy time → honest broken ref
        r.bytes = fi.size();
        r.ok = true;
        return r;
    };
    return s;
}

} // namespace BlockClipboard

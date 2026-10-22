#include "GuideBuilder.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {
struct Section { const char* file; };
// Reading order: files & the canvas · writing · media · in / out · the keys.
const Section kSections[] = {
    { "basics" }, { "canvas" },
    { "editing" }, { "tables" }, { "spelling" },
    { "media" }, { "annotations" },
    { "import" }, { "export" }, { "packages" },
    { "shortcuts" },
};
}

GuideBuilder::GuideBuilder(QObject* parent) : QObject(parent) {}
GuideBuilder::~GuideBuilder() = default;

// The guide folder is named by the app version AND a stamp over what it is built from (the sections' text, the images'
// bytes, the layout's format number): a docs edit or a layout change in a build of the same version rebuilds the guide
// instead of reopening a stale one (user 2026-10-22: "do we need to get rid of the old one first?" — no).
namespace { constexpr int kGuideFormat = 2; }   // 1 = frames + cards (2026-10-22 am) · 2 = pages with inline images
QString GuideBuilder::stamp() const {
    if (!stamp_.isEmpty()) return stamp_;
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArrayLiteral("format:") + QByteArray::number(kGuideFormat));
    for (const Section& s : kSections) {
        QFile f(QStringLiteral(":/guide/") + QLatin1String(s.file) + QStringLiteral(".md"));
        if (f.open(QIODevice::ReadOnly)) h.addData(f.readAll());
    }
    QDir imgs(QStringLiteral(":/guide/images"));
    for (const QString& n : imgs.entryList(QDir::Files, QDir::Name)) {
        QFile f(imgs.filePath(n));
        if (f.open(QIODevice::ReadOnly)) { h.addData(n.toUtf8()); h.addData(f.readAll()); }
    }
    stamp_ = QString::fromLatin1(h.result().toHex().left(10));
    return stamp_;
}
QString GuideBuilder::guideDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Guide/") + QStringLiteral(MINNOTES_APP_VERSION) + QLatin1Char('-') + stamp();
}
QString GuideBuilder::guidePath() const {
    QDir().mkpath(guideDir());
    return guideDir() + QStringLiteral("/Guide.mnd");
}
bool GuideBuilder::exists() const { return QFileInfo::exists(guideDir() + QStringLiteral("/Guide.mnd")); }
bool GuideBuilder::remove() { QDir d(guideDir()); return !d.exists() || d.removeRecursively(); }
void GuideBuilder::sweepOldVersions() {
    QDir root(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Guide"));
    const QString keep = QStringLiteral(MINNOTES_APP_VERSION) + QLatin1Char('-') + stamp();
    for (const QString& v : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (v != keep) QDir(root.filePath(v)).removeRecursively();
}

QString GuideBuilder::unpack() {
    cleanup();
    scratch_ = std::make_unique<QTemporaryDir>();
    if (!scratch_->isValid()) { scratch_.reset(); return QString(); }
    const QString dir = scratch_->path();
    QDir().mkpath(dir + QStringLiteral("/images"));
    static const QRegularExpression frontMatter(QStringLiteral("\\A---\\n.*?\\n---\\n"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression comment(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression image(QStringLiteral("!\\[[^\\]]*\\]\\(([^)\\s]+)\\)[ \\t]*"));
    static const QRegularExpression localLink(QStringLiteral("(?<!!)\\[([^\\]]+)\\]\\((?!https?://)[^)]*\\)"));   // (not an image's ![alt](src))
    static const QRegularExpression h1(QStringLiteral("^# +(.+?)\\s*$"), QRegularExpression::MultilineOption);
    for (const Section& s : kSections) {
        QFile in(QStringLiteral(":/guide/") + QLatin1String(s.file) + QStringLiteral(".md"));
        if (!in.open(QIODevice::ReadOnly)) continue;
        QString md = QString::fromUtf8(in.readAll());
        md.remove(frontMatter);
        md.remove(comment);
        QStringList images;
        for (auto it = image.globalMatch(md); it.hasNext();) {
            const QString ref = it.next().captured(1);
            const QString base = QFileInfo(ref).fileName();
            const QString res = QStringLiteral(":/guide/images/") + base, out = dir + QStringLiteral("/images/") + base;
            if (!QFile::exists(res) || images.contains(out)) continue;
            if (QFile::exists(out) || QFile::copy(res, out)) {
                QFile::setPermissions(out, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
                images << out;
            }
        }
        md.replace(localLink, QStringLiteral("\\1"));
        const QString title = h1.match(md).captured(1).trimmed();
        const QString path = dir + QStringLiteral("/") + QLatin1String(s.file) + QStringLiteral(".md");
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        out.write(md.toUtf8());
        out.close();
        QVariantMap m;
        m.insert(QStringLiteral("file"), QLatin1String(s.file));
        m.insert(QStringLiteral("title"), title.isEmpty() ? QString::fromLatin1(s.file) : title);
        m.insert(QStringLiteral("path"), path);
        m.insert(QStringLiteral("images"), images);
        sections_ << m;
    }
    return sections_.isEmpty() ? QString() : dir;
}
QVariantList GuideBuilder::sections() const { return sections_; }
void GuideBuilder::cleanup() { scratch_.reset(); sections_.clear(); }

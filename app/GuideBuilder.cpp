#include "GuideBuilder.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {
struct Section { const char* file; const char* tint; };
// Reading order + an area tint (the Frame panel's chips): files & the canvas · writing · media · in / out · the keys.
const Section kSections[] = {
    { "basics",      "#3f7fc0" }, { "canvas",   "#3f7fc0" },
    { "editing",     "#4a9a6a" }, { "tables",   "#4a9a6a" }, { "spelling", "#4a9a6a" },
    { "media",       "#d08a2e" }, { "annotations", "#d08a2e" },
    { "import",      "#8a5ac0" }, { "export",   "#8a5ac0" }, { "packages", "#8a5ac0" },
    { "shortcuts",   "#c7b24a" },
};
}

GuideBuilder::GuideBuilder(QObject* parent) : QObject(parent) {}
GuideBuilder::~GuideBuilder() = default;

QString GuideBuilder::guideDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Guide/") + QStringLiteral(MINNOTES_APP_VERSION);
}
QString GuideBuilder::guidePath() const {
    QDir().mkpath(guideDir());
    return guideDir() + QStringLiteral("/Guide.mnd");
}
bool GuideBuilder::exists() const { return QFileInfo::exists(guideDir() + QStringLiteral("/Guide.mnd")); }
bool GuideBuilder::remove() { QDir d(guideDir()); return !d.exists() || d.removeRecursively(); }
void GuideBuilder::sweepOldVersions() {
    QDir root(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Guide"));
    const QString keep = QStringLiteral(MINNOTES_APP_VERSION);
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
    static const QRegularExpression localLink(QStringLiteral("\\[([^\\]]+)\\]\\((?!https?://)[^)]*\\)"));
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
        md.remove(image);
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
        m.insert(QStringLiteral("tint"), QLatin1String(s.tint));
        m.insert(QStringLiteral("path"), path);
        m.insert(QStringLiteral("images"), images);
        sections_ << m;
    }
    return sections_.isEmpty() ? QString() : dir;
}
QVariantList GuideBuilder::sections() const { return sections_; }
void GuideBuilder::cleanup() { scratch_.reset(); sections_.clear(); }

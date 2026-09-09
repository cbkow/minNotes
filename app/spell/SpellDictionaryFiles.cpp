#include "SpellDictionaryFiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

namespace spell {

QString spellingDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/spelling");
}

QString ensureDictionaryExtracted(const QString& version, QString* error) {
    const QString base = spellingDir();
    const QString dir = base + QLatin1Char('/') + (version.isEmpty() ? QStringLiteral("dev") : version);
    QDir().mkpath(dir);
    for (const char* name : {"en.aff", "en.dic"}) {
        const QString dst = dir + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(dst)) continue;
        QFile::remove(dst);
        if (!QFile::copy(QStringLiteral(":/spell/en/") + QLatin1String(name), dst)) {
            if (error) *error = QStringLiteral("could not extract ") + QLatin1String(name);
            return {};
        }
        QFile(dst).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    }
    // Prune sibling version dirs (a version bump leaves the old copy behind).
    for (const QString& sub : QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (base + QLatin1Char('/') + sub != dir) QDir(base + QLatin1Char('/') + sub).removeRecursively();
    return dir;
}

QStringList loadUserDictionary() {
    QStringList out;
    QFile f(spellingDir() + QStringLiteral("/user.dic"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString w = in.readLine().trimmed();
        if (!w.isEmpty()) out << w;
    }
    return out;
}

bool appendUserDictionary(const QString& word) {
    const QString w = word.trimmed();
    if (w.isEmpty()) return false;
    QDir().mkpath(spellingDir());
    QFile f(spellingDir() + QStringLiteral("/user.dic"));
    if (!f.open(QIODevice::Append | QIODevice::Text)) return false;
    QTextStream out(&f);
    out << w << '\n';
    return true;
}

QByteArray loadRulesXml() {
    QFile f(QStringLiteral(":/spell/en/grammar-rules.xml"));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace spell

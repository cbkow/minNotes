#include "PathMap.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QRegularExpression>

namespace mn {

QString currentOsTag() {
#if defined(Q_OS_WIN)
    return QStringLiteral("win");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("mac");
#else
    return QStringLiteral("lin");
#endif
}

namespace {

QVector<PathMapping> g_mappings;

// The root prefix for `os` from a mapping ("" if that OS is unset).
const QString& prefixFor(const PathMapping& m, const QString& os) {
    if (os == QLatin1String("win")) return m.win;
    if (os == QLatin1String("mac")) return m.mac;
    return m.lin;
}

bool osIsWindows(const QString& os) { return os == QLatin1String("win"); }
QChar sepFor(const QString& os)    { return osIsWindows(os) ? QLatin1Char('\\') : QLatin1Char('/'); }

// Normalize for PREFIX COMPARISON only (never for slicing/splitting):
// Windows treats '/' and '\\' alike and is case-insensitive; POSIX is exact
// (a backslash is a legal filename char, so it must NOT be touched there).
QString normForCompare(const QString& s, const QString& os) {
    if (!osIsWindows(os)) return s;
    QString r = s;
    r.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return r.toLower();
}

// Trailing separators (either kind) trimmed — so root + sep + rel doesn't double.
QString trimTrailingSeps(const QString& s) {
    int n = s.size();
    while (n > 0 && (s[n - 1] == QLatin1Char('/') || s[n - 1] == QLatin1Char('\\'))) --n;
    return s.left(n);
}

// Split a path remainder into segments using the SOURCE os's separator rule:
// Windows splits on both '\\' and '/'; POSIX on '/' only (keeps literal '\').
QStringList splitSegments(const QString& remainder, const QString& os) {
    QString r = remainder;
    // Drop any leading separators first.
    while (!r.isEmpty() && (r.front() == QLatin1Char('/') ||
                            (osIsWindows(os) && r.front() == QLatin1Char('\\'))))
        r.remove(0, 1);
    QStringList parts = osIsWindows(os)
        ? r.split(QRegularExpression(QStringLiteral("[\\\\/]")), Qt::SkipEmptyParts)
        : r.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts;
}

// If `absPath` (in `srcOs` form) starts with `root` (same OS form), return the
// remainder segments via *out and true; else false. Comparison honors the OS's
// case/separator rules; slicing uses the original string so segments are exact.
bool matchUnderRoot(const QString& absPath, const QString& root,
                    const QString& srcOs, QStringList* out) {
    if (root.isEmpty()) return false;
    const QString cmpPath = normForCompare(absPath, srcOs);
    const QString cmpRoot = trimTrailingSeps(normForCompare(root, srcOs));
    if (cmpRoot.isEmpty()) return false;
    if (!cmpPath.startsWith(cmpRoot)) return false;
    // The next char must be a boundary (end, or a separator) so "/Vol/share"
    // doesn't spuriously match "/Vol/share-extra".
    if (cmpPath.size() > cmpRoot.size()) {
        const QChar c = cmpPath.at(cmpRoot.size());
        if (c != QLatin1Char('/') && c != QLatin1Char('\\')) return false;
    }
    // Slice the ORIGINAL path by the (un-normalized) trimmed root length. Root
    // and path are the same OS form here, so the trimmed-root length matches the
    // original path's prefix length up to trailing-separator differences; recompute
    // against the original root to stay byte-accurate.
    const QString origRoot = trimTrailingSeps(root);
    const QString remainder = absPath.mid(origRoot.size());
    *out = splitSegments(remainder, srcOs);
    return true;
}

} // namespace

const QVector<PathMapping>& activeMappings() { return g_mappings; }
void setActiveMappings(QVector<PathMapping> mappings) { g_mappings = std::move(mappings); }

QJsonValue toRef(const QString& absPath) {
    if (absPath.isEmpty()) return QJsonValue(absPath);
    const QString os = currentOsTag();
    for (const PathMapping& m : g_mappings) {
        if (!m.enabled) continue;
        QStringList segs;
        if (matchUnderRoot(absPath, prefixFor(m, os), os, &segs)) {
            QJsonArray rel;
            for (const QString& s : segs) rel.append(s);
            QJsonObject ref;
            ref.insert(QStringLiteral("vol"), m.id);
            ref.insert(QStringLiteral("rel"), rel);
            return ref;
        }
    }
    // Outside every mapped volume → keep the absolute path (stays machine-local).
    return QJsonValue(absPath);
}

QString resolveRef(const QJsonValue& src) {
    const QString os = currentOsTag();

    if (src.isObject()) {
        const QJsonObject o = src.toObject();
        const QString vol = o.value(QStringLiteral("vol")).toString();
        const QJsonArray rel = o.value(QStringLiteral("rel")).toArray();
        for (const PathMapping& m : g_mappings) {
            if (m.id != vol) continue;
            if (!m.enabled) return QString();           // volume turned off → unresolved
            const QString root = trimTrailingSeps(prefixFor(m, os));
            if (root.isEmpty()) return QString();        // no root for this OS
            QString path = root;
            const QChar sep = sepFor(os);
            for (const QJsonValue& seg : rel)
                path += sep + seg.toString();
            return path;
        }
        return QString();                                // unknown volume → unresolved
    }

    // String src: best-effort cross-OS translate for legacy absolute paths.
    const QString s = src.toString();
    if (s.isEmpty()) return s;
    static const QStringList kAll = { QStringLiteral("win"), QStringLiteral("mac"),
                                      QStringLiteral("lin") };
    for (const PathMapping& m : g_mappings) {
        if (!m.enabled) continue;
        for (const QString& srcOs : kAll) {
            if (srcOs == os) continue;                   // same-OS path needs no swap
            QStringList segs;
            if (matchUnderRoot(s, prefixFor(m, srcOs), srcOs, &segs)) {
                const QString root = trimTrailingSeps(prefixFor(m, os));
                if (root.isEmpty()) continue;            // no target root — try other rules
                QString path = root;
                const QChar sep = sepFor(os);
                for (const QString& seg : segs) path += sep + seg;
                return path;
            }
        }
    }
    return s;                                            // no mapping matched → as-is
}

} // namespace mn

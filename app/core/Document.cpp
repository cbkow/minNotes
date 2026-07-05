#include "Document.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDateTime>
#include <QRandomGenerator>
#include <QDebug>

// Recorded in doc_meta.app_version on save. The app target defines it from
// PROJECT_VERSION; targets that compile this file without the define (tests)
// fall back.
#ifndef MINNOTES_APP_VERSION
#  define MINNOTES_APP_VERSION "0.0.0"
#endif

namespace {
int g_conn_seq = 0;
}

QString makeUlid() {
    // 48-bit ms timestamp + 80-bit randomness, Crockford base32 (26 chars).
    static const char kEnc[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    quint64 ts = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

    char out[26];
    // Time: 10 chars (50 bits) from the 48-bit ms timestamp, most-significant
    // first → ids sort by creation time.
    for (int i = 9; i >= 0; --i) { out[i] = kEnc[ts & 0x1F]; ts >>= 5; }
    // Randomness: 16 chars (80 bits), one 5-bit draw each.
    for (int i = 10; i < 26; ++i)
        out[i] = kEnc[QRandomGenerator::global()->bounded(32)];
    return QString::fromLatin1(out, 26);
}

Document::~Document() {
    close();
}

void Document::close() {
    if (!open_) return;
    QSqlDatabase::database(conn_).close();
    QSqlDatabase::removeDatabase(conn_);
    open_ = false;
    conn_.clear();
}

bool Document::checkpoint() {
    if (!open_) return false;
    return exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
}

bool Document::vacuumInto(const QString& path) const {
    if (!open_) return false;
    QString p = path; p.replace(QLatin1Char('\''), QStringLiteral("''"));   // SQL-escape
    return exec(QStringLiteral("VACUUM INTO '%1'").arg(p));
}

bool Document::exec(const QString& sql) const {
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (!q.exec(sql)) {
        qWarning() << "Document SQL failed:" << q.lastError().text() << "\n  " << sql;
        return false;
    }
    return true;
}

bool Document::open(const QString& path) {
    close();   // switching files: drop any current connection first
    conn_ = QStringLiteral("mn_doc_%1").arg(++g_conn_seq);
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn_);
    db.setDatabaseName(path);
    if (!db.open()) {
        qWarning() << "Document: cannot open" << path << db.lastError().text();
        return false;
    }
    open_ = true;

    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");

    exec(R"(CREATE TABLE IF NOT EXISTS blocks (
              id       TEXT PRIMARY KEY,
              rank     TEXT NOT NULL,
              depth    INTEGER NOT NULL DEFAULT 0,
              type     TEXT NOT NULL,
              attrs    TEXT,
              content  TEXT NOT NULL DEFAULT '',
              created  INTEGER,
              modified INTEGER
          ))");
    exec("CREATE INDEX IF NOT EXISTS blocks_order ON blocks(rank, depth, type)");

    exec(R"(CREATE TABLE IF NOT EXISTS doc_meta (
              id INTEGER PRIMARY KEY CHECK (id = 1),
              title TEXT, schema_version INTEGER, app_version TEXT,
              created INTEGER, modified INTEGER, last_cursor TEXT
          ))");

    // Sentinel: confirm the connection is actually usable. A WAL-stamped DB on a
    // filesystem that can't back the -shm shared memory (smbfs/NFS) opens but then
    // wedges — every pragma/DDL above returns SQLITE_CANTOPEN. Fail cleanly here so
    // the caller shows "couldn't open" instead of an empty, broken document (which
    // a media-row query would then null-deref). The editor never opens documents
    // over such filesystems directly — it edits a local working copy — but a corrupt
    // working copy must still fail gracefully.
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (!q.exec(QStringLiteral("SELECT count(*) FROM sqlite_master")) || !q.next()) {
        qWarning() << "Document: connection unusable after open" << path
                   << q.lastError().text();
        close();
        return false;
    }

    // Soft format gate: a doc stamped by a NEWER format still opens, but say
    // so — editing it with this build may drop fields the newer format added.
    if (const int v = schemaVersion(); v > kSchemaVersion)
        qWarning() << "Document:" << path << "uses format v" << v
                   << "(this build writes v" << kSchemaVersion
                   << ") — newer fields may be lost on save";
    return true;
}

void Document::stampMeta() {
    if (!open_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(QStringLiteral(
        "INSERT INTO doc_meta (id, schema_version, app_version, created, modified) "
        "VALUES (1, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET schema_version = excluded.schema_version, "
        "app_version = excluded.app_version, modified = excluded.modified"));
    q.addBindValue(kSchemaVersion);
    q.addBindValue(QStringLiteral(MINNOTES_APP_VERSION));
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec())
        qWarning() << "Document: doc_meta stamp failed:" << q.lastError().text();
}

int Document::schemaVersion() const {
    if (!open_) return 0;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (q.exec(QStringLiteral("SELECT schema_version FROM doc_meta WHERE id = 1")) && q.next())
        return q.value(0).toInt();
    return 0;
}

int Document::count() const {
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (q.exec("SELECT COUNT(*) FROM blocks") && q.next())
        return q.value(0).toInt();
    return 0;
}

std::vector<Document::BlockMeta> Document::skinnyScan() const {
    std::vector<BlockMeta> out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    // Index-only on blocks_order — no `content`, the whole point of the scan.
    if (!q.exec("SELECT id, rank, depth, type, attrs FROM blocks ORDER BY rank"))
        return out;
    out.reserve(static_cast<size_t>(count()));
    while (q.next()) {
        BlockMeta m;
        m.id = q.value(0).toString();
        m.rank = q.value(1).toString();
        m.depth = q.value(2).toInt();
        m.type = q.value(3).toString();
        m.attrs = q.value(4).toString();
        out.push_back(std::move(m));
    }
    return out;
}

QString Document::contentFor(const QString& id) const {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("SELECT content FROM blocks WHERE id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

void Document::begin()  { exec("BEGIN"); }
void Document::commit() { exec("COMMIT"); }

void Document::appendBlock(const QString& id, const QString& rank, int depth,
                           const QString& type, const QString& attrs, const QString& content) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("INSERT INTO blocks (id, rank, depth, type, attrs, content, created, modified) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    q.addBindValue(id);
    q.addBindValue(rank);
    q.addBindValue(depth);
    q.addBindValue(type);
    q.addBindValue(attrs);
    // content is NOT NULL in the schema. A null QString (e.g. a freshly inserted
    // empty paragraph/divider — insertBlock/insertDivider pass QString()) binds as
    // SQL NULL and the INSERT fails the constraint, so the row is never created;
    // a later updateContent (a plain UPDATE) then finds no row and the block's
    // typed content is lost on reopen. Coalesce null → "" so empty blocks persist.
    q.addBindValue(content.isNull() ? QString(QLatin1String("")) : content);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec())
        qWarning() << "Document appendBlock failed:" << q.lastError().text();
}

void Document::updateContent(const QString& id, const QString& content) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("UPDATE blocks SET content = ?, modified = ? WHERE id = ?");
    q.addBindValue(content);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document updateContent failed:" << q.lastError().text();
}

void Document::updateMeta(const QString& id, const QString& type, const QString& attrs) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("UPDATE blocks SET type = ?, attrs = ?, modified = ? WHERE id = ?");
    q.addBindValue(type);
    q.addBindValue(attrs);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document updateMeta failed:" << q.lastError().text();
}

void Document::updateRank(const QString& id, const QString& rank) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("UPDATE blocks SET rank = ?, modified = ? WHERE id = ?");
    q.addBindValue(rank);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document updateRank failed:" << q.lastError().text();
}

void Document::deleteBlock(const QString& id) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("DELETE FROM blocks WHERE id = ?");
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document deleteBlock failed:" << q.lastError().text();
}

#include "Document.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDateTime>
#include <QRandomGenerator>
#include <QDebug>

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
    if (open_) {
        QSqlDatabase::database(conn_).close();
        QSqlDatabase::removeDatabase(conn_);
    }
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
    return true;
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
    q.addBindValue(content);
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

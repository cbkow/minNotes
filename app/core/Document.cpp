#include "Document.h"
#include "PerfCounters.h"   // dbWrites counter (type probe)
#include <atomic>
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
// Atomic: import workers open headless Documents off the GUI thread, and
// connection names must stay unique across threads.
std::atomic<int> g_conn_seq{0};
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

// ---------------------------------------------------------------------------
// The shared connection

Document::Conn::~Conn() {
    if (txnDepth > 0) {   // a handle let go mid-transaction: never leave the file locked
        qWarning() << "Document: connection" << name << "closed with" << txnDepth << "open transaction level(s)";
        QSqlQuery q(QSqlDatabase::database(name));
        q.exec(QStringLiteral("ROLLBACK"));
    }
    QSqlDatabase::database(name).close();
    QSqlDatabase::removeDatabase(name);
}

Document::~Document() {
    close();
}

void Document::close() {
    conn_.reset();   // the last handle's reset runs Conn::~Conn
}

QString Document::connectionName() const {
    return conn_ ? conn_->name : QString();
}

QSqlDatabase Document::db() const {
    return conn_ ? QSqlDatabase::database(conn_->name) : QSqlDatabase();
}

Document Document::scoped(const QString& pageId) const {
    Document d(*this);
    d.pageScope_ = pageId;
    return d;
}

bool Document::checkpoint() {
    if (!isOpen()) return false;
    return exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
}

bool Document::vacuumInto(const QString& path) const {
    if (!isOpen()) return false;
    QString p = path; p.replace(QLatin1Char('\''), QStringLiteral("''"));   // SQL-escape
    return exec(QStringLiteral("VACUUM INTO '%1'").arg(p));
}

bool Document::exec(const QString& sql) const {
    if (!isOpen()) return false;
    QSqlQuery q(db());
    if (!q.exec(sql)) {
        qWarning() << "Document SQL failed:" << q.lastError().text() << "\n  " << sql;
        return false;
    }
    return true;
}

bool Document::open(const QString& path) {
    close();   // switching files: drop this handle's current connection first
    pageScope_.clear();
    const QString name = QStringLiteral("mn_doc_%1").arg(++g_conn_seq);
    QSqlDatabase sdb = QSqlDatabase::addDatabase("QSQLITE", name);
    sdb.setDatabaseName(path);
    if (!sdb.open()) {
        qWarning() << "Document: cannot open" << path << sdb.lastError().text();
        QSqlDatabase::removeDatabase(name);
        return false;
    }
    conn_ = std::make_shared<Conn>(name);

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
              created INTEGER, modified INTEGER, last_cursor TEXT,
              page_width INTEGER, format TEXT
          ))");

    // v2: document annotations. block_ink = ONE row per anchored block, the
    // whole serialized stroke blob (block-local coordinate envelope, see
    // app/notes/doc_ink.h). The FK cascade means deleting a block row drops
    // its ink even in older builds editing this doc (foreign_keys=ON above is
    // set by every version). Comments: threads + messages, cascade likewise.
    exec(R"(CREATE TABLE IF NOT EXISTS block_ink (
              block_id TEXT PRIMARY KEY REFERENCES blocks(id) ON DELETE CASCADE,
              ink      TEXT NOT NULL,
              modified INTEGER
          ))");
    exec(R"(CREATE TABLE IF NOT EXISTS comment_threads (
              id       TEXT PRIMARY KEY,
              created  INTEGER,
              resolved INTEGER NOT NULL DEFAULT 0
          ))");
    exec(R"(CREATE TABLE IF NOT EXISTS comment_messages (
              id        TEXT PRIMARY KEY,
              thread_id TEXT NOT NULL REFERENCES comment_threads(id) ON DELETE CASCADE,
              body      TEXT NOT NULL,
              created   INTEGER,
              modified  INTEGER
          ))");

    // Sentinel: confirm the connection is actually usable. A WAL-stamped DB on a
    // filesystem that can't back the -shm shared memory (smbfs/NFS) opens but then
    // wedges — every pragma/DDL above returns SQLITE_CANTOPEN. Fail cleanly here so
    // the caller shows "couldn't open" instead of an empty, broken document (which
    // a media-row query would then null-deref). The editor never opens documents
    // over such filesystems directly — it edits a local working copy — but a corrupt
    // working copy must still fail gracefully.
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("SELECT count(*) FROM sqlite_master")) || !q.next()) {
        qWarning() << "Document: connection unusable after open" << path
                   << q.lastError().text();
        q.finish();
        close();
        return false;
    }

    // Soft format gate: a doc stamped by a NEWER format still opens, but say
    // so — editing it with this build may drop fields the newer format added.
    // Only a .mnd file can be newer; an earlier minNotes file (no marker, its
    // own version numbering) is refused by BlockModel instead.
    if (const int v = schemaVersion(); v > kSchemaVersion && format() == QLatin1String(kFormat))
        qWarning() << "Document:" << path << "uses format v" << v
                   << "(this build writes v" << kSchemaVersion
                   << ") — newer fields may be lost on save";
    return true;
}

void Document::stampMeta() {
    if (!isOpen()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO doc_meta (id, schema_version, app_version, created, modified, format) "
        "VALUES (1, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET schema_version = excluded.schema_version, "
        "app_version = excluded.app_version, modified = excluded.modified, "
        "format = excluded.format"));
    q.addBindValue(kSchemaVersion);
    q.addBindValue(QStringLiteral(MINNOTES_APP_VERSION));
    q.addBindValue(now);
    q.addBindValue(now);
    q.addBindValue(QString::fromLatin1(kFormat));
    if (!q.exec())
        qWarning() << "Document: doc_meta stamp failed:" << q.lastError().text();
}

QString Document::format() const {
    if (!isOpen()) return {};
    QSqlQuery q(db());
    // An earlier minNotes file has no format column at all — the query fails,
    // which reads as "no marker".
    if (q.exec(QStringLiteral("SELECT format FROM doc_meta WHERE id = 1")) && q.next())
        return q.value(0).toString();
    return {};
}

int Document::schemaVersion() const {
    if (!isOpen()) return 0;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT schema_version FROM doc_meta WHERE id = 1")) && q.next())
        return q.value(0).toInt();
    return 0;
}

int Document::pageWidth() const {
    if (!isOpen()) return 760;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT page_width FROM doc_meta WHERE id = 1")) && q.next()) {
        const int w = q.value(0).toInt();
        if (w > 0) return w;
    }
    return 760;   // absent/0 = the classic measure (every pre-v3 doc)
}

int Document::count() const {
    if (!isOpen()) return 0;
    QSqlQuery q(db());
    if (isScoped()) {
        q.prepare("SELECT COUNT(*) FROM blocks WHERE page_id = ?");
        q.addBindValue(pageScope_);
        if (q.exec() && q.next()) return q.value(0).toInt();
        return 0;
    }
    if (q.exec("SELECT COUNT(*) FROM blocks") && q.next())
        return q.value(0).toInt();
    return 0;
}

std::vector<Document::BlockMeta> Document::skinnyScan() const {
    std::vector<BlockMeta> out;
    if (!isOpen()) return out;
    QSqlQuery q(db());
    // Index-only on blocks_order (blocks_page_order when scoped) — no `content`, the whole
    // point of the scan.
    bool ok;
    if (isScoped()) {
        q.prepare("SELECT id, rank, depth, type, attrs FROM blocks WHERE page_id = ? ORDER BY rank");
        q.addBindValue(pageScope_);
        ok = q.exec();
    } else {
        ok = q.exec("SELECT id, rank, depth, type, attrs FROM blocks ORDER BY rank");
    }
    if (!ok) return out;
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
    if (!isOpen()) return {};
    QSqlQuery q(db());
    q.prepare("SELECT content FROM blocks WHERE id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

// Transactions nest on the shared connection: the outermost level is a real BEGIN/COMMIT,
// inner levels are SAVEPOINTs, so a page model's own begin/commit inside a canvas-level
// transaction (or two page models interleaving) never trips "cannot start a transaction
// within a transaction".
void Document::begin() {
    if (!isOpen()) return;
    const int d = conn_->txnDepth;
    if (d == 0) exec("BEGIN");
    else exec(QStringLiteral("SAVEPOINT mn_sp_%1").arg(d));
    ++conn_->txnDepth;
}

void Document::commit() {
    if (!isOpen()) return;
    if (conn_->txnDepth <= 0) { qWarning() << "Document: commit without begin"; return; }
    const int d = --conn_->txnDepth;
    if (d == 0) exec("COMMIT");
    else exec(QStringLiteral("RELEASE mn_sp_%1").arg(d));
}

void Document::rollback() {
    if (!isOpen()) return;
    if (conn_->txnDepth <= 0) { qWarning() << "Document: rollback without begin"; return; }
    const int d = --conn_->txnDepth;
    if (d == 0) exec("ROLLBACK");
    else {
        exec(QStringLiteral("ROLLBACK TO mn_sp_%1").arg(d));
        exec(QStringLiteral("RELEASE mn_sp_%1").arg(d));
    }
}

int Document::transactionDepth() const {
    return conn_ ? conn_->txnDepth : 0;
}

void Document::appendBlock(const QString& id, const QString& rank, int depth,
                           const QString& type, const QString& attrs, const QString& content) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    if (isScoped())
        q.prepare("INSERT INTO blocks (id, rank, depth, type, attrs, content, created, modified, page_id) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    else
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
    if (isScoped()) q.addBindValue(pageScope_);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document appendBlock failed:" << q.lastError().text();
}

void Document::updateContent(const QString& id, const QString& content) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE blocks SET content = ?, modified = ? WHERE id = ?");
    // Same coalesce as appendBlock: an emptied block often holds a NULL QString (clear(),
    // left(0)…), which binds as SQL NULL — the UPDATE then fails the NOT NULL constraint
    // and the block's OLD text survives on disk, coming back on reopen.
    q.addBindValue(content.isNull() ? QString(QLatin1String("")) : content);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document updateContent failed:" << q.lastError().text();
}

void Document::updateMeta(const QString& id, const QString& type, const QString& attrs, int depth) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE blocks SET type = ?, attrs = ?, depth = ?, modified = ? WHERE id = ?");
    q.addBindValue(type);
    q.addBindValue(attrs);
    q.addBindValue(depth);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document updateMeta failed:" << q.lastError().text();
}

void Document::updateRank(const QString& id, const QString& rank) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE blocks SET rank = ?, modified = ? WHERE id = ?");
    q.addBindValue(rank);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document updateRank failed:" << q.lastError().text();
}

void Document::deleteBlock(const QString& id) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("DELETE FROM blocks WHERE id = ?");
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document deleteBlock failed:" << q.lastError().text();
}

QHash<QString, QString> Document::allInk() const {
    QHash<QString, QString> out;
    if (!isOpen()) return out;
    QSqlQuery q(db());
    if (!q.exec("SELECT block_id, ink FROM block_ink")) return out;
    while (q.next())
        out.insert(q.value(0).toString(), q.value(1).toString());
    return out;
}

void Document::deleteInk(const QString& blockId) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("DELETE FROM block_ink WHERE block_id = ?");
    q.addBindValue(blockId);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document deleteInk failed:" << q.lastError().text();
}

// ---------------------------------------------------------------------------
// Canvas schema (2.0 stage 2)

bool Document::hasColumn(const QString& table, const QString& column) const {
    if (!isOpen()) return false;
    QSqlQuery q(db());
    QString t = table; t.replace(QLatin1Char('\''), QStringLiteral("''"));
    if (!q.exec(QStringLiteral("PRAGMA table_info('%1')").arg(t))) return false;
    while (q.next())
        if (q.value(1).toString() == column) return true;
    return false;
}

bool Document::hasCanvasSchema() const {
    if (!isOpen()) return false;
    QSqlQuery q(db());
    if (!q.exec("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'elements'") || !q.next())
        return false;
    return hasColumn(QStringLiteral("blocks"), QStringLiteral("page_id"));
}

void Document::ensureCanvasSchema() {
    if (!isOpen()) return;
    exec(R"(CREATE TABLE IF NOT EXISTS elements (
              id        TEXT PRIMARY KEY,
              kind      TEXT NOT NULL,
              x         REAL NOT NULL DEFAULT 0,
              y         REAL NOT NULL DEFAULT 0,
              w         REAL NOT NULL DEFAULT 0,
              h         REAL NOT NULL DEFAULT 0,
              z         INTEGER NOT NULL DEFAULT 0,
              parent_id TEXT REFERENCES elements(id) ON DELETE SET NULL,
              json      TEXT NOT NULL DEFAULT '{}',
              created   INTEGER,
              modified  INTEGER
          ))");
    exec("CREATE INDEX IF NOT EXISTS elements_z ON elements(z, created)");
    exec("CREATE TABLE IF NOT EXISTS canvas_meta (key TEXT PRIMARY KEY, value TEXT)");
    // SQLite allows ADD COLUMN with a REFERENCES clause only when the default is NULL (it is).
    if (!hasColumn(QStringLiteral("blocks"), QStringLiteral("page_id")))
        exec("ALTER TABLE blocks ADD COLUMN page_id TEXT REFERENCES elements(id) ON DELETE CASCADE");
    exec("CREATE INDEX IF NOT EXISTS blocks_page_order ON blocks(page_id, rank, depth, type)");
}

namespace {
Document::Element readElement(const QSqlQuery& q) {
    Document::Element e;
    e.id = q.value(0).toString();
    e.kind = q.value(1).toString();
    e.x = q.value(2).toDouble();
    e.y = q.value(3).toDouble();
    e.w = q.value(4).toDouble();
    e.h = q.value(5).toDouble();
    e.z = q.value(6).toInt();
    e.parentId = q.value(7).toString();
    e.json = q.value(8).toString();
    e.created = q.value(9).toLongLong();
    e.modified = q.value(10).toLongLong();
    return e;
}
constexpr const char* kElementCols = "id, kind, x, y, w, h, z, parent_id, json, created, modified";
}

std::vector<Document::Element> Document::allElements() const {
    std::vector<Element> out;
    if (!isOpen()) return out;
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("SELECT %1 FROM elements ORDER BY z, created, id").arg(QLatin1String(kElementCols))))
        return out;
    while (q.next()) out.push_back(readElement(q));
    return out;
}

std::optional<Document::Element> Document::element(const QString& id) const {
    if (!isOpen()) return std::nullopt;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT %1 FROM elements WHERE id = ?").arg(QLatin1String(kElementCols)));
    q.addBindValue(id);
    if (q.exec() && q.next()) return readElement(q);
    return std::nullopt;
}

int Document::elementCount() const {
    if (!isOpen()) return 0;
    QSqlQuery q(db());
    if (q.exec("SELECT COUNT(*) FROM elements") && q.next()) return q.value(0).toInt();
    return 0;
}

void Document::insertElement(const Element& e) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("INSERT INTO elements (id, kind, x, y, w, h, z, parent_id, json, created, modified) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    q.addBindValue(e.id);
    q.addBindValue(e.kind);
    q.addBindValue(e.x);
    q.addBindValue(e.y);
    q.addBindValue(e.w);
    q.addBindValue(e.h);
    q.addBindValue(e.z);
    q.addBindValue(e.parentId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(e.parentId));
    q.addBindValue(e.json.isEmpty() ? QStringLiteral("{}") : e.json);
    q.addBindValue(e.created > 0 ? e.created : now);
    q.addBindValue(e.modified > 0 ? e.modified : now);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document insertElement failed:" << q.lastError().text();
}

void Document::updateElementRect(const QString& id, double x, double y, double w, double h) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE elements SET x = ?, y = ?, w = ?, h = ?, modified = ? WHERE id = ?");
    q.addBindValue(x);
    q.addBindValue(y);
    q.addBindValue(w);
    q.addBindValue(h);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document updateElementRect failed:" << q.lastError().text();
}

void Document::updateElementJson(const QString& id, const QString& json) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE elements SET json = ?, modified = ? WHERE id = ?");
    q.addBindValue(json.isEmpty() ? QStringLiteral("{}") : json);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document updateElementJson failed:" << q.lastError().text();
}

void Document::updateElementZ(const QString& id, int z) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE elements SET z = ?, modified = ? WHERE id = ?");
    q.addBindValue(z);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document updateElementZ failed:" << q.lastError().text();
}

void Document::updateElementParent(const QString& id, const QString& parentId) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE elements SET parent_id = ?, modified = ? WHERE id = ?");
    q.addBindValue(parentId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(parentId));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document updateElementParent failed:" << q.lastError().text();
}

void Document::deleteElement(const QString& id) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("DELETE FROM elements WHERE id = ?");   // a page's blocks cascade (FK)
    q.addBindValue(id);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document deleteElement failed:" << q.lastError().text();
}

std::vector<Document::RawBlock> Document::rawBlocksOfPage(const QString& pageId) const {
    std::vector<RawBlock> out;
    if (!isOpen()) return out;
    QSqlQuery q(db());
    q.prepare("SELECT id, rank, depth, type, attrs, content, created, modified, page_id "
              "FROM blocks WHERE page_id = ? ORDER BY rank");
    q.addBindValue(pageId);
    if (!q.exec()) return out;
    while (q.next()) {
        RawBlock r;
        r.id = q.value(0).toString();
        r.rank = q.value(1).toString();
        r.depth = q.value(2).toInt();
        r.type = q.value(3).toString();
        r.attrs = q.value(4).toString();
        r.content = q.value(5).toString();
        r.created = q.value(6).toLongLong();
        r.modified = q.value(7).toLongLong();
        r.pageId = q.value(8).toString();
        out.push_back(std::move(r));
    }
    return out;
}

void Document::restoreRawBlocks(const std::vector<RawBlock>& rows) {
    if (!isOpen() || rows.empty()) return;
    QSqlQuery q(db());
    q.prepare("INSERT OR REPLACE INTO blocks (id, rank, depth, type, attrs, content, created, modified, page_id) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    for (const RawBlock& r : rows) {
        q.addBindValue(r.id);
        q.addBindValue(r.rank);
        q.addBindValue(r.depth);
        q.addBindValue(r.type);
        q.addBindValue(r.attrs);
        q.addBindValue(r.content.isNull() ? QString(QLatin1String("")) : r.content);
        q.addBindValue(r.created);
        q.addBindValue(r.modified);
        q.addBindValue(r.pageId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(r.pageId));
        ++mn::perf::counters().dbWrites;
        if (!q.exec())
            qWarning() << "Document restoreRawBlocks failed:" << q.lastError().text();
    }
}

void Document::assignOrphanBlocks(const QString& pageId) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE blocks SET page_id = ? WHERE page_id IS NULL");
    q.addBindValue(pageId);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document assignOrphanBlocks failed:" << q.lastError().text();
}

QString Document::canvasMeta(const QString& key) const {
    if (!isOpen()) return {};
    QSqlQuery q(db());
    q.prepare("SELECT value FROM canvas_meta WHERE key = ?");
    q.addBindValue(key);
    if (q.exec() && q.next()) return q.value(0).toString();
    return {};
}

void Document::setCanvasMeta(const QString& key, const QString& value) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("INSERT INTO canvas_meta (key, value) VALUES (?, ?) "
              "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    q.addBindValue(key);
    q.addBindValue(value);
    ++mn::perf::counters().dbWrites;
    if (!q.exec())
        qWarning() << "Document setCanvasMeta failed:" << q.lastError().text();
}

// ---------------------------------------------------------------------------
// Comments

std::vector<Document::CommentThread> Document::commentThreads() const {
    std::vector<CommentThread> out;
    if (!isOpen()) return out;
    QSqlQuery q(db());
    if (!q.exec("SELECT id, created, resolved FROM comment_threads ORDER BY created"))
        return out;
    while (q.next()) {
        CommentThread t;
        t.id = q.value(0).toString();
        t.created = q.value(1).toLongLong();
        t.resolved = q.value(2).toInt() != 0;
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<Document::CommentMessage> Document::commentMessages(const QString& threadId) const {
    std::vector<CommentMessage> out;
    if (!isOpen()) return out;
    QSqlQuery q(db());
    q.prepare("SELECT id, body, created, modified FROM comment_messages "
              "WHERE thread_id = ? ORDER BY created");
    q.addBindValue(threadId);
    if (!q.exec()) return out;
    while (q.next()) {
        CommentMessage m;
        m.id = q.value(0).toString();
        m.threadId = threadId;
        m.body = q.value(1).toString();
        m.created = q.value(2).toLongLong();
        m.modified = q.value(3).toLongLong();
        out.push_back(std::move(m));
    }
    return out;
}

void Document::createThread(const QString& id) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("INSERT OR IGNORE INTO comment_threads (id, created, resolved) VALUES (?, ?, 0)");
    q.addBindValue(id);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec())
        qWarning() << "Document createThread failed:" << q.lastError().text();
}

// Timestamp-preserving writers (2026-08-20, the tab-merge program): a
// thread migrated from another document keeps its history honest.
void Document::createThreadAt(const QString& id, qint64 created, bool resolved) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("INSERT OR IGNORE INTO comment_threads (id, created, resolved) VALUES (?, ?, ?)");
    q.addBindValue(id);
    q.addBindValue(created > 0 ? created : QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(resolved ? 1 : 0);
    if (!q.exec())
        qWarning() << "Document createThreadAt failed:" << q.lastError().text();
}

void Document::insertMessageAt(const QString& id, const QString& threadId,
                               const QString& body, qint64 created, qint64 modified) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("INSERT INTO comment_messages (id, thread_id, body, created, modified) "
              "VALUES (?, ?, ?, ?, ?)");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    q.addBindValue(id);
    q.addBindValue(threadId);
    q.addBindValue(body);
    q.addBindValue(created > 0 ? created : now);
    q.addBindValue(modified > 0 ? modified : now);
    if (!q.exec())
        qWarning() << "Document insertMessageAt failed:" << q.lastError().text();
}

void Document::deleteThread(const QString& id) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("DELETE FROM comment_threads WHERE id = ?");   // messages cascade (FK)
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document deleteThread failed:" << q.lastError().text();
}

void Document::setThreadResolved(const QString& id, bool resolved) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE comment_threads SET resolved = ? WHERE id = ?");
    q.addBindValue(resolved ? 1 : 0);
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document setThreadResolved failed:" << q.lastError().text();
}

void Document::insertMessage(const QString& id, const QString& threadId, const QString& body) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("INSERT INTO comment_messages (id, thread_id, body, created, modified) "
              "VALUES (?, ?, ?, ?, ?)");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    q.addBindValue(id);
    q.addBindValue(threadId);
    q.addBindValue(body);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec())
        qWarning() << "Document insertMessage failed:" << q.lastError().text();
}

void Document::updateMessage(const QString& id, const QString& body) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("UPDATE comment_messages SET body = ?, modified = ? WHERE id = ?");
    q.addBindValue(body);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document updateMessage failed:" << q.lastError().text();
}

void Document::deleteMessage(const QString& id) {
    if (!isOpen()) return;
    QSqlQuery q(db());
    q.prepare("DELETE FROM comment_messages WHERE id = ?");
    q.addBindValue(id);
    if (!q.exec())
        qWarning() << "Document deleteMessage failed:" << q.lastError().text();
}

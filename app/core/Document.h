#pragma once
#include <QHash>
#include <QString>
#include <memory>
#include <optional>
#include <vector>

class QSqlDatabase;

// SQLite-canonical document store (DESIGN.md §3–4). The document file *is* this
// database. Owns the `blocks` table; BlockModel reads its eager skinny-scan
// (layout columns in rank order) and lazily fetches content per id.
//
// 2.0 (stage 2): a Document is a HANDLE. Copies share one ref-counted SQLite connection
// (the last copy closes it) and a handle may carry a PAGE SCOPE: scoped `skinnyScan`/`count`
// add `WHERE page_id = ?`, scoped `appendBlock` writes `page_id`; an unscoped handle is
// exactly the 1.0 store (tests, importers, the converter, exporters). Transactions nest —
// `begin/commit` past depth 0 become SAVEPOINTs — so two page models on one connection can
// each wrap their own writes. The canvas tables (`elements`, `canvas_meta`, `blocks.page_id`)
// are created ONLY by `ensureCanvasSchema()`; a plain `open()` still writes a 1.0 file
// (the version flip is the CanvasDocument step).
class Document {
public:
    // One row of the skinny-scan: the layout columns, marker-free `type` as text.
    struct BlockMeta {
        QString id;
        QString rank;
        int     depth = 0;
        QString type;     // "paragraph" | "heading" | "code" | "media" | ...
        QString attrs;    // JSON; empty for now
    };

    Document() = default;
    Document(const Document&) = default;              // shares the connection
    Document& operator=(const Document&) = default;
    ~Document();

    // Open (creating if absent) at `path`; apply pragmas + ensure the 1.0 schema. Drops THIS
    // handle's current connection first (so one handle can switch files); other handles on
    // the old connection keep it alive.
    bool open(const QString& path);
    // Drop this handle's reference. The connection closes when the last handle lets go.
    void close();
    bool isOpen() const { return conn_ != nullptr; }
    // The Qt connection name (tests, diagnostics); "" when closed.
    QString connectionName() const;
    // Flush the WAL into the main file so the .mnd is self-contained (Save).
    bool checkpoint();
    // Write a clean, compacted copy of the DB to `path` (Save As). Does not
    // switch — the caller re-opens the new path.
    bool vacuumInto(const QString& path) const;

    // --- Page scope (2.0) ---
    // A handle on the SAME connection whose block scan/count/append are limited to one page
    // element. `scoped("")` returns an unscoped handle.
    Document scoped(const QString& pageId) const;
    QString pageScope() const { return pageScope_; }
    bool isScoped() const { return !pageScope_.isEmpty(); }

    // --- Format versioning (doc_meta, id=1) ---
    // The .mnd format this build writes. 2.0 (the canvas root): doc_meta.format == "mnd2" at
    // schema 2 = the canvas schema (elements / canvas_meta / blocks.page_id) over the 1.0 block
    // store; the format STRING changed so every shipped 1.0 build refuses a 2.0 file outright
    // (its gate is `format != "mnd"`). A 1.0 file ("mnd" v1) is adopted read-only by
    // CanvasDocument and becomes 2.0 on Save As; anything else is refused, never migrated.
    // Bump kSchemaVersion on any incompatible change; CanvasDocument refuses a NEWER one.
    static constexpr int kSchemaVersion = 2;
    static constexpr const char* kFormat = "mnd2";
    static constexpr const char* kFormatV1 = "mnd";   // the 1.0 marker
    // Upsert the doc_meta row: `created` is written once, then every stamp
    // updates schema_version + app_version + modified. Called on the save
    // paths so a saved file always records what wrote it.
    void stampMeta();
    // schema_version of the open doc; 0 = pre-versioning legacy (never stamped).
    int schemaVersion() const;
    // doc_meta.format of the open doc; "" = not a .mnd file (an earlier
    // minNotes file, or a database this app didn't write).
    QString format() const;
    // The 1.0 per-document page measure (doc_meta.page_width; 760 when absent). Nothing in the
    // app reads it any more (the page is responsive, 2.0 stage 1C); the 2.0 converter reads it
    // once to size the page element. Never written.
    int pageWidth() const;

    int count() const;   // scoped: this page's blocks

    // Eager skinny-scan: layout columns only, already in visible (rank) order. Scoped: one page.
    std::vector<BlockMeta> skinnyScan() const;
    // Lazy fat fetch of one block's content.
    QString contentFor(const QString& id) const;

    // --- Write path (Phase 1b) ---
    // Wrap a batch of writes in begin/commit (also used for fast bulk seeding). Nest freely:
    // depth 0 is BEGIN/COMMIT, deeper levels are SAVEPOINTs on the shared connection.
    void begin();
    void commit();
    void rollback();   // the innermost open level (ROLLBACK TO + RELEASE past depth 0)
    int transactionDepth() const;
    void appendBlock(const QString& id, const QString& rank, int depth,
                     const QString& type, const QString& attrs, const QString& content);
    void updateContent(const QString& id, const QString& content);
    void updateMeta(const QString& id, const QString& type, const QString& attrs, int depth = 0);
    void updateRank(const QString& id, const QString& rank);   // reorder
    void deleteBlock(const QString& id);

    // --- The 1.0 block_ink table (block-pinned margin ink, REMOVED in 2.0 stage 1). Read once at
    // load to move a media block's frame ink into its descriptor and left otherwise for the 2.0
    // converter; never written. The table is still created so older files keep their FK cascade.
    QHash<QString, QString> allInk() const;
    void deleteInk(const QString& blockId);

    // --- Canvas schema (2.0 stage 2). Additive over blocks/comments: `elements` (pages, media,
    // ink, shapes, connectors), `canvas_meta` (camera, focus page, defaults) and `blocks.page_id`
    // (FK → elements, ON DELETE CASCADE: removing a page element removes its blocks). Idempotent;
    // a file that already has the tables is left alone. Not called by open().
    void ensureCanvasSchema();
    bool hasCanvasSchema() const;   // elements table + blocks.page_id present
    bool hasColumn(const QString& table, const QString& column) const;

    struct Element {
        QString id;
        QString kind;        // "page" | "media" | "ink" | "shape" | "connector" | "group"
        double  x = 0, y = 0, w = 0, h = 0;
        int     z = 0;
        QString parentId;    // "" = none
        QString json = QStringLiteral("{}");
        qint64  created = 0, modified = 0;
    };
    std::vector<Element> allElements() const;            // z order, then creation
    std::optional<Element> element(const QString& id) const;
    int elementCount() const;
    void insertElement(const Element& e);                // created/modified stamped when 0
    void updateElementRect(const QString& id, double x, double y, double w, double h);
    void updateElementJson(const QString& id, const QString& json);
    void updateElementZ(const QString& id, int z);
    void updateElementParent(const QString& id, const QString& parentId);
    void deleteElement(const QString& id);               // a page's blocks cascade

    // A page's block rows VERBATIM (every column), for the canvas undo of a page removal: the
    // element is re-inserted, then its rows, with the same ids/ranks/timestamps. Raw SQL on
    // purpose — never through a BlockModel.
    struct RawBlock {
        QString id, rank;
        int     depth = 0;
        QString type, attrs, content;
        qint64  created = 0, modified = 0;
        QString pageId;
    };
    std::vector<RawBlock> rawBlocksOfPage(const QString& pageId) const;
    void restoreRawBlocks(const std::vector<RawBlock>& rows);
    // Re-home every block without a page (1.0 rows, converter) onto one page element.
    void assignOrphanBlocks(const QString& pageId);

    QString canvasMeta(const QString& key) const;        // "" when absent
    void setCanvasMeta(const QString& key, const QString& value);

    // --- Comments (v2). Threads are anchored by SpanComment spans (payload =
    // thread id) in block attrs; these tables hold the bodies. Thread rows
    // are NOT in the undo stack — span edits are, and orphaned threads
    // survive until explicitly deleted.
    struct CommentThread { QString id; qint64 created = 0; bool resolved = false; };
    struct CommentMessage { QString id, threadId, body; qint64 created = 0, modified = 0; };
    std::vector<CommentThread> commentThreads() const;
    std::vector<CommentMessage> commentMessages(const QString& threadId) const;
    void createThread(const QString& id);
    // Timestamp-preserving variants (merge migration, 2026-08-20).
    void createThreadAt(const QString& id, qint64 created, bool resolved);
    void insertMessageAt(const QString& id, const QString& threadId,
                         const QString& body, qint64 created, qint64 modified);
    void deleteThread(const QString& id);                      // cascades messages
    void setThreadResolved(const QString& id, bool resolved);
    void insertMessage(const QString& id, const QString& threadId, const QString& body);
    void updateMessage(const QString& id, const QString& body);
    void deleteMessage(const QString& id);

private:
    // The shared connection. Closes (and removes the Qt connection) when the last handle drops.
    struct Conn {
        explicit Conn(QString n) : name(std::move(n)) {}
        ~Conn();
        Conn(const Conn&) = delete;
        Conn& operator=(const Conn&) = delete;
        QString name;
        int     txnDepth = 0;
    };
    bool exec(const QString& sql) const;
    QSqlDatabase db() const;
    std::shared_ptr<Conn> conn_;
    QString pageScope_;
};

// Crockford-base32 ULID: time-sortable, globally unique block id (DESIGN §4).
QString makeUlid();

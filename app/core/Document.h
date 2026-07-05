#pragma once
#include <QString>
#include <vector>

// SQLite-canonical document store (DESIGN.md §3–4). The document file *is* this
// database. Owns the `blocks` table; BlockModel reads its eager skinny-scan
// (layout columns in rank order) and lazily fetches content per id.
//
// Phase 1a: open/schema/seed + read path. Write-back (persisted edits),
// fractional-rank inserts, FTS5 and layout_cache land in later sub-phases.
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

    ~Document();

    // Open (creating if absent) at `path`; apply pragmas + ensure schema. Closes
    // any currently-open connection first (so one Document can switch files).
    bool open(const QString& path);
    void close();
    bool isOpen() const { return open_; }
    // Flush the WAL into the main file so the .mndb is self-contained (Save).
    bool checkpoint();
    // Write a clean, compacted copy of the DB to `path` (Save As). Does not
    // switch — the caller re-opens the new path.
    bool vacuumInto(const QString& path) const;

    // --- Format versioning (doc_meta, id=1) ---
    // The .mndb format this build writes. Bump on any incompatible change to
    // the blocks/attrs/content encoding; readers gate migrations on
    // schemaVersion(). v1 = clean-text+spans content (markdown markers are
    // consumed on load), string span kinds in attrs, media descriptors with
    // portable src refs.
    static constexpr int kSchemaVersion = 1;
    // Upsert the doc_meta row: `created` is written once, then every stamp
    // updates schema_version + app_version + modified. Called on the save
    // paths so a saved file always records what wrote it.
    void stampMeta();
    // schema_version of the open doc; 0 = pre-versioning legacy (never stamped).
    int schemaVersion() const;

    int count() const;

    // Eager skinny-scan: layout columns only, already in visible (rank) order.
    std::vector<BlockMeta> skinnyScan() const;
    // Lazy fat fetch of one block's content.
    QString contentFor(const QString& id) const;

    // --- Write path (Phase 1b) ---
    // Wrap a batch of writes in begin/commit (also used for fast bulk seeding).
    void begin();
    void commit();
    void appendBlock(const QString& id, const QString& rank, int depth,
                     const QString& type, const QString& attrs, const QString& content);
    void updateContent(const QString& id, const QString& content);
    void updateMeta(const QString& id, const QString& type, const QString& attrs, int depth = 0);
    void updateRank(const QString& id, const QString& rank);   // reorder
    void deleteBlock(const QString& id);

private:
    bool exec(const QString& sql) const;
    QString conn_;
    bool open_ = false;
};

// Crockford-base32 ULID: time-sortable, globally unique block id (DESIGN §4).
QString makeUlid();

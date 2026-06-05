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

    // Open (creating if absent) at `path`; apply pragmas + ensure schema.
    bool open(const QString& path);
    bool isOpen() const { return open_; }

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
    void deleteBlock(const QString& id);

private:
    bool exec(const QString& sql) const;
    QString conn_;
    bool open_ = false;
};

// Crockford-base32 ULID: time-sortable, globally unique block id (DESIGN §4).
QString makeUlid();

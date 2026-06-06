#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <vector>
#include <cstdint>
#include "FenwickTree.h"
#include "Document.h"

// The document model: a QAbstractListModel over the SQLite-canonical store
// (DESIGN.md §3–4). Eager skinny-scan seeds the layout (type/rank) + Fenwick
// height index; content is held per row (Phase 1a loads it all from the DB —
// lazy windowed fetch is a later optimization). Heights live in the Fenwick
// index. The editor talks only to the Q_INVOKABLE seam below.
class BlockModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCountQml NOTIFY modelReset)
    Q_PROPERTY(qreal totalHeight READ totalHeight NOTIFY layoutChangedSpike)
    // Bump on any height change; QML bindings include it to force re-eval of
    // yForRow()/rowForY() (which are Q_INVOKABLE, not properties).
    Q_PROPERTY(int layoutRevision READ layoutRevision NOTIFY layoutChangedSpike)
    // Bump on any CONTENT change (edit/delete). The Flickable arm's text
    // binding includes it to re-read contentForRow() without repositioning.
    Q_PROPERTY(int contentRevision READ contentRevision NOTIFY contentChangedSpike)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStackChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStackChanged)
    // Path of the open document (one fixed scratch doc for now). CONSTANT until
    // the open/new-file flow lands.
    Q_PROPERTY(QString documentPath READ documentPath CONSTANT)
public:
    QString documentPath() const { return docPath_; }
    enum Roles {
        TypeRole = Qt::UserRole + 1,
        ContentRole,
        HeightRole,
        MeasuredRole,
    };
    enum BlockType : uint8_t {
        Paragraph = 0, Heading = 1, Code = 2, Media = 3,
        Quote = 4, ListItem = 5, Divider = 6
    };
    enum Distribution { Uniform = 0, Mixed = 1, Adversarial = 2 };

    explicit BlockModel(QObject* parent = nullptr);

    // QAbstractListModel
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int rowCountQml() const { return static_cast<int>(rows_.size()); }
    qreal totalHeight() const { return fenwick_.total(); }
    int layoutRevision() const { return layoutRevision_; }
    int contentRevision() const { return contentRevision_; }

    // --- Build / reconfigure (driven by Main.qml controls) ---
    Q_INVOKABLE void rebuild(int n, int distribution);

    // --- Geometry, for the Flickable arm (and HUD) ---
    Q_INVOKABLE qreal yForRow(int row) const { return fenwick_.prefix(clampRow(row)); }
    Q_INVOKABLE qreal heightForRow(int row) const { return fenwick_.height(clampRow(row)); }
    Q_INVOKABLE int rowForY(qreal y) const { return static_cast<int>(fenwick_.rowAtOffset(y < 0 ? 0 : y)); }

    // --- Row data, for the Flickable arm (ListView uses roles) ---
    Q_INVOKABLE int typeForRow(int row) const;
    Q_INVOKABLE int levelForRow(int row) const;       // heading level 1–6, else 0
    Q_INVOKABLE QString contentForRow(int row) const;
    // Inline markdown (**bold**, *italic*, `code`) renders via the QML-side
    // InlineMarkdownHighlighter applying char formats to each block's PlainText
    // document — no HTML, identity caret positions, so nothing here changes.

    // type↔markdown autoformat: if content at `row` now starts with a recognised
    // prefix (e.g. "## "), consume it, set type/level, persist, and return the
    // number of chars stripped (so the editor can shift the caret). Else 0.
    Q_INVOKABLE int applyMarkdownTrigger(int row);
    // Enter-triggered: a block whose whole content is "---"/"***"/"___" becomes
    // a divider. Returns true if it converted.
    Q_INVOKABLE bool makeDividerIfMarker(int row);

    // --- Feedback from delegates / edits ---
    // Delegate reports its laid-out height. Emits heightSettled(row, delta) so
    // the Flickable arm can compensate contentY when an off-screen block settles.
    Q_INVOKABLE void setMeasuredHeight(int row, qreal h);
    Q_INVOKABLE void setContent(int row, const QString& text);
    Q_INVOKABLE void insertBlock(int row);
    Q_INVOKABLE void removeBlock(int row);
    // Insert a copy of `row` (content/type/level/lang/spans) directly below it
    // (undoable). The caret-worthy new row is `row + 1`.
    Q_INVOKABLE void duplicateBlock(int row);
    // Reorder: move the block at `from` so it ends up at final index `to`. Just a
    // fractional-rank rewrite (no renumbering); undoable. No-op if out of range.
    Q_INVOKABLE void moveBlock(int from, int to);

    // P8: delete a logical selection spanning (aRow,aCol)..(fRow,fCol). Merges
    // the surviving head of the lo block with the tail of the hi block and
    // removes every block in between — a model op on a logical range, oblivious
    // to which of those blocks currently have delegates (DESIGN.md §7).
    Q_INVOKABLE void deleteRange(int aRow, int aCol, int fRow, int fCol);

    // Editing for the passive-surface arm: insert text at a caret, and split a
    // block at the caret (Enter). The model is the sole owner of content.
    // `marks` = armed typing attributes applied to the inserted run (Word-style):
    // bit 1 = bold, 2 = italic, 4 = code. Applied inside the insert transaction
    // so a run of armed typing still coalesces into one undo step.
    Q_INVOKABLE void insertText(int row, int col, const QString& text, int marks = 0);
    Q_INVOKABLE void splitBlock(int row, int col);

    // --- Semantic format spans (DESIGN §5 endgame): bold/italic/code as a span
    // [s,e) over the block's text, NOT `**` markers in the content. This is how
    // the menu (and, later, markdown-as-input) applies formatting; the render
    // shows clean styled text with no markers. Spans live in attrs and travel
    // with the row; insertText/splitBlock/deleteRange keep their offsets aligned.
    Q_INVOKABLE QVariantList spansForRow(int row) const;   // [{s,e,kind}] for the highlighter
    // Toggle `kind` ("bold"|"italic"|"code") over [start,end): if already fully
    // covered, clear it there; else add (merging overlapping same-kind spans).
    Q_INVOKABLE void toggleFormat(int row, int start, int end, const QString& kind);
    // Explicit add/remove + a coverage query, so the editor can apply a uniform
    // decision across a multi-block selection.
    Q_INVOKABLE bool hasFormat(int row, int start, int end, const QString& kind) const;
    Q_INVOKABLE void setFormat(int row, int start, int end, const QString& kind, bool on);
    // Group several mutations into ONE undo step (transactions nest by depth).
    Q_INVOKABLE void beginGroup(int loRow, int hiRow);
    Q_INVOKABLE void endGroup();
    // Inline-code ranges [{s,e}] (markdown backtick inner runs + code spans), in
    // source cols. The editor draws the code "chip" as an overlay rect from these
    // — NOT a char-format background — so the selection can layer above it.
    Q_INVOKABLE QVariantList codeRangesForRow(int row) const;

    // Markdown-as-input: when the caret LEAVES a block, consume its inline
    // markdown (`**`/`*`/`` ` ``) into semantic spans and strip the markers, so it
    // renders as a clean "normal" block (no markers). One-way; no-op if the block
    // has none. The editor calls this on block-to-block navigation.
    Q_INVOKABLE void commitMarkdown(int row);

    // Clear ALL formatting (bold/italic/code spans) over [start,end).
    Q_INVOKABLE void clearFormat(int row, int start, int end);
    // Set a block's heading level (1–6) or 0 = paragraph. Text blocks only.
    Q_INVOKABLE void setHeading(int row, int level);
    // Set a text block's type to paragraph(0)/quote(4)/list_item(5). Undoable.
    Q_INVOKABLE void setBlockType(int row, int type);
    // Insert a divider block after `afterRow` (undoable).
    Q_INVOKABLE void insertDivider(int afterRow);

    // --- Code blocks ---
    Q_INVOKABLE QString languageForRow(int row) const;          // syntax language, "" if none
    // Convert a block to a code block with `lang` (undoable). Empty lang = plain.
    Q_INVOKABLE void makeCodeBlock(int row, const QString& lang);
    // Change an existing code block's syntax language in place (undoable).
    Q_INVOKABLE void setCodeLanguage(int row, const QString& lang);
    // Enter trigger: if the block's whole content is a "```"/"```lang" fence,
    // turn it into an (empty) code block of that language. Returns true if it did.
    Q_INVOKABLE bool makeCodeBlockIfFence(int row);

    // --- Undo / redo (region-snapshot transactions; see the cpp). Linear today,
    // tree-ready (each entry stores its parent; redo = newest child).
    bool canUndo() const;
    bool canRedo() const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // The editor mirrors its (QML-owned) caret here so transactions can snapshot
    // it; undo/redo emit caretRestoreRequested to put it back.
    Q_INVOKABLE void noteCaret(int row, int col, int anchorRow, int anchorCol);

signals:
    void layoutChangedSpike();
    void contentChangedSpike();
    void modelReset();
    void heightSettled(int row, qreal delta);
    void undoStackChanged();
    void caretRestoreRequested(int row, int col, int anchorRow, int anchorCol);

public:
    enum SpanKind : uint8_t { SpanBold = 1, SpanItalic = 2, SpanCode = 3,
                              SpanStrike = 4, SpanUnderline = 5 };

private:
    struct Span { int s; int e; uint8_t kind; };   // [s,e) over the row's text
    struct Row {
        uint8_t type;
        uint16_t param;   // paragraph/code: line count; media: aspect*100; heading: 0
        uint8_t level = 0;  // heading level 1–6 (0 = not a heading)
        bool measured = false;
        QString lang;       // code blocks: syntax-highlight language (else empty)
        std::vector<Span> spans;   // travels with the row on insert/erase
    };

    // Full, restorable state of one block — the unit an undo transaction snaps.
    struct BlockSnap {
        QString id, rank, content, lang;
        uint8_t type = 0, level = 0;
        std::vector<Span> spans;
    };
    // One undoable step: the touched row-region [lo, lo+before.size()) replaced
    // before↔after, plus the caret on each side. `parent` makes it tree-ready
    // (redo follows the newest child); `coalesce` merges runs of typing.
    struct UndoEntry {
        int lo = 0;
        std::vector<BlockSnap> before, after;
        int cRowB = 0, cColB = 0, aRowB = 0, aColB = 0;   // caret before
        int cRowA = 0, cColA = 0, aRowA = 0, aColA = 0;   // caret after
        int parent = -1;
        QString coalesce;   // non-empty + equal + contiguous ⇒ merge into prev
    };

    static uint8_t spanKindFromString(const QString& s);
    static const char* spanKindToString(uint8_t k);
    // Interval ops on one row's spans (same-kind): union-cover test, add+merge,
    // and subtract a range. Offsets shift via shiftSpans on edits.
    static bool spansCover(const std::vector<Span>& v, int start, int end, uint8_t kind);
    static void addSpan(std::vector<Span>& v, int start, int end, uint8_t kind);
    static void removeSpan(std::vector<Span>& v, int start, int end, uint8_t kind);
    static void shiftSpansInsert(std::vector<Span>& v, int at, int len);
    static void shiftSpansDelete(std::vector<Span>& v, int from, int to);
    // Parse inline markdown in `src` into clean text + spans (markers removed),
    // merging `existing` spans remapped to the clean coords. Returns false (and
    // leaves outputs untouched) if there were no markers to consume.
    static bool convertMarkdown(const QString& src, const std::vector<Span>& existing,
                                QString& cleanText, std::vector<Span>& outSpans);

    // --- Undo internals ---
    QString attrsJson(uint8_t type, uint8_t level, const QString& lang, const std::vector<Span>& spans) const;
    BlockSnap snapAt(int row) const;
    std::vector<BlockSnap> snapshotRange(int lo, int hi) const;
    // Replace the current rows [lo, lo+oldCount) with `snaps` (in-memory + DB +
    // fenwick), then reset the view. The generic undo/redo apply.
    void applySnapshot(int lo, int oldCount, const std::vector<BlockSnap>& snaps);
    void beginTxn(int lo, int hi);              // snapshot `before` for [lo,hi]
    void endTxn(const QString& coalesce = {});  // snapshot `after`, push (or coalesce)
    void clearUndo();

    // Rule table: does `content` start with a markdown trigger? Fills type/
    // level and the prefix length to strip. The single source of truth that
    // (later) also drives import + export.
    static bool matchMarkdownPrefix(const QString& content, BlockType& type, int& level, int& strip);
    void persistMeta(int row);   // write type/attrs of content_[row]'s block

    int clampRow(int row) const;
    double estimatedHeight(const Row& r) const;
    QString genBase(int row, const Row& r) const;   // synthesize a row's text (no edit overlay)
    QString textAt(int row) const;                  // edit override else loaded content
    void bumpLayout();

    // --- SQLite store (Phase 1a/1b) ---
    void loadFromStore();             // skinny-scan → rows_/ids_/ranks_/content_/fenwick
    void seedSyntheticStore(int n);   // write N synthetic blocks if the DB is empty
    void persistContent(int row);     // write content_[row] back to the DB
    static BlockType typeFromString(const QString& s);
    static const char* typeToString(uint8_t t);
    // Lexicographic fractional rank strictly between a and b (DESIGN §4).
    // Empty a = "before all", empty b = "after all". O(shared-prefix length).
    static QString rankBetween(const QString& a, const QString& b);

    Document doc_;
    QString docPath_;
    std::vector<Row> rows_;
    std::vector<QString> ids_;       // block id (ULID) per row, parallel to rows_
    std::vector<QString> ranks_;     // fractional rank per row, parallel to rows_
    std::vector<QString> content_;   // content per row (the in-memory truth; write-through to DB)
    FenwickTree fenwick_;
    int layoutRevision_ = 0;
    int contentRevision_ = 0;

    // Undo/redo state.
    std::vector<UndoEntry> undo_;
    int undoCur_ = -1;            // index of the current state (-1 = initial)
    bool applying_ = false;       // true while undo/redo applies (don't record)
    // open transaction (depth-counted so mutations can nest into one group)
    int txnDepth_ = 0;
    int txnLo_ = 0, txnHi_ = 0;
    size_t txnSize_ = 0;
    std::vector<BlockSnap> txnBefore_;
    // mirrored caret (the editor pushes it via noteCaret)
    int cRow_ = 0, cCol_ = 0, aRow_ = 0, aCol_ = 0;
    bool awaitingAfter_ = false;  // stamp the just-pushed entry's caret-after on next noteCaret
};

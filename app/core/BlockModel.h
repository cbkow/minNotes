#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>
#include "FenwickTree.h"
#include "Document.h"
#include "TableGrid.h"
#include "MediaStore.h"

class QNetworkAccessManager;

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
        Quote = 4, ListItem = 5, Divider = 6, Table = 7,
        TaskListItem = 8   // a list item carrying a tri-state status (todo/doing/done)
    };
    enum TaskState : uint8_t { TaskTodo = 0, TaskDoing = 1, TaskDone = 2, TaskStateCount = 3 };
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
    // True once a row has reported a real laid-out height (cached in the Fenwick).
    // Tables use this to measure only once and reuse the cache on recycle.
    Q_INVOKABLE bool rowMeasured(int row) const { return rows_[clampRow(row)].measured; }
    Q_INVOKABLE int rowForY(qreal y) const { return static_cast<int>(fenwick_.rowAtOffset(y < 0 ? 0 : y)); }

    // --- Row data, for the Flickable arm (ListView uses roles) ---
    Q_INVOKABLE int typeForRow(int row) const;
    Q_INVOKABLE int levelForRow(int row) const;       // heading level 1–6, else 0
    Q_INVOKABLE int taskStateForRow(int row) const;   // task items: 0 todo / 1 doing / 2 done
    Q_INVOKABLE void toggleTask(int row);             // cycle todo→doing→done→todo
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
    // Media is known-geometry: its height is a pure function of the probed dims
    // and the page width the doc is laid out at, so the view never measures it
    // back — it sets the width here (on load + resize) and the model re-derives
    // every media row's height. mediaDisplayHeight returns a row's frame px.
    Q_INVOKABLE void  setContentWidth(qreal w);
    Q_INVOKABLE qreal mediaDisplayHeight(int row) const;
    // Effective displayed media width (per-block override or default). Set a new
    // per-block width (w<=0 resets to the default); clamped to [80, pageWidth].
    // Persists in the descriptor, so undo/reload Just Work; one undo step.
    Q_INVOKABLE int  mediaDispWidth(int row) const;
    Q_INVOKABLE void setMediaWidth(int row, int w);
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

    // Smart multi-block paste: split `text` into blocks (blank lines separate;
    // each line gets block-prefix + inline-markdown parsing), splice them at the
    // caret, and record the whole thing as ONE undo step. Returns [caretRow,
    // caretCol] for where the caret should land after the paste ([] on no-op).
    // Fenced code blocks are not yet reconstructed (``` lines paste literally).
    Q_INVOKABLE QVariantList pasteText(int row, int col, const QString& text);

    // Rich paste: parse clipboard HTML (Word/Google Docs/Excel/web) via QTextDocument
    // and splice the resulting blocks — headings/lists/paragraphs + bold/italic/
    // underline/strike/links, and tables → Table blocks — at the caret as ONE undo
    // step. Returns [caretRow, caretCol] (or [] on no usable content). Images are
    // dropped for now (a later milestone fetches them into the sidecar).
    Q_INVOKABLE QVariantList pasteHtml(int row, int col, const QString& html);

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
    // Links: a SpanLink carries a URL target. setLink over [start,end) replaces any
    // link spans there; an empty url removes them. linkAt returns the URL of a link
    // covering `col` (for Cmd/Ctrl-click open + hover), or "" if none.
    Q_INVOKABLE void setLink(int row, int start, int end, const QString& url);
    Q_INVOKABLE QString linkAt(int row, int col) const;
    // Text color / highlight over [start,end): an empty color removes that kind.
    // (Both ride the same payload-span path as links.)
    Q_INVOKABLE void setTextColor(int row, int start, int end, const QString& color);
    Q_INVOKABLE void setHighlight(int row, int start, int end, const QString& color);
    // The [s,e] range of a link span covering `col` (for "edit this link" with no
    // selection), or an empty list if none.
    Q_INVOKABLE QVariantList linkRangeAt(int row, int col) const;
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

    // --- Tables ---
    // The grid lives as compact JSON in the block's `content`; these read a cached
    // parse and the mutators reserialize + persist through the txn chokepoint, so
    // undo/redo work unchanged. Cell typing coalesces per cell ("tcell:r:c").
    // Insert a fresh `nRows`x`nCols` table block after `afterRow` (undoable).
    Q_INVOKABLE void insertTable(int afterRow, int nRows, int nCols);
    // Build a table block from pasted TSV (tabs = columns, newlines = rows) and
    // insert it after `afterRow`. Returns the new table's row index, or -1.
    Q_INVOKABLE int  insertTableFromTSV(int afterRow, const QString& tsv);
    Q_INVOKABLE int  tableRows(int row) const;
    Q_INVOKABLE int  tableColumns(int row) const;
    Q_INVOKABLE int  tableHeaderRows(int row) const;
    Q_INVOKABLE QString tableCell(int row, int r, int c) const;
    Q_INVOKABLE int  tableColWidth(int row, int c) const;     // 0 = auto-size
    Q_INVOKABLE int  tableColAlign(int row, int c) const;     // 0 left,1 center,2 right
    Q_INVOKABLE void tableSetCell(int row, int r, int c, const QString& text);
    // Cell / row / column colour. fg=true sets text colour, else background.
    // empty color removes it. tableCellBg/Fg return the EFFECTIVE colour for
    // rendering (cell → row → column → "" cascade).
    Q_INVOKABLE void tableSetCellColor(int row, int r0, int c0, int r1, int c1, bool fg, const QString& color);
    Q_INVOKABLE void tableSetRowColor(int row, int r, bool fg, const QString& color);
    Q_INVOKABLE void tableSetColColor(int row, int c, bool fg, const QString& color);
    Q_INVOKABLE QString tableCellBg(int row, int r, int c) const;
    Q_INVOKABLE QString tableCellFg(int row, int r, int c) const;
    // Rich text inside a cell — inline spans, mirroring the block-level span API.
    // tableCellSpans returns [{s,e,k,u?}] for the highlighter; the format ops add/
    // remove a span over a char range; insert/delete keep spans aligned with text.
    Q_INVOKABLE QVariantList tableCellSpans(int row, int r, int c) const;
    Q_INVOKABLE bool tableCellHasFormat(int row, int r, int c, int start, int end, const QString& kind) const;
    Q_INVOKABLE void tableSetCellFormat(int row, int r, int c, int start, int end, const QString& kind, bool on);
    Q_INVOKABLE void tableClearCellFormat(int row, int r, int c, int start, int end);
    Q_INVOKABLE void tableCellInsert(int row, int r, int c, int at, const QString& text);
    Q_INVOKABLE void tableCellDelete(int row, int r, int c, int from, int to);
    // Image inside a cell — the descriptor JSON ({src,w,h}) lives in cell.media,
    // imported through MediaStore exactly like a media block. Setting one widens a
    // narrow column to a sensible default so the image is visible.
    Q_INVOKABLE bool tableSetCellImageFromClipboard(int row, int r, int c);
    Q_INVOKABLE bool tableSetCellImageFromUrl(int row, int r, int c, const QString& fileUrl);
    Q_INVOKABLE void tableClearCellMedia(int row, int r, int c);
    Q_INVOKABLE QString tableCellMedia(int row, int r, int c) const;     // raw descriptor ("" = none)
    Q_INVOKABLE QString tableCellMediaUrl(int row, int r, int c) const;  // resolved loadable URL
    Q_INVOKABLE int tableCellMediaW(int row, int r, int c) const;        // intrinsic width
    Q_INVOKABLE int tableCellMediaH(int row, int r, int c) const;        // intrinsic height
    Q_INVOKABLE int tableCellMediaDw(int row, int r, int c) const;       // display-width override (0 = none)
    Q_INVOKABLE void tableSetCellImageWidth(int row, int r, int c, int w);  // w<=0 clears the override
    Q_INVOKABLE void tableInsertRow(int row, int at);
    Q_INVOKABLE void tableInsertColumn(int row, int at);
    Q_INVOKABLE void tableDeleteRow(int row, int at);
    Q_INVOKABLE void tableDeleteColumn(int row, int at);
    Q_INVOKABLE void tableSetColWidth(int row, int c, int w);
    Q_INVOKABLE void tableSetColAlign(int row, int c, int a);
    Q_INVOKABLE void tableSetHeaderRows(int row, int n);
    // Reorder a row / column (`to` is the post-removal index); one undo step.
    Q_INVOKABLE void tableMoveRow(int row, int from, int to);
    Q_INVOKABLE void tableMoveColumn(int row, int from, int to);
    Q_INVOKABLE void tableDuplicateRow(int row, int at);     // copy → insert below
    Q_INVOKABLE void tableDuplicateColumn(int row, int at);  // copy → insert right
    // One-shot sort of the body rows by a column (header rows stay pinned).
    Q_INVOKABLE void tableSortByColumn(int row, int c, bool asc);
    // Fill the range from its top row / left column (whole cells; one undo step).
    Q_INVOKABLE void tableFillDown(int row, int r0, int c0, int r1, int c1);
    Q_INVOKABLE void tableFillRight(int row, int r0, int c0, int r1, int c1);
    // Choice columns (typed columns): a shared, ordered option set per column;
    // body cells reference an option by its stable id. All edits go through
    // mutateTable, so undo / persistence / refresh come for free.
    Q_INVOKABLE int  tableColumnKind(int row, int c) const;              // 0 text, 1 choice
    Q_INVOKABLE void tableSetColumnKind(int row, int c, int kind);
    Q_INVOKABLE QVariantList tableColumnOptions(int row, int c) const;   // [{id,label,color}]
    Q_INVOKABLE QString tableAddOption(int row, int c, const QString& label, const QString& color);  // → new id
    // Replace a choice column's whole option set in one step (the modal editor's
    // Done): each entry is {id,label,color}; a missing/empty id is minted. Preserves
    // ids so cell selections survive; drops cells whose option was deleted.
    Q_INVOKABLE void tableSetColumnOptions(int row, int c, const QVariantList& opts);
    Q_INVOKABLE void tableRenameOption(int row, int c, const QString& id, const QString& label);
    Q_INVOKABLE void tableRecolorOption(int row, int c, const QString& id, const QString& color);
    Q_INVOKABLE void tableRemoveOption(int row, int c, const QString& id);
    Q_INVOKABLE void tableMoveOption(int row, int c, const QString& id, int toIndex);
    Q_INVOKABLE QString tableCellChoice(int row, int r, int c) const;    // selected option id ("" = none)
    Q_INVOKABLE void tableSetCellChoice(int row, int r, int c, const QString& id);
    Q_INVOKABLE QString tableCellChoiceLabel(int row, int r, int c) const;   // selected option's label
    Q_INVOKABLE QString tableCellChoiceColor(int row, int r, int c) const;   // selected option's colour hex
    Q_INVOKABLE int  tableCellCheck(int row, int r, int c) const;            // check column: 0/1/2
    Q_INVOKABLE void tableCycleCellCheck(int row, int r, int c);             // cycle todo→doing→done
    Q_INVOKABLE void tableSetCellCheck(int row, int r, int c, int state);    // set 0/1/2 (kanban drop)
    // Paste TSV (tab/newline) into the table at anchor (r,c), growing as needed.
    Q_INVOKABLE void tablePasteTSV(int row, int r, int c, const QString& tsv);
    // Serialize an inclusive cell range for the clipboard (TSV + HTML <table>).
    Q_INVOKABLE QString tableRangeTSV(int row, int r0, int c0, int r1, int c1) const;
    Q_INVOKABLE QString tableRangeHtml(int row, int r0, int c0, int r1, int c1) const;
    // Clear every cell in an inclusive range (one undo step).
    Q_INVOKABLE void tableClearRange(int row, int r0, int c0, int r1, int c1);
    // Ordered block ids of every table in the document (for the table-tab strip).
    Q_INVOKABLE QStringList tableBlockIds() const;
    // Ordered block ids of every inline PDF (for the PDF full-page tab strip).
    Q_INVOKABLE QStringList pdfBlockIds() const;
    // Current row of a block id, or -1 if it no longer exists.
    Q_INVOKABLE int rowForId(const QString& id) const;
    // Block id at `row` (empty if out of range).
    Q_INVOKABLE QString idForRow(int row) const;

    // --- Media (images + video). The media descriptor lives as JSON in the
    // block's content ({src,w,h,kind,...}) so undo/persistence reuse the
    // chokepoint; bytes are never in the DB (see MediaStore). ---
    Q_INVOKABLE bool insertImageFromUrl(int afterRow, const QString& fileUrl);
    Q_INVOKABLE bool insertImageFromClipboard(int afterRow);
    Q_INVOKABLE bool insertVideoFromUrl(int afterRow, const QString& fileUrl);
    // PDF → inline page view (kind:"pdf", referenced in place).
    Q_INVOKABLE bool insertPdfFromUrl(int afterRow, const QString& fileUrl);
    // Unsupported file → a generic attachment chip (referenced in place, no copy).
    Q_INVOKABLE bool insertFileFromUrl(int afterRow, const QString& fileUrl);
    // Route a dropped/pasted file: video → pdf → image → file-attachment fallback.
    Q_INVOKABLE bool insertMediaFromUrl(int afterRow, const QString& fileUrl);
    Q_INVOKABLE QString mediaUrl(int row) const;   // resolved file:// URL ("" if none)
    Q_INVOKABLE QString mediaLocalPath(int row) const; // resolved absolute path (for the decoder)
    Q_INVOKABLE int mediaW(int row) const;          // intrinsic width (0 if unknown)
    Q_INVOKABLE int mediaH(int row) const;          // intrinsic height
    Q_INVOKABLE QString mediaKind(int row) const;   // "image" | "video" | "file"
    Q_INVOKABLE QString mediaFileName(int row) const; // attachment display name
    Q_INVOKABLE int     mediaPdfPages(int row) const;  // PDF page count (0 if not a PDF)
    Q_INVOKABLE void    openMediaInUfb(int row) const; // open in the ufb browser (deep link)
    Q_INVOKABLE double  mediaFps(int row) const;     // video fps (0 if image/unknown)
    Q_INVOKABLE int     mediaFrames(int row) const;  // video frame count (0 if image)
    Q_INVOKABLE qreal   mediaDurationMs(int row) const; // video duration ms (0 if image)
    Q_INVOKABLE void    revealMedia(int row) const;  // show the media file in Finder/Explorer

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
                              SpanStrike = 4, SpanUnderline = 5, SpanLink = 6,
                              SpanFgColor = 7, SpanHighlight = 8 };  // href holds the color hex

private:
    // [s,e) over the row's text. `href` is set only for SpanLink (the link target);
    // empty for every other kind. Aggregate-init as {s,e,kind} leaves href empty.
    struct Span { int s; int e; uint8_t kind; QString href; };
    struct Row {
        uint8_t type;
        uint16_t param;   // paragraph/code: line count; media: aspect*100; heading: 0
        uint8_t level = 0;  // heading level 1–6 (0 = not a heading)
        uint8_t taskState = 0;  // task items: 0 todo / 1 doing / 2 done (else 0)
        bool measured = false;
        bool isVideo = false;   // media only: true → reserve the transport-toolbar height
        bool isFile = false;    // media only: kind=="file" → an unsupported-file chip
        bool isPdf = false;     // media only: kind=="pdf" → inline page view + nav
        uint16_t dispW = 0;     // media only: per-block display width override (0 = default/fit)
        uint16_t mediaW = 0;    // media only: intrinsic width px  (exact no-upscale height estimate)
        uint16_t mediaH = 0;    // media only: intrinsic height px
        QString lang;       // code blocks: syntax-highlight language (else empty)
        std::vector<Span> spans;   // travels with the row on insert/erase
    };

    // Full, restorable state of one block — the unit an undo transaction snaps.
    struct BlockSnap {
        QString id, rank, content, lang;
        uint8_t type = 0, level = 0, taskState = 0;
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
    void setPayloadSpan(int row, int start, int end, uint8_t kind, const QString& payload);
    static void addSpan(std::vector<Span>& v, int start, int end, uint8_t kind);
    static void removeSpan(std::vector<Span>& v, int start, int end, uint8_t kind);
    static void shiftSpansInsert(std::vector<Span>& v, int at, int len);
    static void shiftSpansDelete(std::vector<Span>& v, int from, int to);
    // Table cells store spans as a JSON array ({s,e,k,u?}); convert to/from the
    // Span vector so the static span helpers above can be reused for cell editing.
    static std::vector<Span> cellSpansFromJson(const QJsonArray& a);
    static QJsonArray cellSpansToJson(const std::vector<Span>& v);
    // Parse inline markdown in `src` into clean text + spans (markers removed),
    // merging `existing` spans remapped to the clean coords. Returns false (and
    // leaves outputs untouched) if there were no markers to consume.
    static bool convertMarkdown(const QString& src, const std::vector<Span>& existing,
                                QString& cleanText, std::vector<Span>& outSpans);

    // --- Undo internals ---
    QString attrsJson(uint8_t type, uint8_t level, const QString& lang, const std::vector<Span>& spans,
                      uint8_t taskState = 0) const;
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
    // Derive a Media row's intrinsic dims / video flag / aspect param from its
    // descriptor JSON. The SINGLE place media metadata comes from content — used
    // by loadFromStore, insertMedia, AND undo/redo restore, so they can't diverge
    // (divergence is what hid undo-restored media at ~0 height). No-op for non-media.
    void fillMediaMeta(Row& r, const QString& content) const;
    double estimatedHeight(const Row& r) const;
    double mediaFrameHeight(const Row& r) const;   // displayed media frame px at contentWidth_
    double mediaDisplayWidth(const Row& r) const;  // per-block override or default width
    double contentWidth_ = 760.0;                  // page width the doc is laid out at (view-set)
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

    // Table grid cache: parse the focused table's content JSON once per
    // (row, contentRevision) so the many per-cell QML queries don't re-parse.
    const TableGrid& gridFor(int row) const;
    mutable TableGrid tableCache_;
    mutable int tableCacheRow_ = -1;
    mutable int tableCacheRev_ = -1;
    std::unique_ptr<MediaStore> mediaStore_;
    // Remote <img> from pasted HTML: download in the background (block displays
    // the remote URL meanwhile) and swap the descriptor to the sidecar copy.
    QNetworkAccessManager* net_ = nullptr;
    void fetchRemoteMediaIn(int loRow, int hiRow);                    // scan a range, fetch http src
    void updateMediaDescriptor(const QString& blockId, const QString& json);  // localized → swap in
    // Insert a media block (content = descriptor JSON) after `afterRow`; undoable.
    void insertMedia(int afterRow, const QString& json, uint16_t aspectParam);
    // Apply a mutation to the table at `row` via a lambda, then reserialize to
    // content, persist, and snapshot (one txn; `coalesce` groups cell typing).
    void mutateTable(int row, const std::function<void(TableGrid&)>& fn,
                     const QString& coalesce = QString());

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

// Headless regression harness for the document-state fixes made during the
// Windows port (run: mn_regression_test). Drives BlockModel directly — the layer
// the fixes live in — so the checks are deterministic (no flaky GUI clicking).
// setMeasuredHeight() is the same seam the QML delegate uses to report a laid-out
// height, so we can reproduce the undo/redo height-preservation bug without a view.
//
// Built only when configured with -DMINNOTES_BUILD_TESTS=ON. Exit code = number
// of failed checks (0 = all pass).
#include "BlockModel.h"
#include "Exporter.h"
#include "../app/notes/doc_ink.h"
#include "../app/notes/sketch_text.h"
#include <private/qzipreader_p.h>

#include <QFontDatabase>

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) qInfo("  PASS: " __VA_ARGS__); \
    else { qCritical("  FAIL: " __VA_ARGS__); ++g_fail; } \
} while (0)

static int findRowOfType(const BlockModel& m, int type) {
    for (int i = 0; i < m.rowCountQml(); ++i)
        if (m.typeForRow(i) == type) return i;
    return -1;
}

// Build a small doc: paragraph "A", a 3x3 table, paragraph "B", paragraph "C".
// Returns once documentOpen() with the expected shape.
static void buildDoc(BlockModel& m) {
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);   // start from a clean slate
    m.insertBlock(0);            m.setContent(0, QStringLiteral("A"));
    m.insertTable(0, 3, 3);      // table inserted after row 0 → at row 1
    int after = findRowOfType(m, BlockModel::Table);
    m.insertBlock(after + 1);    m.setContent(after + 1, QStringLiteral("B"));
    m.insertBlock(after + 2);    m.setContent(after + 2, QStringLiteral("C"));
}

// --- Test 1: count NOTIFY fires on incremental insert/remove ----------------
static void testCountNotify() {
    qInfo("[1] count NOTIFY on incremental insert/remove (Enter-split bug)");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    int notifies = 0;
    QObject::connect(&m, &BlockModel::countChanged, &m, [&notifies] { ++notifies; });
    const int before = m.rowCountQml();
    m.insertBlock(1);                                  // the Enter-split insert
    CHECK(m.rowCountQml() == before + 1, "rowCount grew by 1");
    CHECK(notifies >= 1, "countChanged emitted on insert");
    notifies = 0;
    m.removeBlock(1);
    CHECK(notifies >= 1, "countChanged emitted on remove");
}

// --- Test 2: inline markdown converts to clean text on commit ---------------
static void testCommitMarkdown() {
    qInfo("[2] commitMarkdown converts inline markers to spans (strips markers)");
    BlockModel m; m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    auto roundtrip = [&](const QString& src, const QString& clean, const char* label) {
        m.insertBlock(0); m.setContent(0, src); m.commitMarkdown(0);
        CHECK(m.contentForRow(0) == clean, "%s: '%s' -> '%s'",
              label, qPrintable(src), qPrintable(m.contentForRow(0)));
        m.removeBlock(0);
    };
    roundtrip(QStringLiteral("*italic words*"),  QStringLiteral("italic words"), "italic");
    roundtrip(QStringLiteral("**bold**"),        QStringLiteral("bold"),         "bold");
    roundtrip(QStringLiteral("`code`"),          QStringLiteral("code"),         "code");
    roundtrip(QStringLiteral("~~strike~~"),      QStringLiteral("strike"),       "strike");
    roundtrip(QStringLiteral("plain text"),      QStringLiteral("plain text"),   "no-op");
}

// --- Test 3: undo/redo preserves measured heights of untouched rows ---------
// (The applySnapshot fix — table outside the edit must keep its real height, not
// reset to estimate, or it overlaps the blocks below.)
static void testUndoRedoHeights() {
    qInfo("[3] undo/redo preserves measured heights (table layout corruption)");
    BlockModel m;
    buildDoc(m);
    const int t0 = findRowOfType(m, BlockModel::Table);
    CHECK(t0 >= 1, "table built at row >= 1 (row %d)", t0);

    // Simulate the view measuring every row: paragraphs 30px, the table 200px.
    const qreal TABLE_H = 200.0;
    for (int i = 0; i < m.rowCountQml(); ++i)
        m.setMeasuredHeight(i, (i == t0) ? TABLE_H : 30.0);
    CHECK(qFuzzyCompare(m.heightForRow(t0), TABLE_H), "table measured to 200 pre-edit");

    // Delete paragraph "A" at row 0 (a DIFFERENT row than the table), then undo.
    m.removeBlock(0);
    m.undo();
    const int t1 = findRowOfType(m, BlockModel::Table);
    CHECK(m.rowCountQml() >= 4, "undo restored the removed block");
    CHECK(m.rowMeasured(t1), "table still flagged measured after undo");
    CHECK(qFuzzyCompare(m.heightForRow(t1), TABLE_H),
          "table height preserved after undo (got %.1f, want 200)", m.heightForRow(t1));
    // The re-inserted row 0 is in the replaced region → must re-measure (its height
    // reverts to an estimate until the view lays it out again; that's correct).
    CHECK(!m.rowMeasured(0), "re-inserted row marked unmeasured (will re-measure)");

    // Redo the deletion: the table is untouched → keeps its measured height.
    m.redo();
    const int t2 = findRowOfType(m, BlockModel::Table);
    CHECK(m.rowMeasured(t2) && qFuzzyCompare(m.heightForRow(t2), TABLE_H),
          "table height preserved after redo (got %.1f, want 200)", m.heightForRow(t2));
}

// --- Test 4: save -> close -> reopen round-trips structure + content --------
static void testSaveReopen() {
    qInfo("[4] save -> reopen round-trip (persistence)");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_test.mndb");
    QFile::remove(path);

    BlockModel m;
    buildDoc(m);
    const int countA = m.rowCountQml();
    const int tableA = findRowOfType(m, BlockModel::Table);
    QStringList typesA, textA;
    for (int i = 0; i < countA; ++i) { typesA << QString::number(m.typeForRow(i)); textA << m.contentForRow(i); }
    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();
    CHECK(!m.documentOpen(), "document closed");

    BlockModel m2;
    CHECK(m2.openDocument(path), "openDocument() succeeded");
    CHECK(m2.rowCountQml() == countA, "row count round-trips (%d == %d)", m2.rowCountQml(), countA);
    CHECK(findRowOfType(m2, BlockModel::Table) == tableA, "table at same row after reopen");
    bool typesOk = true, textOk = true;
    for (int i = 0; i < m2.rowCountQml() && i < countA; ++i) {
        if (QString::number(m2.typeForRow(i)) != typesA[i]) typesOk = false;
        if (m2.contentForRow(i) != textA[i]) textOk = false;
    }
    CHECK(typesOk, "block types round-trip");
    CHECK(textOk, "block contents round-trip");

    // Reopened doc + undo/redo (stack reset on open → no-op, must not corrupt).
    const int countReopen = m2.rowCountQml();
    m2.undo(); m2.redo();
    CHECK(m2.rowCountQml() == countReopen, "undo/redo after reopen leaves structure intact");

    QFile::remove(path);
}

// --- Test 5: coalesce must not absorb into a non-leaf undo node -------------
// The "delete block corrupts the snapshot" bug: after an undo, the current undo
// node still has a child (the just-undone entry). A subsequent coalescible edit
// (single-char del/type run) must BRANCH — a new entry parented to the node —
// not overwrite the node's `after` in place. Overwriting orphans the child's
// `before`, and a later redo replays the child onto a state it never followed,
// resurrecting stale content.
static void testUndoBranchCoalesce() {
    qInfo("[5] coalesce onto a non-leaf undo node must branch (undo-branch corruption)");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    // Mirror the QML cursor protocol: every caret move syncs into the model,
    // and undo/redo restore the caret through caretRestoreRequested.
    QObject::connect(&m, &BlockModel::caretRestoreRequested, &m,
                     [&m](int r, int c, int ar, int ac) { m.noteCaret(r, c, ar, ac); });

    m.noteCaret(0, 0, 0, 0);
    m.insertText(0, 0, QStringLiteral("a"), 0, {}, {}); m.noteCaret(0, 1, 0, 1);
    m.insertText(0, 1, QStringLiteral("b"), 0, {}, {}); m.noteCaret(0, 2, 0, 2);
    // E1: backspace "ab" -> "a" (coalesce key "del").
    m.deleteRange(0, 1, 0, 2); m.noteCaret(0, 1, 0, 1);
    // E2: type "z" -> "az" (child of E1).
    m.insertText(0, 1, QStringLiteral("z"), 0, {}, {}); m.noteCaret(0, 2, 0, 2);
    // Undo E2 -> "a". The current node is now E1 — a NON-leaf (E2 is its child).
    m.undo();
    CHECK(m.contentForRow(0) == QStringLiteral("a"), "undo returned to 'a'");
    // Backspace again ("del", same key as E1). Pre-fix this coalesced INTO E1.
    m.deleteRange(0, 0, 0, 1); m.noteCaret(0, 0, 0, 0);
    CHECK(m.contentForRow(0).isEmpty(), "second backspace deleted to ''");
    // Undo must return to 'a' — the state this run actually started from.
    // (Pre-fix it jumped to 'ab': E1's before, the coalesced-over entry.)
    m.undo();
    CHECK(m.contentForRow(0) == QStringLiteral("a"),
          "undo returns to 'a', not 'ab' (no coalesce across the branch)");
    m.undo();
    CHECK(m.contentForRow(0) == QStringLiteral("ab"), "second undo returns to 'ab'");
    // Redo follows the NEWEST branch: 'ab' -> 'a' -> '' (stale 'az' must not return).
    m.redo();
    CHECK(m.contentForRow(0) == QStringLiteral("a"), "redo replays the del run to 'a'");
    m.redo();
    CHECK(m.contentForRow(0).isEmpty(),
          "redo follows the newest branch to '' (stale 'az' not resurrected)");
}

// --- Test 6: on-disk markdown canonicalization + doc_meta stamping ----------
// v1 format rule: clean-text+spans is the ONE on-disk text form. A document
// saved with raw inline markers (legacy) must come back canonicalized after an
// open→save cycle — and the open itself must NOT mark the doc dirty (it's a
// normalization, not a user edit). Saves stamp doc_meta.schema_version.
static void testCanonicalizeAndStamp() {
    qInfo("[6] markdown canonicalized on disk after open->save; doc_meta stamped");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_canon.mndb");
    QFile::remove(path);

    {   // Write a doc whose DB content still holds raw markers (setContent
        // persists verbatim; conversion only happens on load/commit).
        BlockModel m;
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        m.setContent(0, QStringLiteral("**bold** word"));
        CHECK(m.saveAs(path), "saveAs() succeeded");
        m.closeDocument();
    }
    {   // The save path stamped doc_meta.
        Document d;
        CHECK(d.open(path), "raw Document opens the saved file");
        CHECK(d.schemaVersion() == Document::kSchemaVersion,
              "saveAs stamped schema_version (%d == %d)",
              d.schemaVersion(), Document::kSchemaVersion);
        d.close();
    }
    {   // Open converts markers -> spans (in memory AND into the working copy)
        // without dirtying; a save then canonicalizes the original on disk.
        BlockModel m;
        CHECK(m.openDocument(path), "openDocument() succeeded");
        CHECK(m.contentForRow(0) == QStringLiteral("bold word"),
              "markers consumed on load ('%s')", qPrintable(m.contentForRow(0)));
        CHECK(!m.dirty(), "canonicalization does not mark the document dirty");
        CHECK(m.save(), "save() succeeded");
        m.closeDocument();
    }
    {   // Direct on-disk proof: the blocks table now holds clean text + spans.
        Document d;
        CHECK(d.open(path), "canonicalized file opens");
        const auto metas = d.skinnyScan();
        CHECK(!metas.empty(), "canonicalized file has blocks");
        if (!metas.empty()) {
            CHECK(d.contentFor(metas[0].id) == QStringLiteral("bold word"),
                  "on-disk content is marker-free ('%s')",
                  qPrintable(d.contentFor(metas[0].id)));
            CHECK(metas[0].attrs.contains(QStringLiteral("\"k\": \"bold\""))
                      || metas[0].attrs.contains(QStringLiteral("\"k\":\"bold\"")),
                  "on-disk attrs carry the bold span (%s)", qPrintable(metas[0].attrs));
        }
        d.close();
    }
    QFile::remove(path);
}

// --- Test 7: ordered lists + nesting depth -----------------------------------
// v1 list semantics: "N. " triggers an ordered item (number COMPUTED at render
// time), Enter continues a list at the same type/depth, Tab/Shift+Tab shift
// depth (one undo step), deeper children don't break a numbering run, and
// depth survives save/reopen and undo.
static void testListsAndDepth() {
    qInfo("[7] ordered lists: trigger, continuation, numbering, depth round-trip, undo");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    m.noteCaret(0, 0, 0, 0);

    m.setContent(0, QStringLiteral("1. first"));
    m.applyMarkdownTrigger(0);
    CHECK(m.typeForRow(0) == BlockModel::OrderedListItem, "\"1. \" trigger -> ordered item");
    CHECK(m.contentForRow(0) == QStringLiteral("first"), "trigger stripped the prefix");

    m.splitBlock(0, m.contentForRow(0).length());          // Enter at end of item
    CHECK(m.typeForRow(1) == BlockModel::OrderedListItem, "Enter continues the ordered list");
    m.setContent(1, QStringLiteral("second"));
    CHECK(m.orderedNumberForRow(0) == 1 && m.orderedNumberForRow(1) == 2,
          "run numbers 1, 2 (got %d, %d)", m.orderedNumberForRow(0), m.orderedNumberForRow(1));

    m.indentBlocks(1, 1, 1);                                // Tab
    CHECK(m.depthForRow(1) == 1, "Tab indents to depth 1");
    CHECK(m.orderedNumberForRow(1) == 1, "nested item restarts numbering at 1");

    m.splitBlock(1, m.contentForRow(1).length());           // continues at depth 1
    CHECK(m.depthForRow(2) == 1, "continuation inherits depth");
    m.indentBlocks(2, 2, -1);                               // Shift+Tab back to top level
    m.setContent(2, QStringLiteral("third"));
    CHECK(m.depthForRow(2) == 0, "Shift+Tab outdents");
    CHECK(m.orderedNumberForRow(2) == 2,
          "deeper child doesn't break the top-level run (got %d)", m.orderedNumberForRow(2));

    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_lists.mndb");
    QFile::remove(path);
    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();

    BlockModel m2;
    CHECK(m2.openDocument(path), "reopen succeeded");
    CHECK(m2.typeForRow(1) == BlockModel::OrderedListItem && m2.depthForRow(1) == 1,
          "ordered type + depth round-trip (type %d, depth %d)",
          m2.typeForRow(1), m2.depthForRow(1));
    m2.noteCaret(2, 0, 2, 0);
    m2.indentBlocks(2, 2, 1);
    CHECK(m2.depthForRow(2) == 1, "indent after reopen");
    m2.undo();
    CHECK(m2.depthForRow(2) == 0, "undo restores depth");
    QFile::remove(path);
}

// --- Test 8: margin-ink storage + undo integration ---------------------------
// Tier-2 annotations: ink is an opaque blob per anchor block (block_ink
// table), snapshotted into undo entries so deleting a block and undoing
// restores its ink. setBlockInk is the single mutator (one undo step each).
static void testInkUndoPersist() {
    qInfo("[8] margin ink: set/undo/redo, delete-block restore, persistence");
    const QString kInk = QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"block-local\",\"space\":\"px\","
        "\"shapes\":[{\"id\":\"s1\",\"type\":\"freehand\",\"color\":[1,0,0,1],"
        "\"stroke_width\":4,\"filled\":false,\"is_modeled\":true,"
        "\"points\":[[-390.5,2.0],[10.0,44.5]]}]}");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_ink.mndb");
    QFile::remove(path);

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("alpha"));
    m.insertBlock(1); m.setContent(1, QStringLiteral("beta"));
    m.noteCaret(0, 0, 0, 0);
    QObject::connect(&m, &BlockModel::caretRestoreRequested, &m,
                     [&m](int r, int c, int ar, int ac) { m.noteCaret(r, c, ar, ac); });

    // Set → undo → redo.
    m.setBlockInk(0, kInk);
    CHECK(m.inkForRow(0) == kInk, "setBlockInk stored the blob");
    CHECK(m.dirty(), "ink edit marks the document dirty");
    m.undo();
    CHECK(m.inkForRow(0).isEmpty(), "undo removes the stroke");
    m.redo();
    CHECK(m.inkForRow(0) == kInk, "redo restores the stroke");

    // Identical blob → sameSnaps no-op (no extra undo entry).
    m.setBlockInk(0, kInk);
    m.undo();
    CHECK(m.inkForRow(0).isEmpty(), "identical re-set was a no-op (one undo clears)");
    m.redo();

    // Delete the inked block → ink gone; undo → block AND ink restored.
    m.removeBlock(0);
    CHECK(m.inkBlockIds().isEmpty(), "removeBlock drops the ink");
    m.undo();
    CHECK(m.contentForRow(0) == QStringLiteral("alpha") && m.inkForRow(0) == kInk,
          "undo of removeBlock restores block AND ink");

    // deleteRange across the inked block → same round trip.
    m.noteCaret(0, 0, 0, 0);
    m.deleteRange(0, 0, 1, 2);   // merges rows 0-1, deletes block 1... region includes row 0
    m.undo();
    CHECK(m.inkForRow(0) == kInk, "undo of deleteRange keeps the ink");

    // Typing coalesce in an inked block leaves ink intact through undo/redo.
    m.noteCaret(0, 5, 0, 5);
    m.insertText(0, 5, QStringLiteral("x"), 0, {}, {}); m.noteCaret(0, 6, 0, 6);
    m.insertText(0, 6, QStringLiteral("y"), 0, {}, {}); m.noteCaret(0, 7, 0, 7);
    m.undo();
    CHECK(m.inkForRow(0) == kInk, "ink survives a coalesced typing undo");

    // Persistence round-trip.
    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();
    BlockModel m2;
    CHECK(m2.openDocument(path), "reopen succeeded");
    int inked = -1;
    for (int i = 0; i < m2.rowCountQml(); ++i)
        if (!m2.inkForRow(i).isEmpty()) { inked = i; break; }
    CHECK(inked >= 0 && m2.inkForRow(inked) == kInk, "ink round-trips through save/reopen");
    {
        Document d;
        CHECK(d.open(path), "raw open");
        CHECK(d.schemaVersion() == Document::kSchemaVersion,
              "stamped v%d", Document::kSchemaVersion);
        d.close();
    }
    QFile::remove(path);
}

// --- Test 9: comments — span anchor, shift, orphan/undo, persistence ---------
// Tier-3 annotations: a SpanComment span (payload = thread id) rides the span
// machinery for free; thread bodies live in comment_* tables, are NOT undoable,
// and survive orphaning (span deleted) until deleteThread.
static void testComments() {
    qInfo("[9] comments: span anchor, shift, orphan/undo, persistence");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_comments.mndb");
    QFile::remove(path);

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    m.setContent(0, QStringLiteral("review this sentence please"));
    m.noteCaret(0, 0, 0, 0);
    QObject::connect(&m, &BlockModel::caretRestoreRequested, &m,
                     [&m](int r, int c, int ar, int ac) { m.noteCaret(r, c, ar, ac); });

    const QString tid = m.addComment(0, 7, 11);   // "this"
    CHECK(!tid.isEmpty(), "addComment mints a thread id");
    CHECK(m.commentAt(0, 8) == tid, "commentAt hits inside the range");
    CHECK(m.commentAt(0, 3).isEmpty(), "commentAt misses outside it");
    m.addCommentMessage(tid, QStringLiteral("First message"));
    CHECK(m.commentMessages(tid).size() == 1, "thread message stored");
    CHECK(m.commentPinRows().size() == 1, "one pin row");

    // The span rides edits: insert before it shifts the range.
    m.insertText(0, 0, QStringLiteral("XX"), 0, {}, {});
    CHECK(m.commentAt(0, 10) == tid, "span shifted with the insert");

    // Deleting the anchoring text ORPHANS the thread (body survives).
    m.noteCaret(0, 9, 0, 9);
    m.deleteRange(0, 9, 0, 13);
    CHECK(m.threadAnchorRow(tid) == -1, "span deletion orphans the thread");
    CHECK(m.commentMessages(tid).size() == 1, "orphaned thread keeps its body");
    m.undo();
    CHECK(m.threadAnchorRow(tid) == 0 && m.commentAt(0, 10) == tid,
          "undo re-links the thread");

    // Persistence round-trip (span in attrs as k:"comment", bodies in tables).
    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();
    BlockModel m2;
    CHECK(m2.openDocument(path), "reopen succeeded");
    CHECK(m2.commentAt(0, 10) == tid, "comment span round-trips");
    CHECK(m2.commentMessages(tid).size() == 1, "thread body round-trips");
    CHECK(m2.commentThreads().size() == 1, "thread listed");

    // Panel ordering is DOCUMENT order, not creation order: a newer thread
    // anchored ABOVE an older one lists first.
    m2.insertBlock(0);
    m2.setContent(0, QStringLiteral("prologue paragraph"));
    const QString tid2 = m2.addComment(0, 0, 8);
    const QVariantList ordered = m2.commentThreads();
    CHECK(ordered.size() == 2
              && ordered[0].toMap().value(QStringLiteral("id")).toString() == tid2,
          "threads sort by document position (new-above lists first)");

    // deleteThread: unlinks the span (undoable) and destroys the bodies (not).
    m2.deleteThread(tid);
    CHECK(m2.threadAnchorRow(tid) == -1, "deleteThread unlinked the span");
    CHECK(m2.commentMessages(tid).isEmpty(), "deleteThread cascaded the messages");
    QFile::remove(path);
}

// --- Test 10: Exporter — markdown emission over the model -------------------
// Drives the export walker headless with a recording sink: block mapping,
// overlapping-span nesting (whitespace-safe markers), links, comment
// footnotes, list depth/blank-line policy, code fences, pipe tables.
namespace {
class RecordingSink : public Exporter::AssetSink {
public:
    QStringList files, images;
    QString addFile(const QString&, const QString& baseName) override {
        files << baseName;
        return QStringLiteral("assets/") + baseName + QStringLiteral(".png");
    }
    QString addImage(const QImage&, const QString& baseName) override {
        images << baseName;
        return QStringLiteral("assets/") + baseName + QStringLiteral(".png");
    }
};
} // namespace

static void testExportMarkdown() {
    qInfo("[10] Exporter: markdown emission (blocks, spans, footnotes, table)");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);

    m.insertBlock(0); m.setContent(0, QStringLiteral("Title"));
    m.setHeading(0, 2);
    //                                                 0123456789...
    m.insertBlock(1); m.setContent(1, QStringLiteral("plain bold bolditalic italic link"));
    m.toggleFormat(1, 6, 21, QStringLiteral("bold"));      // "bold bolditalic"
    m.toggleFormat(1, 11, 28, QStringLiteral("italic"));   // "bolditalic italic"
    m.setLink(1, 29, 33, QStringLiteral("https://example.com"));
    const QString tid = m.addComment(1, 0, 5);             // footnote on "plain"
    m.addCommentMessage(tid, QStringLiteral("note body"));

    m.insertBlock(2); m.setContent(2, QStringLiteral("item one"));
    m.setBlockType(2, BlockModel::ListItem);
    m.insertBlock(3); m.setContent(3, QStringLiteral("sub item"));
    m.setBlockType(3, BlockModel::ListItem);
    m.indentBlocks(3, 3, 1);

    m.insertDivider(3);                                    // → row 4
    m.insertBlock(5); m.setContent(5, QStringLiteral("int x = 1;"));
    m.makeCodeBlock(5, QStringLiteral("cpp"));

    m.insertTable(5, 2, 2);                                // → row 6
    const int t = 6;
    m.tableSetHeaderRows(t, 1);
    m.tableSetCell(t, 0, 0, QStringLiteral("H1"));
    m.tableSetCell(t, 0, 1, QStringLiteral("H2"));
    m.tableSetCell(t, 1, 0, QStringLiteral("a"));
    m.tableSetCell(t, 1, 1, QStringLiteral("b|pipe"));

    Exporter ex;
    ex.setModel(&m);
    RecordingSink sink;
    const QString md = ex.toMarkdown(Exporter::Options{}, sink);

    CHECK(md.contains(QStringLiteral("## Title")), "heading level maps to ##");
    CHECK(md.contains(QStringLiteral(
              "plain[^1] **bold *bolditalic*** *italic* [link](https://example.com)")),
          "overlapping spans nest with whitespace-safe markers + footnote ref");
    CHECK(md.contains(QStringLiteral("- item one\n  - sub item")),
          "list run: single newline + 2-space depth indent");
    CHECK(md.contains(QStringLiteral("---")), "divider emits");
    CHECK(md.contains(QStringLiteral("```cpp\nint x = 1;\n```")), "code fence with language");
    CHECK(md.contains(QStringLiteral("| H1 | H2 |")), "table header row");
    CHECK(md.contains(QStringLiteral("| a | b\\|pipe |")), "table body row escapes pipes");
    CHECK(md.contains(QStringLiteral("[^1]: note body")), "footnote body emitted");
    const QVariantMap scan = ex.scan();
    CHECK(scan.value(QStringLiteral("videos")).toInt() == 0
              && scan.value(QStringLiteral("videoNotes")).toInt() == 0,
          "scan reports no videos/notes on a text doc");

    // --- Media ink → HTML z-layer: a synthetic image block with a
    // programmatic Frame-space ink blob must export an .inkwrap stack and
    // arm the Annotations toggle. ---
    {
        const QString imgPath = QDir::temp().filePath(QStringLiteral("mn_ink_test.png"));
        QImage probe(64, 48, QImage::Format_RGB32);
        probe.fill(Qt::darkGray);
        probe.save(imgPath, "PNG");
        CHECK(m.insertImageFromUrl(m.rowCountQml() - 1,
                                   QUrl::fromLocalFile(imgPath).toString()),
              "synthetic image block inserted");
        int imgRow = -1;
        for (int i = 0; i < m.rowCountQml(); ++i)
            if (m.typeForRow(i) == BlockModel::Media
                && m.mediaKind(i) == QLatin1String("image")) { imgRow = i; break; }
        CHECK(imgRow >= 0, "image row found");
        mn::DocInkAnchor a;
        a.space = mn::DocInkAnchor::Frame;
        qcv::ActiveStroke st;
        st.tool = qcv::DrawingTool::Freehand;
        st.points = { QPointF(0.1, 0.1), QPointF(0.9, 0.9) };
        st.strokeWidth = 4;
        a.strokes.push_back(st);
        m.setBlockInk(imgRow, mn::docInkToJson(a));
        RecordingSink isink;
        const QString ihtml = ex.toHtml(Exporter::Options{}, isink);
        CHECK(ihtml.contains(QStringLiteral("class=\"inkwrap\""))
                  && ihtml.contains(QStringLiteral("class=\"ink\"")),
              "media ink exports as a z-layer stack");
        CHECK(ihtml.contains(QStringLiteral("id=\"mn-ink\"")),
              "ink layer arms the Annotations toggle");
        // Page ink on a TEXT block exports too (Px space → positioned layer).
        mn::DocInkAnchor pa;
        pa.space = mn::DocInkAnchor::Px;
        qcv::ActiveStroke pst;
        pst.tool = qcv::DrawingTool::Freehand;
        pst.points = { QPointF(-40, 4), QPointF(60, 18) };   // page px around center
        pst.strokeWidth = 3;
        pa.strokes.push_back(pst);
        m.setBlockInk(1, mn::docInkToJson(pa));              // the spans paragraph
        RecordingSink psink;
        const QString phtml = ex.toHtml(Exporter::Options{}, psink);
        CHECK(phtml.contains(QStringLiteral("position:absolute;left:"))
                  && phtml.count(QStringLiteral("class=\"ink\"")) >= 2,
              "text-block page ink exports as a positioned layer");
        CHECK(phtml.contains(QStringLiteral("class=\"cmtcard\""))
                  && phtml.contains(QStringLiteral("note body")),
              "comment hover card rides inside the tinted range");

        // --- DOCX: real Word parts, native comments, embedded image. ---
        const QString docxPath = QDir::temp().filePath(QStringLiteral("mn_export_test.docx"));
        QFile::remove(docxPath);
        CHECK(ex.exportDocx(docxPath, true), "exportDocx wrote the file");
        QZipReader zr(docxPath);
        const QByteArray doc = zr.fileData(QStringLiteral("word/document.xml"));
        const QByteArray cmts = zr.fileData(QStringLiteral("word/comments.xml"));
        const QByteArray nums = zr.fileData(QStringLiteral("word/numbering.xml"));
        CHECK(doc.contains("Title") && doc.contains("w:commentRangeStart"),
              "document.xml carries content + native comment ranges");
        CHECK(cmts.contains("note body"), "comments.xml carries the thread body");
        CHECK(!nums.isEmpty() && doc.contains("w:numPr"),
              "numbering part present and lists reference it");
        CHECK(doc.contains("w:tbl") && doc.contains("w:gridCol"),
              "table exports with a grid");
        CHECK(!zr.fileData(QStringLiteral("word/media/image1.png")).isEmpty()
                  || !zr.fileData(QStringLiteral("word/media/image1.jpeg")).isEmpty(),
              "embedded media part present");
        QFile::remove(docxPath);
        QFile::remove(imgPath);
    }

    // End-to-end file path (the FileSink wrapper): write + read back.
    const QString outPath = QDir::temp().filePath(QStringLiteral("mn_export_test.md"));
    QFile::remove(outPath);
    CHECK(ex.exportMarkdown(outPath, true), "exportMarkdown wrote the file");
    QFile f(outPath);
    QString onDisk;
    if (f.open(QIODevice::ReadOnly)) { onDisk = QString::fromUtf8(f.readAll()); f.close(); }
    CHECK(onDisk.contains(QStringLiteral("## Title")), "written file round-trips content");
    QFile::remove(outPath);

    // --- HTML emitter over the same document (+ a color span, which HTML
    // keeps and markdown drops). ---
    m.setTextColor(1, 0, 5, QStringLiteral("#ff6f68"));   // color "plain"
    RecordingSink hsink;
    const QString html = ex.toHtml(Exporter::Options{}, hsink);
    CHECK(html.contains(QStringLiteral("Title</h2>"))
              && html.contains(QStringLiteral("class=\"bnum\"")),
          "HTML heading tag carries its block number");
    CHECK(html.contains(QStringLiteral("<span style=\"color:#ff6f68\">plain</span>")),
          "HTML keeps the color span markdown dropped");
    CHECK(html.contains(QStringLiteral("<strong>bold <em>bolditalic</em></strong>"))
              || html.contains(QStringLiteral("<strong>bold <em>bolditalic</em></strong><em>")),
          "HTML nests overlapping bold/italic");
    CHECK(html.contains(QStringLiteral("<a href=\"https://example.com\">link</a>")),
          "HTML link tag");
    CHECK(html.contains(QStringLiteral("class=\"cmt\""))
              && html.contains(QStringLiteral("href=\"#c1\"")),
          "commented range tints + links to the comments section");
    CHECK(html.contains(QStringLiteral("<ul>")) && html.contains(QStringLiteral("item one</li>")),
          "HTML list run opens a real <ul>");
    // Syntax colouring wraps tokens in inline-styled spans ("int" is a cpp
    // keyword; "=", "1", ";" get their own spans too), so assert the
    // structure + a coloured keyword + the identifier left plain between them.
    CHECK(html.contains(QStringLiteral("<pre><code class=\"language-cpp\">"))
              && html.contains(QStringLiteral(">int</span> x ")),
          "HTML code block with language class + syntax-coloured spans");
    CHECK(html.contains(QStringLiteral("<th>H1</th>")) || html.contains(QStringLiteral("<th >H1</th>")),
          "HTML table header cell");
    CHECK(html.contains(QStringLiteral("id=\"c1\"")) && html.contains(QStringLiteral("note body")),
          "comments section carries the thread body");
    CHECK(html.startsWith(QStringLiteral("<!doctype html>")), "self-contained document skeleton");
}

// --- Test 11: sketch canvas resize — renormalization + layout + undo --------
// sketchResizeCanvas applies per-side source-px deltas, rewriting every stroke
// point and image rect so the ink keeps its position (ovals: center shifts,
// radii only rescale). Must also re-derive media meta + Fenwick height (the
// setMediaWidth sequence), and round-trip through undo/persistence.
static void testSketchResizeRenorm() {
    qInfo("[11] sketch resize: renormalize strokes/images, heights, undo, persist");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_sketch.mndb");
    QFile::remove(path);

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("para"));
    const int row = m.insertSketch(0);
    CHECK(row == 1 && m.mediaW(row) == 480 && m.mediaH(row) == 480,
          "insertSketch seeds a 480x480 canvas at row 1");

    // Freehand (positions) + oval (center = position, radii = scale-only).
    m.sketchSetShapes(row, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
        "{\"id\":\"f1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[0.5,0.5],[0.25,0.75]]},"
        "{\"id\":\"o1\",\"type\":\"oval\",\"color\":[0,1,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":false,\"points\":[[0.5,0.5],[0.2,0.1]]}]}"));
    const QString probePng = QDir::temp().filePath(QStringLiteral("mn_sketch_probe.png"));
    { QImage probe(64, 48, QImage::Format_RGB32); probe.fill(Qt::darkGray);
      probe.save(probePng, "PNG"); }
    CHECK(m.sketchAddImageFromUrl(row, QUrl::fromLocalFile(probePng).toString()),
          "probe image placed into the sketch");
    CHECK(m.sketchAddText(row, 0.5, 0.5, 0.3, QStringLiteral("note"), 20,
                          QStringLiteral("#FF8800")) == 0,
          "text element added at index 0");
    const QString preResize = m.contentForRow(row);
    const double img0x = QJsonDocument::fromJson(preResize.toUtf8()).object()
        .value(QStringLiteral("images")).toArray().at(0).toObject()
        .value(QStringLiteral("x")).toDouble();
    const qreal hBefore = m.heightForRow(row);

    m.sketchResizeCanvas(row, 100, 50, 0, 0);   // grow left+top = origin shift
    const QJsonObject o = QJsonDocument::fromJson(m.contentForRow(row).toUtf8()).object();
    CHECK(o.value(QStringLiteral("w")).toInt() == 580
              && o.value(QStringLiteral("h")).toInt() == 530
              && m.mediaW(row) == 580 && m.mediaH(row) == 530,
          "frame 480x480 + (dl=100,dt=50) = 580x530, media meta re-derived");
    const QJsonArray shapes = o.value(QStringLiteral("shapes")).toArray();
    const QJsonArray fp = shapes.at(0).toObject()
        .value(QStringLiteral("points")).toArray().at(0).toArray();
    CHECK(qFuzzyCompare(fp.at(0).toDouble(), (0.5 * 480 + 100) / 580.0)
              && qFuzzyCompare(fp.at(1).toDouble(), (0.5 * 480 + 50) / 530.0),
          "freehand point renormalized exactly ((x*oldW+dl)/newW)");
    const QJsonArray oc = shapes.at(1).toObject()
        .value(QStringLiteral("points")).toArray().at(0).toArray();
    const QJsonArray orr = shapes.at(1).toObject()
        .value(QStringLiteral("points")).toArray().at(1).toArray();
    CHECK(qFuzzyCompare(oc.at(0).toDouble(), (0.5 * 480 + 100) / 580.0)
              && qFuzzyCompare(orr.at(0).toDouble(), 0.2 * 480 / 580.0)
              && qFuzzyCompare(orr.at(1).toDouble(), 0.1 * 480 / 530.0),
          "oval center shifted, radii rescaled WITHOUT origin shift");
    const QJsonObject img = o.value(QStringLiteral("images")).toArray().at(0).toObject();
    CHECK(qFuzzyCompare(img.value(QStringLiteral("x")).toDouble(),
                        (img0x * 480 + 100) / 580.0)
              && qFuzzyCompare(img.value(QStringLiteral("w")).toDouble(),
                               (64.0 / 480.0) * 480 / 580.0),
          "image rect: position shifted, size rescaled");
    const QJsonObject txt = o.value(QStringLiteral("texts")).toArray().at(0).toObject();
    CHECK(qFuzzyCompare(txt.value(QStringLiteral("x")).toDouble(), (0.5 * 480 + 100) / 580.0)
              && qFuzzyCompare(txt.value(QStringLiteral("y")).toDouble(), (0.5 * 480 + 50) / 530.0)
              && qFuzzyCompare(txt.value(QStringLiteral("w")).toDouble(), 0.3 * 480 / 580.0)
              && txt.value(QStringLiteral("size")).toDouble() == 20.0
              && txt.value(QStringLiteral("text")).toString() == QStringLiteral("note")
              && txt.value(QStringLiteral("color")).toString() == QStringLiteral("#FF8800"),
          "text element: position shifted, width rescaled, size/text/color untouched");
    CHECK(!qFuzzyCompare(hBefore, m.heightForRow(row)),
          "Fenwick height re-derived from the new aspect (%.1f -> %.1f)",
          hBefore, m.heightForRow(row));

    m.undo();
    CHECK(m.contentForRow(row) == preResize && m.mediaW(row) == 480,
          "undo restores byte-identical content + media meta");
    m.redo();
    CHECK(m.mediaW(row) == 580, "redo re-applies the resize");

    // Cap clamps (source px): grow far past max, shrink far past min.
    m.sketchResizeCanvas(row, 0, 0, 20000, 0);
    CHECK(m.mediaW(row) == 8192, "width clamps to 8192");
    m.sketchResizeCanvas(row, 0, 0, -20000, -20000);
    CHECK(m.mediaW(row) == 64 && m.mediaH(row) == 64, "frame clamps to 64 minimum");

    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();
    BlockModel m2;
    CHECK(m2.openDocument(path), "reopen succeeded");
    int srow = -1;
    for (int i = 0; i < m2.rowCountQml(); ++i)
        if (m2.mediaKind(i) == QLatin1String("sketch")) { srow = i; break; }
    CHECK(srow >= 0 && m2.mediaW(srow) == 64, "resized frame round-trips save/reopen");
    QFile::remove(path);
    QFile::remove(probePng);
}

// --- Test 13: sketch export — raster cap + HTML natural width ---------------
// renderSketch rasterizes at 2× until the output long edge would hit 8192,
// then 1×..2× proportional (a max canvas exports at 1×, never a GiB image);
// HTML shows the sketch at its SOURCE size (or dw), not the raster size.
static void testSketchExportCaps() {
    qInfo("[13] sketch export: raster cap at 8192, HTML width in source px");
    class SizeSink : public Exporter::AssetSink {
    public:
        QList<QSize> sizes;
        QImage last;
        QString addFile(const QString&, const QString& b) override {
            return QStringLiteral("assets/") + b + QStringLiteral(".png"); }
        QString addImage(const QImage& img, const QString& b) override {
            sizes << img.size();
            last = img;
            return QStringLiteral("assets/") + b + QStringLiteral(".png"); }
    };

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    const int row = m.insertSketch(-1);
    m.sketchSetShapes(row, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
        "{\"id\":\"f1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[0.1,0.1],[0.9,0.9]]}]}"));

    Exporter ex;
    ex.setModel(&m);

    SizeSink s1;
    QString html = ex.toHtml(Exporter::Options{}, s1);
    CHECK(s1.sizes.size() == 1 && s1.sizes[0] == QSize(960, 960),
          "480x480 canvas rasters at 2x (960x960)");
    CHECK(html.contains(QStringLiteral("class=\"sketch\""))
              && html.contains(QStringLiteral("width:480px")),
          "HTML sketch width is the SOURCE 480px, not the raster 960");

    m.sketchResizeCanvas(row, 0, 0, 6000 - 480, 3000 - 480);   // → 6000x3000
    SizeSink s2;
    html = ex.toHtml(Exporter::Options{}, s2);
    CHECK(s2.sizes.size() == 1 && s2.sizes[0] == QSize(8192, 4096),
          "6000x3000 canvas caps at output long edge 8192 (got %dx%d)",
          s2.sizes.isEmpty() ? 0 : s2.sizes[0].width(),
          s2.sizes.isEmpty() ? 0 : s2.sizes[0].height());
    CHECK(html.contains(QStringLiteral("width:6000px")),
          "HTML width follows the resized source frame");

    m.setMediaWidth(row, 700);                                  // dw override
    SizeSink s3;
    html = ex.toHtml(Exporter::Options{}, s3);
    CHECK(html.contains(QStringLiteral("width:700px;max-width:none")),
          "dw override rides the export like the image branch");

    // Text-only sketch: baked into the raster (dimensions unchanged, glyph
    // pixels present under any resolved font).
    {
        BlockModel tm;
        tm.newDocument();
        while (tm.rowCountQml() > 0) tm.removeBlock(0);
        const int trow = tm.insertSketch(-1);
        tm.sketchAddText(trow, 0.1, 0.1, 0.5, QStringLiteral("baked label"), 32,
                         QStringLiteral("#FFFFFF"));
        Exporter tex;
        tex.setModel(&tm);
        SizeSink ts;
        tex.toHtml(Exporter::Options{}, ts);
        CHECK(ts.sizes.size() == 1 && ts.sizes[0] == QSize(960, 960),
              "text-only sketch rasters at 2x, dimensions unchanged");
        bool lit = false;
        for (int yy = 0; yy < ts.last.height() && !lit; yy += 2)
            for (int xx = 0; xx < ts.last.width() && !lit; xx += 2)
                if (qAlpha(ts.last.pixel(xx, yy)) != 0) lit = true;
        CHECK(lit, "text glyphs baked into the raster");
    }
}

// --- Test 12: sketch fit-to-ink — signed overflow coords, bbox resize -------
// Overflow ink stores as coords < 0 / > 1 (signed normalized, no clamping
// anywhere in the pipeline); sketchFitToInk grows/shrinks the frame to the
// signed bbox + 8px margin, pulling everything back into [0,1].
static void testSketchFitToInk() {
    qInfo("[12] sketch fit-to-ink: signed coords survive, bbox resize");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    const int row = m.insertSketch(-1);

    CHECK(!m.sketchFitToInk(row), "empty sketch: fit is a no-op (false)");

    m.sketchSetShapes(row, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
        "{\"id\":\"f1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,"
        "\"points\":[[-0.1,0.2],[0.5,0.5],[1.2,0.9]]}]}"));
    CHECK(m.contentForRow(row).contains(QStringLiteral("-0.1")),
          "signed overflow coords round-trip the model untouched");

    CHECK(m.sketchFitToInk(row), "fit-to-ink resizes");
    // bbox src: x [-48,576] pad 2 -> [-50,578] margin 8 -> [-58,586] => w 644
    //           y [96,432]  pad 2 -> [94,434]  margin 8 -> [86,442]  => h 356
    CHECK(m.mediaW(row) == 644 && m.mediaH(row) == 356,
          "frame = signed bbox + stroke pad + 8px margin (got %dx%d)",
          m.mediaW(row), m.mediaH(row));
    const QJsonArray pts = QJsonDocument::fromJson(m.contentForRow(row).toUtf8())
        .object().value(QStringLiteral("shapes")).toArray().at(0).toObject()
        .value(QStringLiteral("points")).toArray();
    bool allIn = true;
    for (const QJsonValue& v : pts) {
        const QJsonArray p = v.toArray();
        allIn = allIn && p.at(0).toDouble() >= 0.0 && p.at(0).toDouble() <= 1.0
                      && p.at(1).toDouble() >= 0.0 && p.at(1).toDouble() <= 1.0;
    }
    CHECK(allIn, "all points pulled inside [0,1] after fit");
    CHECK(!m.sketchFitToInk(row), "second fit is a no-op (frame already fits)");
}

// --- Test 15: sketch text elements — invokables, undo, fit-to-ink ------------
// texts[] = {x,y,w,text,size,color}: height is DERIVED (never stored), blank
// setText deletes (the overlay's empty-commit contract), fit-to-ink uses the
// same layout helper the model does so the check is font-agnostic.
static void testSketchTextElements() {
    qInfo("[15] sketch text: add/set/box/remove, blank-deletes, fit-to-ink");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_text.mndb");
    QFile::remove(path);
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    const int row = m.insertSketch(-1);
    auto texts = [&m, row] {
        return QJsonDocument::fromJson(m.contentForRow(row).toUtf8()).object()
            .value(QStringLiteral("texts")).toArray();
    };

    CHECK(m.sketchAddText(row, 0.1, 0.2, 0.4, QStringLiteral("   "), 24,
                          QStringLiteral("#FF5768")) == -1,
          "blank text rejected (no element, no undo step)");
    const int idx = m.sketchAddText(row, 0.1, 0.2, 0.001,
        QStringLiteral("hello wrap world, a label long enough to wrap"),
        24, QStringLiteral("#FF5768"));
    CHECK(idx == 0, "text added at index 0");
    CHECK(qFuzzyCompare(texts().at(0).toObject().value(QStringLiteral("w")).toDouble(),
                        2.0 * 24 / 480.0),
          "width clamped to the 2em floor");
    m.undo();
    CHECK(texts().isEmpty(), "one undo removes the add");
    m.redo();
    CHECK(texts().size() == 1, "redo restores it");

    m.sketchSetText(row, 0, QStringLiteral("edited"));
    CHECK(texts().at(0).toObject().value(QStringLiteral("text")).toString()
              == QStringLiteral("edited"), "setText replaces content (one txn)");
    m.sketchSetText(row, 0, QStringLiteral("edited"));   // unchanged → no txn
    m.undo();
    CHECK(texts().at(0).toObject().value(QStringLiteral("text")).toString()
              .startsWith(QStringLiteral("hello")),
          "unchanged setText added no undo step (one undo crosses the edit)");
    m.redo();

    m.sketchSetText(row, 0, QStringLiteral("  "));
    CHECK(texts().isEmpty(), "blank setText DELETES the element");
    m.undo();
    CHECK(texts().size() == 1
              && texts().at(0).toObject().value(QStringLiteral("text")).toString()
                     == QStringLiteral("edited"),
          "undo resurrects the deleted element");

    m.sketchSetTextBox(row, 0, 0.3, 0.4, 0.5, 24);
    const QJsonObject tb = texts().at(0).toObject();
    CHECK(qFuzzyCompare(tb.value(QStringLiteral("x")).toDouble(), 0.3)
              && qFuzzyCompare(tb.value(QStringLiteral("w")).toDouble(), 0.5),
          "setTextBox moves/resizes");

    // Fit-to-ink expectation computed through the SAME helper + margin math
    // the model uses — holds under any resolved font.
    mn::SketchTextSpec spec;
    spec.text = tb.value(QStringLiteral("text")).toString();
    spec.x = 0.3; spec.y = 0.4; spec.w = 0.5; spec.size = 24;
    spec.family = mn::sketchTextFamily();
    const QRectF r = mn::sketchTextRectSrc(spec, 480, 480);
    const double eps = 1e-6, margin = 8.0;
    const int dl = -int(std::floor(r.left() - margin + eps));
    const int dt = -int(std::floor(r.top() - margin + eps));
    const int dr = int(std::ceil(r.right() + margin - eps)) - 480;
    const int db = int(std::ceil(r.bottom() + margin - eps)) - 480;
    const int expW = std::clamp(480 + dl + dr, 64, 8192);
    const int expH = std::clamp(480 + dt + db, 64, 8192);
    CHECK(m.sketchFitToInk(row), "fit-to-ink on a text-only sketch resizes");
    CHECK(m.mediaW(row) == expW && m.mediaH(row) == expH,
          "frame = derived text rect + margin (got %dx%d want %dx%d)",
          m.mediaW(row), m.mediaH(row), expW, expH);

    // R7: a stroke edit merges through sketchSetShapes — texts[] must survive
    // the merge byte-identically (unknown-key preservation, the old-build
    // compatibility guarantee).
    const QString preShapes = QString::fromUtf8(QJsonDocument(
        QJsonDocument::fromJson(m.contentForRow(row).toUtf8()).object()
            .value(QStringLiteral("texts")).toArray().at(0).toObject()
        ).toJson(QJsonDocument::Compact));
    m.sketchSetShapes(row, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
        "{\"id\":\"s9\",\"type\":\"line\",\"color\":[0,0,1,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":false,\"points\":[[0.1,0.1],[0.9,0.9]]}]}"));
    const QString postShapes = QString::fromUtf8(QJsonDocument(
        QJsonDocument::fromJson(m.contentForRow(row).toUtf8()).object()
            .value(QStringLiteral("texts")).toArray().at(0).toObject()
        ).toJson(QJsonDocument::Compact));
    CHECK(postShapes == preShapes, "texts[] survives a stroke-edit merge byte-identically");

    m.sketchRemoveText(row, 0);
    CHECK(texts().isEmpty(), "removeText deletes");
    m.undo();
    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();
    BlockModel m2;
    CHECK(m2.openDocument(path), "reopen succeeded");
    int srow = -1;
    for (int i = 0; i < m2.rowCountQml(); ++i)
        if (m2.mediaKind(i) == QLatin1String("sketch")) { srow = i; break; }
    CHECK(srow >= 0 && m2.contentForRow(srow).contains(QStringLiteral("\"texts\"")),
          "texts[] round-trips save/reopen");
    QFile::remove(path);
}

// --- Test 14: sketch embed width — natural size, dw honoured -----------------
// Sketches left the "always page-wide" special case (only PDFs keep it):
// natural canvas width up to the page, dw drag-resize like any image.
static void testSketchEmbedWidth() {
    qInfo("[14] sketch embed width: natural up to page, dw override");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    const int row = m.insertSketch(-1);
    m.setContentWidth(760.0);
    CHECK(m.mediaDispWidth(row) == 480,
          "480 canvas embeds at natural 480, not page-wide (got %d)",
          m.mediaDispWidth(row));
    m.sketchResizeCanvas(row, 0, 0, 1520, 0);   // → 2000 wide
    CHECK(m.mediaDispWidth(row) == 760,
          "2000 canvas caps at the page measure (got %d)", m.mediaDispWidth(row));
    m.setMediaWidth(row, 900);
    CHECK(m.mediaDispWidth(row) == 900,
          "dw override honoured past the page (got %d)", m.mediaDispWidth(row));
    m.setMediaWidth(row, 0);
    CHECK(m.mediaDispWidth(row) == 760, "dw reset returns to the default");
}

// --- Test 16: doc-ink text chips — envelope round-trip + export presence ----
// texts[] is a TYPED DocInkAnchor member: the writer rebuilds the envelope
// from the struct, so this locks the critical mechanic — a stroke commit
// (parse → append stroke → serialize) must never destroy texts, and a
// text-only anchor must not serialize to "" (the delete-the-row signal).
static void testDocInkTexts() {
    qInfo("[16] doc-ink texts: envelope round-trip, stroke-commit survival");

    mn::SketchTextSpec t;
    t.text = QStringLiteral("margin note");
    t.x = -120.0; t.y = 40.0; t.w = 240.0;   // px space: x is Δ from page CENTER
    t.size = 16.0;
    t.color = QColor(QStringLiteral("#FF5768"));

    // Mixed anchor: one stroke + one text → full field fidelity.
    mn::DocInkAnchor a;
    a.space = mn::DocInkAnchor::Px;
    qcv::ActiveStroke st;
    st.tool = qcv::DrawingTool::Freehand;
    st.points = { QPointF(-50.0, 10.0), QPointF(80.0, 90.0) };
    st.strokeWidth = 4;
    a.strokes.push_back(st);
    a.texts.push_back(t);

    const QString blob = mn::docInkToJson(a);
    mn::DocInkAnchor b;
    CHECK(mn::docInkFromJson(blob, b) && b.strokes.size() == 1 && b.texts.size() == 1,
          "mixed anchor round-trips");
    CHECK(b.texts[0].text == t.text && qFuzzyCompare(b.texts[0].x, t.x)
              && qFuzzyCompare(b.texts[0].w, t.w) && qFuzzyCompare(b.texts[0].size, t.size)
              && b.texts[0].color.name() == QStringLiteral("#ff5768"),
          "text fields survive with fidelity (incl. negative x)");

    // THE mechanic: a stroke commit is parse → append → serialize.
    mn::DocInkAnchor c = b;
    qcv::ActiveStroke st2 = st;
    st2.points = { QPointF(0.0, 0.0), QPointF(10.0, 10.0) };
    c.strokes.push_back(st2);
    mn::DocInkAnchor d;
    CHECK(mn::docInkFromJson(mn::docInkToJson(c), d)
              && d.strokes.size() == 2 && d.texts.size() == 1
              && d.texts[0].text == t.text,
          "stroke commit (parse->append->serialize) preserves texts");

    // Text-only anchor: must NOT serialize to "" (the delete signal).
    mn::DocInkAnchor onlyText;
    onlyText.space = mn::DocInkAnchor::Frame;
    mn::SketchTextSpec ft = t;
    ft.x = 0.1; ft.y = 0.1; ft.w = 0.4; ft.size = 24;   // frame space: normalized
    onlyText.texts.push_back(ft);
    const QString tBlob = mn::docInkToJson(onlyText);
    CHECK(!tBlob.isEmpty(), "text-only anchor serializes non-empty");
    mn::DocInkAnchor e;
    CHECK(mn::docInkFromJson(tBlob, e) && e.strokes.empty() && e.texts.size() == 1
              && e.space == mn::DocInkAnchor::Frame,
          "text-only anchor survives round-trip with its space");

    // Legacy stroke-only blob (no texts key) parses clean; empty anchor = "".
    mn::DocInkAnchor legacyIn;
    legacyIn.space = mn::DocInkAnchor::Px;
    legacyIn.strokes.push_back(st);
    const QString legacy = mn::docInkToJson(legacyIn);
    CHECK(!legacy.contains(QStringLiteral("\"texts\"")),
          "texts key omitted when empty (old blobs stay byte-stable)");
    mn::DocInkAnchor f;
    CHECK(mn::docInkFromJson(legacy, f) && f.texts.empty(), "legacy blob -> empty texts");
    CHECK(mn::docInkToJson(mn::DocInkAnchor{}).isEmpty(),
          "truly empty anchor still serializes to the delete signal");

    // Truth table.
    CHECK(mn::docInkHasStrokes(blob) && mn::docInkHasContent(blob),
          "mixed: hasStrokes && hasContent");
    CHECK(!mn::docInkHasStrokes(tBlob) && mn::docInkHasContent(tBlob),
          "text-only: !hasStrokes && hasContent");
    CHECK(!mn::docInkHasContent(QString()), "empty: no content");

    // --- Export presence: text-only anchors reach BOTH renderers ---
    class InkSink : public Exporter::AssetSink {
    public:
        QList<QImage> images;
        QString addFile(const QString&, const QString& b) override {
            return QStringLiteral("assets/") + b + QStringLiteral(".png"); }
        QString addImage(const QImage& img, const QString& b) override {
            images << img;
            return QStringLiteral("assets/") + b + QStringLiteral(".png"); }
    };
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("annotated paragraph"));
    const QString probePng = QDir::temp().filePath(QStringLiteral("mn_ink_text_probe.png"));
    { QImage probe(64, 48, QImage::Format_RGB32); probe.fill(Qt::darkGray);
      probe.save(probePng, "PNG"); }
    CHECK(m.insertImageFromUrl(0, QUrl::fromLocalFile(probePng).toString()),
          "image block inserted for the frame anchor");
    int imgRow = -1;
    for (int i = 0; i < m.rowCountQml(); ++i)
        if (m.mediaKind(i) == QLatin1String("image")) { imgRow = i; break; }
    mn::DocInkAnchor pxA;
    pxA.space = mn::DocInkAnchor::Px;
    pxA.texts.push_back(t);
    m.setBlockInk(0, mn::docInkToJson(pxA));           // px text-only anchor
    m.setBlockInk(imgRow, mn::docInkToJson(onlyText)); // frame text-only anchor

    Exporter ex;
    ex.setModel(&m);
    InkSink sink;
    const QString html = ex.toHtml(Exporter::Options{}, sink);
    CHECK(html.contains(QStringLiteral("id=\"mn-ink\"")),
          "text-only ink arms the Annotations toggle");
    CHECK(html.contains(QStringLiteral("class=\"ink\"")), "ink layer(s) emitted");
    bool anyLit = false;
    for (const QImage& im : sink.images)
        for (int yy = 0; yy < im.height() && !anyLit; yy += 2)
            for (int xx = 0; xx < im.width() && !anyLit; xx += 2)
                if (qAlpha(im.pixel(xx, yy)) != 0) anyLit = true;
    CHECK(sink.images.size() >= 2 && anyLit,
          "chips baked into the ink rasters (%d ink images)", int(sink.images.size()));

    // Undo semantics ride setBlockInk (one step per call — the test-8 rule).
    m.setBlockInk(0, QString());
    CHECK(!mn::docInkHasContent(m.inkForRow(0)), "text anchor cleared");
    m.undo();
    CHECK(mn::docInkHasContent(m.inkForRow(0)) && !mn::docInkHasStrokes(m.inkForRow(0)),
          "one undo restores the text-only anchor");
    QFile::remove(probePng);
}

int main(int argc, char** argv) {
    // Uses the native platform (the test creates no windows). QGuiApplication —
    // not QCoreApplication — because BlockModel/MediaStore touch QImage/QPixmap.
    QGuiApplication app(argc, argv);
    app.setApplicationName("minNotes");
    app.setOrganizationName("minNotes");
    // Register the app text font so sketch-text height derivation matches the
    // app. Non-fatal if missing — text assertions are font-relative (computed
    // through the same helper the code under test uses).
    QFontDatabase::addApplicationFont(
        QStringLiteral(MN_FONTS_DIR "/Aspekta-400.ttf"));

    qInfo("=== minNotes regression pass ===");
    testCountNotify();
    testCommitMarkdown();
    testUndoRedoHeights();
    testSaveReopen();
    testUndoBranchCoalesce();
    testCanonicalizeAndStamp();
    testListsAndDepth();
    testInkUndoPersist();
    testComments();
    testExportMarkdown();
    testSketchResizeRenorm();
    testSketchFitToInk();
    testSketchExportCaps();
    testSketchEmbedWidth();
    testSketchTextElements();
    testDocInkTexts();

    if (g_fail == 0) qInfo("=== ALL CHECKS PASSED ===");
    else             qCritical("=== %d CHECK(S) FAILED ===", g_fail);
    return g_fail;
}

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
#include "Importer.h"
#include "TableGrid.h"
#include <QTextDocument>
#include "../app/notes/doc_ink.h"
#include "../app/notes/sketch_text.h"
#include "PackageFormat.h"
#include "PackageExporter.h"
#include "DocumentMerger.h"
#include "RtfConvert.h"
#include <private/qzipreader_p.h>
#include <private/qzipwriter_p.h>

#include <QFontDatabase>

#include <QGuiApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
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
static void testPageWidth() {
    qInfo("[8b] page width: edge-affinity migration, undo atomicity, persistence");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_pw.mndb");
    QFile::remove(path);

    // One px anchor with: a LEFT-margin stroke (bbox fully left of the 760
    // column), an in-COLUMN stroke, and a RIGHT-margin chip.
    mn::DocInkAnchor a;
    a.space = mn::DocInkAnchor::Px;
    qcv::ActiveStroke left;
    left.tool = qcv::DrawingTool::Freehand;
    left.points = { QPointF(-460.0, 2.0), QPointF(-420.0, 40.0) };
    left.strokeWidth = 4;
    qcv::ActiveStroke center;
    center.tool = qcv::DrawingTool::Freehand;
    center.points = { QPointF(-10.0, 2.0), QPointF(10.0, 40.0) };
    center.strokeWidth = 4;
    a.strokes.push_back(left);
    a.strokes.push_back(center);
    mn::SketchTextSpec chip;
    chip.text = QStringLiteral("margin note");
    chip.x = 420.0; chip.y = 10.0; chip.w = 90.0; chip.size = 16.0;
    chip.color = QColor(QStringLiteral("#FF5768"));
    a.texts.push_back(chip);
    const QString kInk = mn::docInkToJson(a);

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("alpha"));
    m.noteCaret(0, 0, 0, 0);
    m.setBlockInk(0, kInk);
    CHECK(qFuzzyCompare(m.pageWidth(), 760.0), "fresh doc reads the classic 760");

    // 760 → 1000: Δw/2 = 120. Left marginalia shifts -120 (keeps its content
    // position against the FIXED left edge), right marginalia +120, the
    // in-column stroke stays center-relative.
    m.setPageWidth(1000);
    CHECK(qFuzzyCompare(m.pageWidth(), 1000.0), "setPageWidth applied");
    mn::DocInkAnchor mig;
    CHECK(mn::docInkFromJson(m.inkForRow(0), mig)
              && mig.strokes.size() == 2 && mig.texts.size() == 1,
          "migrated blob keeps every element");
    CHECK(qFuzzyCompare(mig.strokes[0].points[0].x(), -580.0)
              && qFuzzyCompare(mig.strokes[0].points[1].x(), -540.0),
          "left-margin stroke shifted by -dW/2 (content position kept)");
    CHECK(qFuzzyCompare(mig.strokes[1].points[0].x(), -10.0),
          "in-column stroke stays center-relative");
    CHECK(qFuzzyCompare(mig.texts[0].x, 540.0),
          "right-margin chip shifted by +dW/2");

    // ONE undo restores blobs AND width atomically; redo re-applies both.
    m.undo();
    CHECK(qFuzzyCompare(m.pageWidth(), 760.0), "undo restores the width");
    mn::DocInkAnchor back;
    CHECK(mn::docInkFromJson(m.inkForRow(0), back)
              && qFuzzyCompare(back.strokes[0].points[0].x(), -460.0)
              && qFuzzyCompare(back.texts[0].x, 420.0),
          "the SAME undo restores the pre-migration ink (atomic)");
    m.redo();
    CHECK(qFuzzyCompare(m.pageWidth(), 1000.0), "redo re-applies the width");
    mn::DocInkAnchor fwd;
    CHECK(mn::docInkFromJson(m.inkForRow(0), fwd)
              && qFuzzyCompare(fwd.strokes[0].points[0].x(), -580.0),
          "redo re-applies the migrated ink");

    // Pure width change (no ink): still one undoable step.
    m.setBlockInk(0, QString());
    m.setPageWidth(1200);
    CHECK(qFuzzyCompare(m.pageWidth(), 1200.0), "pure width change applied");
    m.undo();
    CHECK(qFuzzyCompare(m.pageWidth(), 1000.0), "pure width change undoes");
    m.undo();   // restores the ink blob cleared above

    // Persistence: the width travels through save/reopen (doc_meta v3).
    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();
    CHECK(qFuzzyCompare(m.pageWidth(), 760.0), "close resets to 760");
    BlockModel m2;
    CHECK(m2.openDocument(path), "reopen succeeded");
    CHECK(qFuzzyCompare(m2.pageWidth(), 1000.0), "page width persisted through doc_meta");
    m2.closeDocument();
    QFile::remove(path);
}

static void testUndoHistory() {
    qInfo("[8c] undo history: active path, labels, jump time-travel");
    mn::DocInkAnchor a;
    a.space = mn::DocInkAnchor::Px;
    qcv::ActiveStroke st;
    st.tool = qcv::DrawingTool::Freehand;
    st.points = { QPointF(-450.0, 2.0), QPointF(-420.0, 30.0) };
    st.strokeWidth = 4;
    a.strokes.push_back(st);
    const QString kInk = mn::docInkToJson(a);

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("alpha"));
    m.noteCaret(0, 0, 0, 0);
    QObject::connect(&m, &BlockModel::caretRestoreRequested, &m,
                     [&m](int r, int c, int ar, int ac) { m.noteCaret(r, c, ar, ac); });

    const int rev0 = m.undoRevision();
    m.setBlockInk(0, kInk);      // → "Ink"
    m.setPageWidth(1000);        // → "Page width 1000" (grouped w/ migration)
    m.insertBlock(1);            // → "Insert block"
    CHECK(m.undoRevision() > rev0, "undoRevision advances with the stack");

    const QVariantList h = m.undoHistory();
    CHECK(h.size() >= 4, "history has baseline + the three entries");
    CHECK(h.first().toMap().value("label").toString() == QStringLiteral("Opened"),
          "baseline row leads");
    QStringList labels;
    for (const QVariant& v : h) labels << v.toMap().value("label").toString();
    CHECK(labels.contains(QStringLiteral("Ink")), "ink entry labeled");
    CHECK(labels.contains(QStringLiteral("Page width 1000")), "width entry labeled");
    CHECK(labels.contains(QStringLiteral("Insert block")), "insert entry labeled");
    CHECK(h.last().toMap().value("current").toBool(), "current = the newest state");

    // Time-travel to the baseline: one call unwinds ink + width + insert.
    m.undoJumpTo(-1);
    CHECK(qFuzzyCompare(m.pageWidth(), 760.0) && m.inkForRow(0).isEmpty()
              && m.rowCountQml() == 1,
          "jump to baseline undoes everything");
    const QVariantList h2 = m.undoHistory();
    CHECK(h2.first().toMap().value("current").toBool(), "baseline is now current");
    CHECK(h2.last().toMap().value("future").toBool(), "the leaf reads as future");

    // And forward again to the leaf in one jump.
    const int leaf = h2.last().toMap().value("idx").toInt();
    m.undoJumpTo(leaf);
    CHECK(qFuzzyCompare(m.pageWidth(), 1000.0) && m.rowCountQml() == 2
              && !m.inkForRow(0).isEmpty(),
          "jump forward replays to the leaf");
    // A stale/off-path target is a safe no-op.
    m.undoJumpTo(9999);
    CHECK(m.rowCountQml() == 2, "off-path jump target is a no-op");
}

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

    CHECK(md.startsWith(QStringLiteral("`Untitled`\n\n")),
          "document-name label leads the markdown (code-formatted, 2026-08-21)");
    // Copy as Markdown (2026-08-20): whole doc keeps the header; an explicit
    // range omits it and carries only its rows (asset-free clipboard form).
    {
        const QString all = ex.copyMarkdown(-1, -1);
        CHECK(all.startsWith(QStringLiteral("`Untitled`")),
              "copyMarkdown whole-doc keeps the header");
        const QString range = ex.copyMarkdown(2, 3);
        CHECK(!range.contains(QStringLiteral("`Untitled`"))
                  && !range.contains(QStringLiteral("## Title"))
                  && range.contains(QStringLiteral("- item one")),
              "copyMarkdown range: no header, no out-of-range rows");
        CHECK(ex.copyMarkdown(1, 1).contains(QStringLiteral("[^1]")),
              "range copy still numbers its footnotes");
    }
    // Absolute image destinations become encoded file:// URLs (2026-08-20,
    // user-caught: a bare "</Volumes/… with spaces>" is the least portable
    // CommonMark destination form).
    {
        const QString imgPng = QDir::temp().filePath(QStringLiteral("mn_md_dest.png"));
        { QImage probe(6, 4, QImage::Format_RGB32); probe.fill(Qt::darkYellow);
          probe.save(imgPng, "PNG"); }
        const int ir = m.insertImageFromUrl(m.rowCountQml() - 1,
                                            QUrl::fromLocalFile(imgPng).toString());
        CHECK(ir > 0, "dest-check image inserted");
        const QString one = ex.copyMarkdown(ir, ir);
        CHECK(one.contains(QStringLiteral("](file://"))
                  && !one.contains(QStringLiteral("](</")),
              "clipboard image dest is an encoded file:// URL");
    }
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
                                   QUrl::fromLocalFile(imgPath).toString()) >= 0,
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
        // Code colors (2026-08-20): the cpp block emits COLORED runs that
        // still carry the Courier + EFEFEF code identity (the round-trip
        // sniff). This doc has no colored spans, so any w:color inside a
        // Courier run is the syntax highlighter's.
        CHECK(doc.contains("Courier New") && doc.contains("w:color")
                  && doc.contains("EFEFEF"),
              "code block exports as colored runs with the code identity");
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
    // Header-AGNOSTIC exports (user ruling 2026-08-20): every row is a plain
    // <td> row — the header flag can't be trusted on sheet imports.
    CHECK(html.contains(QStringLiteral("<td>H1</td>")) && !html.contains(QStringLiteral("<th")),
          "HTML table rows are header-agnostic (all <td>)");
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
    CHECK(row == 1 && m.mediaW(row) == 760 && m.mediaH(row) == 480,
          "insertSketch seeds a page-width x 480 canvas (2026-08-21 ruling)");
    m.sketchResizeCanvas(row, 0, 0, 480 - m.mediaW(row), 0);   // classic 480x480 fixture

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
    m.sketchResizeCanvas(row, 0, 0, 480 - m.mediaW(row), 480 - m.mediaH(row));   // classic 480x480 fixture
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
        tm.sketchResizeCanvas(trow, 0, 0, 480 - tm.mediaW(trow), 480 - tm.mediaH(trow));
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
    m.sketchResizeCanvas(row, 0, 0, 480 - m.mediaW(row), 480 - m.mediaH(row));   // classic 480x480 fixture

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
    m.sketchResizeCanvas(row, 0, 0, 480 - m.mediaW(row), 480 - m.mediaH(row));   // classic 480x480 fixture
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
    CHECK(m.mediaDispWidth(row) == 760,
          "sketches fill the page (2026-08-21 ruling; got %d)",
          m.mediaDispWidth(row));
    m.setContentWidth(1200.0);
    CHECK(m.mediaDispWidth(row) == 1200,
          "and TRACK page-width changes (got %d)", m.mediaDispWidth(row));
    m.setContentWidth(760.0);
    m.setMediaWidth(row, 900);
    CHECK(m.mediaDispWidth(row) == 900,
          "dw override honoured past the page (got %d)", m.mediaDispWidth(row));
    m.setMediaWidth(row, 0);
    CHECK(m.mediaDispWidth(row) == 760, "dw reset returns to page-fill");
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
    CHECK(m.insertImageFromUrl(0, QUrl::fromLocalFile(probePng).toString()) >= 0,
          "image block inserted for the frame anchor");
    int imgRow = -1;
    for (int i = 0; i < m.rowCountQml(); ++i)
        if (m.mediaKind(i) == QLatin1String("image")) { imgRow = i; break; }
    mn::DocInkAnchor pxA;
    pxA.space = mn::DocInkAnchor::Px;
    pxA.texts.push_back(t);
    // Margin-spanning chips: LEFT gutter (x from page CENTER, page = ±380,
    // gutter to ±500) and RIGHT gutter — the layer bbox must exceed the 760
    // block, which the global img{max-width:100%} used to squeeze.
    mn::SketchTextSpec lm = t; lm.x = -500.0; lm.w = 110.0;
    mn::SketchTextSpec rm = t; rm.x = 390.0;  rm.w = 110.0;
    pxA.texts.push_back(lm);
    pxA.texts.push_back(rm);
    m.setBlockInk(0, mn::docInkToJson(pxA));           // px text-only anchor
    // Frame anchor gains an OVERSHOOTING chip (x < 0 = margin overshoot):
    // the layer raster must widen past the frame and carry percent offsets.
    mn::DocInkAnchor frameA = onlyText;
    mn::SketchTextSpec ov = onlyText.texts[0];
    ov.x = -0.5; ov.y = 0.2; ov.w = 0.4; ov.size = 8;
    frameA.texts.push_back(ov);
    m.setBlockInk(imgRow, mn::docInkToJson(frameA));   // frame anchor w/ overshoot

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
    // Margin-spanning layer: wider than the 760 block (2x raster), and the
    // tag opts out of the global img{max-width:100%} clamp that squeezed it.
    int widest = 0;
    for (const QImage& im : sink.images) widest = std::max(widest, im.width());
    CHECK(widest > 1520, "margin-spanning ink raster exceeds the block width (%d)", widest);
    {   // Inspection artifact: the emitted page for eyeballing ink geometry.
        QFile hf(QDir::tempPath() + QStringLiteral("/mn_ink_export_probe.html"));
        if (hf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            hf.write(html.toUtf8());
    }
    CHECK(html.contains(QStringLiteral("class=\"ink\" style=\"position:absolute"))
              && html.contains(QStringLiteral("max-width:none;z-index:2")),
          "page-ink layer escapes the max-width clamp");
    CHECK(html.contains(QStringLiteral("left:-50%"))
              && html.contains(QStringLiteral("right:auto;bottom:auto;max-width:none")),
          "overshooting media ink carries percent offsets (not frame-clipped)");
    CHECK(html.contains(QStringLiteral("id=\"mn-lb\""))
              && html.contains(QStringLiteral("ArrowRight"))
              && html.contains(QStringLiteral("cursor:zoom-in")),
          "lightbox chrome + script + zoom cursor emitted");

    // Undo semantics ride setBlockInk (one step per call — the test-8 rule).
    m.setBlockInk(0, QString());
    CHECK(!mn::docInkHasContent(m.inkForRow(0)), "text anchor cleared");
    m.undo();
    CHECK(mn::docInkHasContent(m.inkForRow(0)) && !mn::docInkHasStrokes(m.inkForRow(0)),
          "one undo restores the text-only anchor");
    QFile::remove(probePng);
}

// --- Test 17: empty-anchor consumption — inserts replace an empty block ----
// Inserting a new block AFTER an empty paragraph consumes it: the new block
// takes its row (one undo step restores the paragraph). Non-empty, non-
// paragraph, and ink-bearing anchors are never consumed; failed inserts
// leave the anchor (and the undo stack) untouched.
static void testConsumeEmptyAnchor() {
    qInfo("[17] empty-anchor consume: replace-not-stack, one undo, guards");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("above"));
    m.insertBlock(1);                                    // the empty anchor
    m.insertBlock(2); m.setContent(2, QStringLiteral("below"));
    CHECK(m.rowCountQml() == 3, "fixture: above / (empty) / below");

    const int tr = m.insertTable(1, 2, 2);
    CHECK(tr == 1 && m.rowCountQml() == 3 && m.typeForRow(1) == BlockModel::Table,
          "table CONSUMED the empty anchor (row %d, count %d)", tr, m.rowCountQml());
    CHECK(m.contentForRow(2) == QStringLiteral("below"), "below block undisturbed");
    m.undo();
    CHECK(m.rowCountQml() == 3 && m.typeForRow(1) == BlockModel::Paragraph
              && m.contentForRow(1).isEmpty(),
          "ONE undo restores the empty paragraph");
    m.redo();
    CHECK(m.typeForRow(1) == BlockModel::Table, "redo re-consumes");
    m.undo();

    // Sketch + divider consume too (returned row = the anchor's row).
    CHECK(m.insertSketch(1) == 1 && m.mediaKind(1) == QLatin1String("sketch"),
          "sketch consumes the empty anchor");
    m.undo();
    CHECK(m.insertDivider(1) == 1 && m.typeForRow(1) == BlockModel::Divider,
          "divider consumes the empty anchor");
    m.undo();

    // Media consumes; multi-insert chains off the returned rows in order.
    const QString probe2 = QDir::temp().filePath(QStringLiteral("mn_consume_probe.png"));
    { QImage pr(32, 24, QImage::Format_RGB32); pr.fill(Qt::gray); pr.save(probe2, "PNG"); }
    const int m1 = m.insertMediaFromUrl(1, QUrl::fromLocalFile(probe2).toString());
    CHECK(m1 == 1 && m.typeForRow(1) == BlockModel::Media && m.rowCountQml() == 3,
          "media consumes the empty anchor");
    const int m2 = m.insertMediaFromUrl(m1, QUrl::fromLocalFile(probe2).toString());
    CHECK(m2 == 2 && m.rowCountQml() == 4
              && m.typeForRow(2) == BlockModel::Media
              && m.contentForRow(3) == QStringLiteral("below"),
          "second media chains AFTER the first (no consume of a media anchor)");
    m.undo(); m.undo();
    CHECK(m.rowCountQml() == 3 && m.contentForRow(1).isEmpty(),
          "two undos restore the empty paragraph");

    // Guards: non-empty, non-paragraph, and inked anchors are NOT consumed.
    m.setContent(1, QStringLiteral("text"));
    CHECK(m.insertTable(1, 2, 2) == 2 && m.rowCountQml() == 4,
          "non-empty anchor: inserts BELOW");
    m.undo();
    m.setContent(1, QString());
    m.setHeading(1, 2);                                  // empty HEADING
    CHECK(m.insertDivider(1) == 2 && m.rowCountQml() == 4,
          "empty heading anchor: not consumed");
    m.undo();
    m.setHeading(1, 0);
    m.setBlockInk(1, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"block-local\",\"space\":\"px\","
        "\"shapes\":[{\"id\":\"s1\",\"type\":\"line\",\"color\":[1,0,0,1],"
        "\"stroke_width\":4,\"filled\":false,\"is_modeled\":false,"
        "\"points\":[[0,0],[10,10]]}]}"));
    CHECK(m.insertDivider(1) == 2 && m.rowCountQml() == 4,
          "ink-bearing empty anchor: not consumed (ink pins its block)");
    m.undo();
    m.setBlockInk(1, QString());

    // Failed insert: anchor intact, no undo entry burned.
    const int fr = m.insertMediaFromUrl(1, QStringLiteral("file:///nope/missing.zzz"));
    // (an unknown file lands as a generic attachment chip — force a REAL
    // failure with an empty path instead)
    if (fr >= 0) m.undo();
    const int fail = m.insertImageFromUrl(1, QString());
    CHECK(fail < 0 && m.rowCountQml() == 3 && m.contentForRow(1).isEmpty(),
          "failed insert leaves the empty anchor untouched");

    // Single-empty-block document: consuming may not strand a zero-block doc.
    BlockModel s;
    s.newDocument();
    while (s.rowCountQml() > 0) s.removeBlock(0);
    s.insertBlock(0);                                    // lone empty paragraph
    CHECK(s.insertTable(0, 2, 2) == 0 && s.rowCountQml() == 1
              && s.typeForRow(0) == BlockModel::Table,
          "lone empty block: table takes its place");
    s.undo();
    CHECK(s.rowCountQml() == 1 && s.typeForRow(0) == BlockModel::Paragraph,
          "undo restores the lone empty paragraph");
    QFile::remove(probe2);
}

// --- Test 18: insertSpecs — the shared importer sink persists depth/lang ---
// Extracting the pasteHtml tail into insertSpecs FIXED two latent drops:
// list depth and code lang were never written to the DB (appendBlock got
// literal 0 / no lang). Drive the sink directly and prove a reload keeps them.
static void testInsertSpecs() {
    qInfo("[18] insertSpecs: depth + lang persist through save/reopen");
    const QString path = QDir::tempPath() + QStringLiteral("/mn_regression_specs.mndb");
    QFile::remove(path);

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);   // blank paragraph — the reuse row

    std::vector<BlockModel::BlockSpec> specs;
    {
        BlockModel::BlockSpec a; a.type = BlockModel::ListItem; a.text = QStringLiteral("parent");
        BlockModel::BlockSpec b; b.type = BlockModel::ListItem; b.text = QStringLiteral("child"); b.depth = 2;
        BlockModel::BlockSpec c; c.type = BlockModel::Code; c.lang = QStringLiteral("cpp");
        c.text = QStringLiteral("int x = 1;\nint y = 2;");
        BlockModel::BlockSpec d; d.type = BlockModel::OrderedListItem; d.text = QStringLiteral("numbered"); d.depth = 1;
        specs = { a, b, c, d };
    }
    const auto [cr, cc] = m.insertSpecs(0, specs);
    CHECK(cr == 3 && m.rowCountQml() == 4, "reuse folded spec 0 into the blank row (caret %d)", cr);
    CHECK(m.typeForRow(1) == BlockModel::ListItem && m.depthForRow(1) == 2,
          "nested list depth held in-memory");
    CHECK(m.typeForRow(2) == BlockModel::Code
              && m.languageForRow(2) == QStringLiteral("cpp"),
          "code lang held in-memory");
    CHECK(m.typeForRow(3) == BlockModel::OrderedListItem && m.depthForRow(3) == 1,
          "ordered item + depth held in-memory");

    CHECK(m.saveAs(path), "saveAs() succeeded");
    m.closeDocument();
    BlockModel m2;
    CHECK(m2.openDocument(path), "reopen succeeded");
    CHECK(m2.depthForRow(1) == 2, "list depth SURVIVES reload (the fixed drop)");
    CHECK(m2.languageForRow(2) == QStringLiteral("cpp"),
          "code lang SURVIVES reload (the fixed drop)");
    CHECK(m2.typeForRow(3) == BlockModel::OrderedListItem && m2.depthForRow(3) == 1,
          "ordered depth survives reload");
    QFile::remove(path);
}

static void testImporterWalker() {
    qInfo("[19] Importer walker: HTML + Markdown reader conventions decode");
    using Spec = BlockModel::BlockSpec;

    // --- HTML reader ---------------------------------------------------------
    {
        QTextDocument d;
        d.setHtml(QStringLiteral(
            "<ol><li>one</li><li>two</li></ol>"
            "<ul><li>a<ul><li>b</li></ul></li></ul>"
            "<blockquote><p>quoted text</p></blockquote>"
            "<pre>line1\nline2</pre>"
            "<hr>"
            "<p><span style=\"color:#ff0000\">red</span> and "
            "<span style=\"background-color:#ffff00\">hi</span></p>"
            "<p style=\"color:#000000\">explicit black</p>"
            "<table><thead><tr><th>H</th></tr></thead>"
            "<tbody><tr><td>c</td></tr></tbody></table>"));
        const std::vector<Spec> s = Importer::specsFromTextDocument(d, nullptr);
        CHECK(s.size() == 10, "HTML fixture → 10 specs (got %d)", int(s.size()));
        if (s.size() == 10) {
            CHECK(s[0].type == BlockModel::OrderedListItem && s[0].depth == 0
                      && s[1].type == BlockModel::OrderedListItem,
                  "<ol> items → OrderedListItem");
            CHECK(s[2].type == BlockModel::ListItem && s[2].depth == 0
                      && s[3].type == BlockModel::ListItem && s[3].depth == 1,
                  "nested <ul> → depth 0 / 1");
            CHECK(s[4].type == BlockModel::Quote && s[4].text == QStringLiteral("quoted text"),
                  "<blockquote> → Quote");
            CHECK(s[5].type == BlockModel::Code && s[5].lang.isEmpty()
                      && s[5].text == QStringLiteral("line1\nline2"),
                  "<pre> lines coalesce into ONE Code block");
            CHECK(s[6].type == BlockModel::Divider, "<hr> → Divider");
            bool fg = false, bg = false;
            for (const auto& x : s[7].spans) {
                if (x.kind == BlockModel::SpanFgColor && x.href == QStringLiteral("#ff0000")) fg = true;
                if (x.kind == BlockModel::SpanHighlight && x.href == QStringLiteral("#ffff00")) bg = true;
            }
            CHECK(s[7].type == BlockModel::Paragraph && fg && bg,
                  "explicit color/background-color → SpanFgColor + SpanHighlight");
            bool anyColor = false;
            for (const auto& x : s[8].spans) anyColor |= (x.kind == BlockModel::SpanFgColor);
            CHECK(!anyColor, "explicit BLACK fg is skipped (browser default ink)");
            CHECK(s[9].type == BlockModel::Table
                      && TableGrid::fromJson(s[9].tableJson).headerRows() == 1
                      && TableGrid::fromJson(s[9].tableJson).cellText(1, 0) == QStringLiteral("c"),
                  "<thead> → headerRows 1, body cell intact");
        }
    }

    // --- Markdown reader (GitHub dialect) ------------------------------------
    {
        QTextDocument d;
        d.setMarkdown(QStringLiteral(
                          "1. one\n2. two\n\n"
                          "- a\n  - b\n    - c\n\n"
                          "> a quote\n\n"
                          "```cpp\nint x;\n\nint y;\n```\n\n"
                          "---\n\n"
                          "- [ ] todo\n- [x] done\n\n"
                          "para one\nsame para hard-wrapped\n"),
                      QTextDocument::MarkdownDialectGitHub);
        const std::vector<Spec> s = Importer::specsFromTextDocument(d, nullptr);
        CHECK(s.size() == 11, "MD fixture → 11 specs (got %d)", int(s.size()));
        if (s.size() == 11) {
            CHECK(s[0].type == BlockModel::OrderedListItem
                      && s[1].type == BlockModel::OrderedListItem,
                  "md ordered list → OrderedListItem");
            CHECK(s[2].depth == 0 && s[3].depth == 1 && s[4].depth == 2
                      && s[4].type == BlockModel::ListItem,
                  "md nested bullets → depth 0/1/2");
            CHECK(s[5].type == BlockModel::Quote, "md > quote → Quote");
            CHECK(s[6].type == BlockModel::Code && s[6].lang == QStringLiteral("cpp")
                      && s[6].text == QStringLiteral("int x;\n\nint y;"),
                  "fence → ONE Code block, lang carried, BLANK LINE inside survives");
            CHECK(s[7].type == BlockModel::Divider, "md --- → Divider");
            CHECK(s[8].type == BlockModel::TaskListItem && s[8].taskState == BlockModel::TaskTodo
                      && s[9].type == BlockModel::TaskListItem && s[9].taskState == BlockModel::TaskDone,
                  "GFM task markers → todo/done");
            CHECK(s[10].type == BlockModel::Paragraph
                      && s[10].text == QStringLiteral("para one same para hard-wrapped"),
                  "hard-wrapped paragraph JOINS; leaked list marker does NOT make it a task");
        }
    }

    // --- pasteHtml still routes through the walker ---------------------------
    {
        BlockModel m;
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        m.pasteHtml(0, 0, QStringLiteral(
            "<ol><li>first</li></ol><p><span style=\"color:#00ff00\">green</span></p>"));
        CHECK(m.rowCountQml() == 2 && m.typeForRow(0) == BlockModel::OrderedListItem,
              "pasteHtml delegation: ordered item landed");
        CHECK(m.hasFormat(1, 0, 5, QStringLiteral("color")),
              "pasteHtml delegation: color span landed");
        m.closeDocument();
    }
}

static void testImportFileCores() {
    qInfo("[20] Importer file cores: md / txt / csv / html land in a fresh doc");
    QDir dir(QDir::temp().filePath(QStringLiteral("mn_import_fixtures")));
    dir.removeRecursively();
    QDir::temp().mkpath(QStringLiteral("mn_import_fixtures"));
    auto writeFixture = [&](const QString& name, const QByteArray& bytes) {
        QFile f(dir.filePath(name));
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        return dir.filePath(name);
    };
    auto freshModel = [](BlockModel& m) {
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);   // the fresh-tab shape: one blank paragraph
    };

    Importer imp;
    CHECK(imp.formatFor(QStringLiteral("file:///x/Notes%20File.MD")) == QStringLiteral("md")
              && imp.formatFor(QStringLiteral("a.htm")) == QStringLiteral("html")
              && imp.formatFor(QStringLiteral("a.mndb")).isEmpty()
              && imp.formatFor(QStringLiteral("a")).isEmpty(),
          "formatFor: case-folded extensions classify; unknown → \"\"");

    // --- Markdown (incl. the tri-state sentinel round-trip) ------------------
    {
        const QString p = writeFixture(QStringLiteral("doc.md"),
            "# Title\n\n"
            "- [ ] todo\n- [/] doing **now**\n- [x] done\n\n"
            "```js\nlet a = 1;\n```\n");
        BlockModel m; freshModel(m);
        imp.setModel(&m);
        CHECK(imp.importFile(QUrl::fromLocalFile(p).toString()), "md import succeeded");
        CHECK(m.rowCountQml() == 5 && m.typeForRow(0) == BlockModel::Heading,
              "md: blank row consumed, heading first (%d rows)", m.rowCountQml());
        CHECK(m.taskStateForRow(1) == BlockModel::TaskTodo
                  && m.taskStateForRow(2) == BlockModel::TaskDoing
                  && m.taskStateForRow(3) == BlockModel::TaskDone,
              "md: tri-state `- [/] ` survives the GFM reader via the sentinel");
        CHECK(m.contentForRow(2) == QStringLiteral("doing now")
                  && m.hasFormat(2, 6, 9, QStringLiteral("bold")),
              "md: sentinel stripped, span offsets shifted with it");
        CHECK(m.typeForRow(4) == BlockModel::Code
                  && m.languageForRow(4) == QStringLiteral("js"),
              "md: fence lang carried");
        m.closeDocument();
    }

    // --- Plain text (smart-prefix rules) -------------------------------------
    {
        const QString p = writeFixture(QStringLiteral("doc.txt"),
                                       "# heading line\nplain line\n");
        BlockModel m; freshModel(m);
        imp.setModel(&m);
        CHECK(imp.importFile(p), "txt import succeeded");
        CHECK(m.rowCountQml() == 2 && m.typeForRow(0) == BlockModel::Heading
                  && m.typeForRow(1) == BlockModel::Paragraph,
              "txt: pasteText smart prefixes applied");
        m.closeDocument();
    }

    // --- CSV (quoted fields, embedded comma + newline) -----------------------
    {
        const QString p = writeFixture(QStringLiteral("data.csv"),
            "name,note\n\"Doe, Jane\",\"line1\nline2\"\nplain,cell\n");
        BlockModel m; freshModel(m);
        imp.setModel(&m);
        CHECK(imp.importFile(p), "csv import succeeded");
        CHECK(m.rowCountQml() == 1 && m.typeForRow(0) == BlockModel::Table,
              "csv: one Table block, blank row consumed");
        CHECK(m.tableCell(0, 1, 0) == QStringLiteral("Doe, Jane")
                  && m.tableCell(0, 1, 1) == QStringLiteral("line1\nline2")
                  && m.tableCell(0, 2, 1) == QStringLiteral("cell"),
              "csv: RFC-4180 quoting held (comma + newline in cells)");
        m.closeDocument();
    }

    // --- HTML file (relative image resolves against the file's dir) ----------
    {
        { QImage probe(10, 8, QImage::Format_RGB32); probe.fill(Qt::red);
          probe.save(dir.filePath(QStringLiteral("pic.png")), "PNG"); }
        const QString p = writeFixture(QStringLiteral("page.html"),
            "<h2>Hi</h2><p>before</p><img src=\"pic.png\"><p>after</p>");
        BlockModel m; freshModel(m);
        imp.setModel(&m);
        CHECK(imp.importFile(p), "html import succeeded");
        CHECK(m.rowCountQml() == 4 && m.typeForRow(0) == BlockModel::Heading,
              "html: heading + paragraphs landed (%d rows)", m.rowCountQml());
        CHECK(m.typeForRow(2) == BlockModel::Media,
              "html: RELATIVE img src resolved against the file's directory");
        m.closeDocument();
    }

    dir.removeRecursively();
}

static void testPackageFormat() {
    qInfo("[21] mnpkg archive layer: round-trip, entry methods, zip-slip, manifest");
    QDir dir(QDir::temp().filePath(QStringLiteral("mn_pkg_test")));
    dir.removeRecursively();
    QDir::temp().mkpath(QStringLiteral("mn_pkg_test"));

    // Incompressible-ish fixture standing in for media bytes.
    QByteArray mediaBytes;
    for (int i = 0; i < 4096; ++i) mediaBytes += char((i * 37 + i / 7) & 0xff);
    const QString mediaPath = dir.filePath(QStringLiteral("media.bin"));
    { QFile f(mediaPath); if (f.open(QIODevice::WriteOnly)) f.write(mediaBytes); }

    const QString zipPath = dir.filePath(QStringLiteral("pkg.mnpkg"));
    {
        mnpkg::PackageWriter w(zipPath);
        CHECK(w.ok(), "writer opened");
        CHECK(w.addCompressed(QLatin1String(mnpkg::kDbEntry), QByteArray(2000, 'a')),
              "db entry added (deflate)");
        CHECK(w.addStoredFile(QStringLiteral("media/media.bin"), mediaPath),
              "media entry added (store)");
        const QJsonObject man = mnpkg::makeManifest(1, mediaBytes.size());
        CHECK(w.addCompressed(QLatin1String(mnpkg::kManifestEntry),
                              QJsonDocument(man).toJson(QJsonDocument::Compact)),
              "manifest added");
        CHECK(w.bytesAdded() > 4096, "bytes counter runs");
        CHECK(w.finish(), "finish clean");
    }

    CHECK(mnpkg::isPackagePath(QStringLiteral("file:///x/Doc.MnPkg"))
              && !mnpkg::isPackagePath(QStringLiteral("/x/doc.mndb")),
          "isPackagePath: extension classifier (case-folded, URL-tolerant)");

    const QJsonObject man = mnpkg::readManifest(zipPath);
    CHECK(man.value(QStringLiteral("formatVersion")).toInt() == mnpkg::kFormatVersion
              && man.value(QStringLiteral("mediaCount")).toInt() == 1,
          "manifest round-trips");

    // Entry compression methods, from the raw zip local headers (PK\3\4 …
    // method = LE u16 at +8, name at +30): media must be STORED (0), the db
    // DEFLATE (8) — the disk-copy-speed repack contract.
    {
        QFile f(zipPath);
        CHECK(f.open(QIODevice::ReadOnly), "zip readable raw");
        const QByteArray z = f.readAll();
        int dbMethod = -1, mediaMethod = -1;
        for (int i = 0; i + 30 <= z.size();) {
            if (!(z[i] == 'P' && z[i+1] == 'K' && z[i+2] == 3 && z[i+3] == 4)) { ++i; continue; }
            const int method = quint8(z[i+8]) | (quint8(z[i+9]) << 8);
            const int nameLen = quint8(z[i+26]) | (quint8(z[i+27]) << 8);
            const QByteArray name = z.mid(i + 30, nameLen);
            if (name == mnpkg::kDbEntry) dbMethod = method;
            if (name == "media/media.bin") mediaMethod = method;
            i += 30 + nameLen;
        }
        CHECK(mediaMethod == 0, "media entry STORED (method %d)", mediaMethod);
        CHECK(dbMethod == 8, "db entry DEFLATEd (method %d)", dbMethod);
    }

    // Extraction round-trip (byte-exact).
    const QString out = dir.filePath(QStringLiteral("out"));
    CHECK(mnpkg::extractArchive(zipPath, out), "extractArchive succeeded");
    {
        QFile m(out + QStringLiteral("/media/media.bin"));
        QFile d(out + QStringLiteral("/document.mndb"));
        CHECK(m.open(QIODevice::ReadOnly) && m.readAll() == mediaBytes,
              "stored media byte-exact after extract");
        CHECK(d.open(QIODevice::ReadOnly) && d.readAll() == QByteArray(2000, 'a'),
              "deflated db byte-exact after extract");
    }

    // Zip-slip: a hostile `../` entry fails the WHOLE extraction, and nothing
    // lands outside the destination.
    const QString evilZip = dir.filePath(QStringLiteral("evil.zip"));
    {
        QZipWriter z(evilZip);
        z.addFile(QStringLiteral("ok.txt"), QByteArray("fine"));
        z.addFile(QStringLiteral("../escape.txt"), QByteArray("evil"));
        z.close();
    }
    const QString slipDir = dir.filePath(QStringLiteral("slip"));
    CHECK(!mnpkg::extractArchive(evilZip, slipDir), "zip-slip archive rejected");
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("escape.txt")))
              && !QFileInfo::exists(slipDir + QStringLiteral("/ok.txt"))
              && !QFileInfo::exists(slipDir),
          "traversal left NOTHING behind (dest cleaned, nothing outside)");

    // The sidecar convention survives packaging: `.qcview` is a MID-path dot
    // component, which QZipReader leaves alone (only LEADING dots mangle).
    const QString scZip = dir.filePath(QStringLiteral("sidecar.zip"));
    {
        mnpkg::PackageWriter w(scZip);
        w.addStoredFile(QStringLiteral("media/.qcview/media.bin/notes.json"), mediaPath);
        CHECK(w.finish(), "sidecar-path package wrote");
    }
    const QString scOut = dir.filePath(QStringLiteral("sc-out"));
    CHECK(mnpkg::extractArchive(scZip, scOut)
              && QFileInfo::exists(scOut + QStringLiteral("/media/.qcview/media.bin/notes.json")),
          "mid-path .qcview sidecar dir round-trips verbatim");

    dir.removeRecursively();
}

static void testPackageExporter() {
    qInfo("[22] packer: descriptor-walk plan, rewrite-on-copy, sidecar carry");
    // The packer must keep rewriting ABSOLUTE-src descriptors (network-share
    // refs, legacy docs). importFile can no longer author those from local
    // fixtures — every local source copies into .minnotes since the
    // paste-copy ruling (2026-08-19) — so this test hand-builds them below,
    // like the video descriptor always was.
    QDir dir(QCoreApplication::applicationDirPath()
             + QStringLiteral("/mn_pack_src"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    QDir().mkpath(dir.filePath(QStringLiteral("b")));
    QDir().mkpath(dir.filePath(QStringLiteral("c")));

    auto writePng = [&](const QString& rel, QColor color) {
        QImage img(12, 10, QImage::Format_RGB32);
        img.fill(color);
        img.save(dir.filePath(rel), "PNG");
        return dir.filePath(rel);
    };
    const QString picB = writePng(QStringLiteral("b/pic.png"), Qt::red);
    const QString picC = writePng(QStringLiteral("c/pic.png"), Qt::blue);
    // A junk "video" + its QCView sidecar tree (content is never probed by
    // the packer — the descriptor is hand-built below).
    const QString clip = dir.filePath(QStringLiteral("clip.mp4"));
    { QFile f(clip); if (f.open(QIODevice::WriteOnly)) f.write(QByteArray(512, 'V')); }
    QDir().mkpath(dir.filePath(QStringLiteral(".qcview/clip.mp4/images")));
    { QFile f(dir.filePath(QStringLiteral(".qcview/clip.mp4/notes.json")));
      if (f.open(QIODevice::WriteOnly)) f.write("{\"notes\":[]}"); }
    { QFile f(dir.filePath(QStringLiteral(".qcview/clip.mp4/images/note_00.png")));
      if (f.open(QIODevice::WriteOnly)) f.write(QByteArray(64, 'N')); }

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    m.setContent(0, QStringLiteral("hello"));
    const auto absImageJson = [](const QString& p) {
        return QStringLiteral("{\"src\":\"%1\",\"w\":12,\"h\":10}").arg(p);
    };
    const auto absImageSpec = [&](const QString& p) {
        BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
        sp.mediaJson = absImageJson(p);
        return sp;
    };
    m.insertSpecs(0, {absImageSpec(picB)}, false);
    m.insertSpecs(1, {absImageSpec(picC)}, false);
    const int img1 = 1, img2 = 2;
    CHECK(m.mediaKind(img1) == QLatin1String("image")
              && m.mediaKind(img2) == QLatin1String("image"),
          "fixture images inserted (abs-src descriptors)");
    const int tRow = m.insertTable(img2, 2, 2);
    CHECK(tRow > 0, "table inserted");
    m.tableSetCellMedia(tRow, 1, 0, absImageJson(picB));
    CHECK(m.tableCellMedia(tRow, 1, 0).contains(QStringLiteral("pic.png")),
          "table cell abs-src descriptor set");
    // Hand-built video descriptor (absolute src — the referenced-in-place shape).
    {
        BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
        sp.mediaJson = QStringLiteral(
            "{\"src\":\"%1\",\"w\":320,\"h\":240,\"kind\":\"video\","
            "\"durMs\":1000,\"frames\":24,\"fps\":24}").arg(clip);
        m.insertSpecs(tRow, {sp}, false);
    }
    const int vRow = tRow + 1;
    CHECK(m.mediaKind(vRow) == QStringLiteral("video"), "video row landed");

    // Plan: videos detected; excluded by default option…
    const auto planNoVid = PackageExporter::buildPackPlan(&m, /*includeVideos*/false);
    CHECK(planNoVid.videoCount == 1 && planNoVid.excludedVideos == 1,
          "plan counts the video, excludes it without the option");
    bool clipPlanned = false;
    for (const auto& it : planNoVid.items) clipPlanned |= it.srcPath == clip;
    CHECK(!clipPlanned, "excluded video not in the pack items");
    // …and included with it, sidecar discovered, same-name images deduped.
    const auto plan = PackageExporter::buildPackPlan(&m, /*includeVideos*/true);
    QString clipSidecar; QSet<QString> names;
    for (const auto& it : plan.items) {
        names.insert(it.packedName);
        if (it.srcPath == clip) clipSidecar = it.sidecarDir;
    }
    CHECK(!clipSidecar.isEmpty(), "video sidecar dir discovered by layout");
    CHECK(names.contains(QStringLiteral("pic.png")) && names.contains(QStringLiteral("pic-2.png")),
          "basename collision → unique -2 suffix");

    // Pack. The LIVE document's descriptors must be untouched.
    const QString before = m.contentForRow(img1);
    const QString pkg = QDir::temp().filePath(QStringLiteral("mn_pack_out.mnpkg"));
    QFile::remove(pkg);
    QString err;
    CHECK(PackageExporter::packDocument(&m, pkg, /*includeVideos*/true, &err),
          "packDocument succeeded (%s)", qPrintable(err));
    CHECK(m.contentForRow(img1) == before, "live descriptors untouched by packing");
    m.closeDocument();

    // Open path simulation: extract, media/ → .minnotes/, open the db.
    const QString ext = QDir::temp().filePath(QStringLiteral("mn_pack_ext"));
    QDir(ext).removeRecursively();
    CHECK(mnpkg::extractArchive(pkg, ext), "package extracts");
    CHECK(QDir(ext).rename(QStringLiteral("media"), QStringLiteral(".minnotes")),
          "media/ renamed to .minnotes/");
    CHECK(QFileInfo::exists(ext + QStringLiteral("/.minnotes/.qcview/clip.mp4/notes.json"))
              && QFileInfo::exists(ext + QStringLiteral("/.minnotes/.qcview/clip.mp4/images/note_00.png")),
          "sidecar tree re-associates by layout beside the packed video");

    BlockModel m2;
    CHECK(m2.openDocument(ext + QStringLiteral("/document.mndb")), "packed db opens");
    int mediaRows = 0; QSet<QString> resolved;
    for (int r = 0; r < m2.rowCountQml(); ++r) {
        if (m2.typeForRow(r) != BlockModel::Media) continue;
        ++mediaRows;
        const QString p = m2.mediaLocalPath(r);
        CHECK(!p.isEmpty() && QFileInfo::exists(p)
                  && p.startsWith(ext + QStringLiteral("/.minnotes/")),
              "media row %d resolves INSIDE the package dir", r);
        resolved.insert(QFileInfo(p).fileName());
    }
    CHECK(mediaRows == 3 && resolved.contains(QStringLiteral("clip.mp4")),
          "all three media rows resolve (imgs + video)");
    // Byte-exact through the STORE path, collisions kept distinct.
    {
        QFile o(picB), p(ext + QStringLiteral("/.minnotes/pic.png"));
        QFile o2(picC), p2(ext + QStringLiteral("/.minnotes/pic-2.png"));
        CHECK(o.open(QIODevice::ReadOnly) && p.open(QIODevice::ReadOnly)
                  && o.readAll() == p.readAll(),
              "pic.png byte-exact in the package");
        CHECK(o2.open(QIODevice::ReadOnly) && p2.open(QIODevice::ReadOnly)
                  && o2.readAll() == p2.readAll(),
              "collision copy pic-2.png byte-exact");
    }
    // Table cell media rewrote to the packaged copy.
    {
        const QString desc = m2.tableCellMedia(tRow, 1, 0);
        CHECK(desc.contains(QStringLiteral(".minnotes/pic")),
              "table cell descriptor rewrote to the packaged src");
    }
    m2.closeDocument();

    QFile::remove(pkg);
    QDir(ext).removeRecursively();
    dir.removeRecursively();
}

static void testPackageLifecycle() {
    qInfo("[23] .mnpkg lifecycle: sealed snapshot — lazy view, Save As materializes");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_pkglife"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    { QImage img(8, 8, QImage::Format_RGB32); img.fill(Qt::green);
      img.save(dir.filePath(QStringLiteral("pic.png")), "PNG"); }

    // Author a doc + pack it.
    const QString pkg = dir.filePath(QStringLiteral("doc.mnpkg"));
    {
        BlockModel m;
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        m.setContent(0, QStringLiteral("original"));
        CHECK(m.insertImageFromUrl(0,
                  QUrl::fromLocalFile(dir.filePath(QStringLiteral("pic.png"))).toString()) == 1,
              "fixture image inserted");
        QString err;
        CHECK(PackageExporter::packDocument(&m, pkg, true, &err),
              "fixture package packed (%s)", qPrintable(err));
        m.closeDocument();
    }

    const auto pkgScratchCount = [] {
        return QDir(BlockModel::scratchDir())
            .entryList({QStringLiteral("pkg-*")}, QDir::Dirs).size();
    };
    const auto beforeOpen = pkgScratchCount();

    // Open the package LIVE, edit, save (repack), reopen.
    {
        BlockModel m;
        CHECK(m.openDocument(pkg), "package opens as a live document");
        CHECK(m.documentName() == QStringLiteral("doc"), "documentName from the .mnpkg");
        CHECK(pkgScratchCount() == beforeOpen + 1, "extraction dir staged in scratch");
        // LAZY open: only the db is extracted up front; media stays in the
        // archive until something resolves it.
        QString pkgScratch;
        for (const QString& d : QDir(BlockModel::scratchDir())
                 .entryList({QStringLiteral("pkg-*")}, QDir::Dirs))
            pkgScratch = BlockModel::scratchDir() + QLatin1Char('/') + d;
        CHECK(QFileInfo::exists(pkgScratch + QStringLiteral("/document.mndb"))
                  && !QFileInfo::exists(pkgScratch + QStringLiteral("/.minnotes/pic.png")),
              "lazy open: db extracted, media NOT yet");
        const QString mp = m.mediaLocalPath(1);
        CHECK(!mp.isEmpty() && QFileInfo::exists(mp)
                  && mp.contains(QStringLiteral("/pkg-")),
              "first access extracts the media into the extraction dir");
        // SEALED SNAPSHOT: typing works, but save() refuses (untitled
        // semantics — Save As is the only way out) and the .mnpkg on disk
        // is never written.
        const qint64 pkgSize = QFileInfo(pkg).size();
        const QDateTime pkgMtime = QFileInfo(pkg).lastModified();
        m.setContent(0, QStringLiteral("edited in view"));
        CHECK(!m.save() && !m.overwriteSave(),
              "packages are snapshots: save/overwrite refuse");
        CHECK(QFileInfo(pkg).size() == pkgSize
                  && QFileInfo(pkg).lastModified() == pkgMtime,
              "the .mnpkg was NEVER written");
        m.closeDocument();
        CHECK(pkgScratchCount() == beforeOpen, "extraction dir cleaned on close");
    }
    {
        BlockModel m;
        CHECK(m.openDocument(pkg), "package reopens");
        CHECK(m.contentForRow(0) == QStringLiteral("original"),
              "view edits were DISCARDED (snapshot untouched)");
        CHECK(m.documentName() == QStringLiteral("doc"),
              "package view keeps the package's name (not 'Untitled')");

        // Save As → .mndb materializes the package (db + .minnotes sidecar),
        // media included even though nothing lazily extracted it first.
        m.setContent(0, QStringLiteral("my copy"));
        const QString mndb = dir.filePath(QStringLiteral("materialized.mndb"));
        CHECK(m.saveAs(mndb), "Save As .mndb from a package view");
        CHECK(m.documentName() == QStringLiteral("materialized"), "identity re-homed");
        CHECK(m.save(), "the materialized copy saves normally from now on");
        m.closeDocument();

        BlockModel m2;
        CHECK(m2.openDocument(mndb), "materialized .mndb opens");
        CHECK(m2.contentForRow(0) == QStringLiteral("my copy"),
              "content in the materialized doc");
        const QString mp2 = m2.mediaLocalPath(1);
        bool bytesOk = false;
        {
            QFile a(dir.filePath(QStringLiteral("pic.png"))), b(mp2);
            bytesOk = a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly)
                      && a.readAll() == b.readAll();
        }
        CHECK(!mp2.isEmpty() && bytesOk
                  && mp2.startsWith(dir.absolutePath() + QStringLiteral("/.minnotes/")),
              "media materialized beside the .mndb BYTE-EXACT (never lazily touched)");
        m2.closeDocument();
    }

    dir.removeRecursively();
}

static void testAsyncPackagePaths() {
    qInfo("[24] async package paths: non-blocking display resolve + worker export splice");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_pkgasync"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    { QImage img(8, 8, QImage::Format_RGB32); img.fill(Qt::magenta);
      img.save(dir.filePath(QStringLiteral("pic.png")), "PNG"); }

    const QString pkg = dir.filePath(QStringLiteral("doc.mnpkg"));
    {
        BlockModel m;
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        m.setContent(0, QStringLiteral("hello"));
        CHECK(m.insertImageFromUrl(0,
                  QUrl::fromLocalFile(dir.filePath(QStringLiteral("pic.png"))).toString()) == 1,
              "fixture image inserted");
        QString err;
        CHECK(PackageExporter::packDocument(&m, pkg, true, &err),
              "fixture packed (%s)", qPrintable(err));
        m.closeDocument();
    }

    // --- Non-blocking display resolve: "" + pending, then lands + reveals.
    {
        BlockModel m;
        CHECK(m.openDocument(pkg), "package view opens");
        const QString firstUrl = m.mediaUrl(1);
        CHECK(firstUrl.isEmpty() && m.mediaExtracting(1),
              "display URL is \"\" while the background extraction runs");
        QElapsedTimer t; t.start();
        while (m.mediaUrl(1).isEmpty() && t.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        CHECK(!m.mediaUrl(1).isEmpty() && !m.mediaExtracting(1),
              "background extraction landed; URL resolves");
        CHECK(QFileInfo::exists(QUrl(m.mediaUrl(1)).toLocalFile()),
              "extracted file exists on disk");
        m.closeDocument();
    }

    // --- Worker export from an untouched view: pure SPLICE (no extraction).
    {
        BlockModel m;
        CHECK(m.openDocument(pkg), "package view reopens");
        // mediaUrl NOT called — the media must go archive→archive.
        const QString pkg2 = dir.filePath(QStringLiteral("resend.mnpkg"));
        PackageExporter pe;
        pe.setModel(&m);
        bool done = false, okResult = false;
        QObject::connect(&pe, &PackageExporter::exportFinished, &pe,
                         [&](bool ok, const QString&) { done = true; okResult = ok; });
        pe.startExport(QUrl::fromLocalFile(pkg2).toString(), /*videos*/true);
        QElapsedTimer t; t.start();
        while (!done && t.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        CHECK(done && okResult, "async export finished ok");
        CHECK(!pe.running(), "running flag cleared");
        m.closeDocument();

        // The re-exported package carries the media even though this session
        // never extracted it.
        BlockModel m2;
        CHECK(m2.openDocument(pkg2), "re-exported package opens");
        const QString p = m2.mediaLocalPath(1);   // blocking pull
        bool bytesOk = false;
        {
            QFile a(dir.filePath(QStringLiteral("pic.png"))), b(p);
            bytesOk = a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly)
                      && a.readAll() == b.readAll();
        }
        CHECK(bytesOk, "media SPLICED archive→archive byte-exact (never extracted)");
        m2.closeDocument();
    }

    dir.removeRecursively();
}

static void testPackageStreaming() {
    qInfo("[25] streaming: packaged video plays from a byte range, no extraction");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_pkgstream"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());

    // Junk "video" + a QCView sidecar beside it.
    const QString clip = dir.filePath(QStringLiteral("clip.mp4"));
    QByteArray clipBytes;
    for (int i = 0; i < 100000; ++i) clipBytes += char((i * 131) & 0xff);
    { QFile f(clip); if (f.open(QIODevice::WriteOnly)) f.write(clipBytes); }
    QDir().mkpath(dir.filePath(QStringLiteral(".qcview/clip.mp4")));
    { QFile f(dir.filePath(QStringLiteral(".qcview/clip.mp4/notes.json")));
      if (f.open(QIODevice::WriteOnly)) f.write("{\"notes\":[]}"); }

    const QString pkg = dir.filePath(QStringLiteral("doc.mnpkg"));
    {
        BlockModel m;
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
        sp.mediaJson = QStringLiteral(
            "{\"src\":\"%1\",\"w\":320,\"h\":240,\"kind\":\"video\","
            "\"durMs\":1000,\"frames\":24,\"fps\":24}").arg(clip);
        m.insertSpecs(0, {sp}, false);
        QString err;
        CHECK(PackageExporter::packDocument(&m, pkg, true, &err),
              "fixture packed (%s)", qPrintable(err));
        m.closeDocument();
    }

    {
        BlockModel m;
        CHECK(m.openDocument(pkg), "package view opens");
        // Sidecar extracted EAGERLY; the video is NOT.
        const QString anchor = m.mediaAnchorPath(1);
        CHECK(!anchor.isEmpty() && !QFileInfo::exists(anchor),
              "anchor path computed; video NOT on disk");
        CHECK(QFileInfo::exists(QFileInfo(anchor).absolutePath()
                                + QStringLiteral("/.qcview/clip.mp4/notes.json")),
              "sidecar extracted eagerly beside the anchor");
        // Playback source = subfile spec whose byte range IS the video.
        const QString spec = m.mediaPlaybackSource(1);
        CHECK(spec.startsWith(QStringLiteral("subfile,,start,")),
              "playback source is a subfile spec (%s)", qPrintable(spec.left(40)));
        const QStringList parts = spec.split(QLatin1Char(','));
        const qint64 start = parts.value(3).toLongLong();
        const qint64 end = parts.value(5).toLongLong();
        QFile zf(pkg);
        QByteArray range;
        if (zf.open(QIODevice::ReadOnly) && zf.seek(start))
            range = zf.read(end - start);
        CHECK(range == clipBytes, "spec byte range is the video BYTE-EXACT");
        CHECK(!QFileInfo::exists(anchor),
              "resolving the playback source extracted NOTHING");
        m.closeDocument();
    }

    // Real decode through the spec (libav in-process): generate a genuine
    // mp4 with the vendored ffmpeg when present; skip quietly otherwise.
#if defined(Q_OS_WIN)
    const QString ffmpeg = QStringLiteral(MN_SOURCE_DIR "/external/ffmpeg/bin/ffmpeg.exe");
    const QString h264enc = QStringLiteral("libx264");
#else
    const QString ffmpeg = QStringLiteral(MN_SOURCE_DIR "/external/ffmpeg/bin/ffmpeg");
    const QString h264enc = QStringLiteral("h264_videotoolbox");
#endif
    if (QFileInfo::exists(ffmpeg)) {
        const QString real = dir.filePath(QStringLiteral("real.mp4"));
        QProcess p;
        p.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                         QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                         QStringLiteral("-i"), QStringLiteral("testsrc=size=320x240:rate=24"),
                         QStringLiteral("-t"), QStringLiteral("1"),
                         QStringLiteral("-c:v"), h264enc,
                         QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                         QStringLiteral("-y"), real});
        p.waitForFinished(20000);
        if (p.exitCode() == 0 && QFileInfo::exists(real)) {
            BlockModel m;
            m.newDocument();
            while (m.rowCountQml() > 0) m.removeBlock(0);
            m.insertBlock(0);
            BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
            sp.mediaJson = QStringLiteral(
                "{\"src\":\"%1\",\"w\":320,\"h\":240,\"kind\":\"video\","
                "\"durMs\":1000,\"frames\":24,\"fps\":24}").arg(real);
            m.insertSpecs(0, {sp}, false);
            const QString pkg2 = dir.filePath(QStringLiteral("real.mnpkg"));
            QString err;
            CHECK(PackageExporter::packDocument(&m, pkg2, true, &err), "real pkg packed");
            m.closeDocument();
            BlockModel m2;
            CHECK(m2.openDocument(pkg2), "real pkg opens");
            const QImage frame =
                MediaStore::extractFrame(m2.mediaPlaybackSource(1), 0, 64);
            CHECK(!frame.isNull() && frame.width() > 0,
                  "libav DECODED a frame straight out of the archive (%dx%d)",
                  frame.width(), frame.height());
            m2.closeDocument();
        } else {
            qInfo("  (real-decode leg skipped: ffmpeg testsrc encode unavailable)");
        }
    } else {
        qInfo("  (real-decode leg skipped: vendored ffmpeg absent)");
    }

    dir.removeRecursively();
}

static void testEnexImport() {
    qInfo("[26] ENEX import: notes → folder of docs (images, todos, attachments)");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_enex"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());

    // A real 4x4 png, base64'd, plus its MD5 (the en-media linkage key).
    QByteArray png;
    {
        QImage img(4, 4, QImage::Format_RGB32); img.fill(Qt::cyan);
        QBuffer b(&png); b.open(QIODevice::WriteOnly); img.save(&b, "PNG");
    }
    const QString pngMd5 = QString::fromLatin1(
        QCryptographicHash::hash(png, QCryptographicHash::Md5).toHex());
    const QByteArray attach("PDFISH-BYTES-123");
    const QString attachMd5 = QString::fromLatin1(
        QCryptographicHash::hash(attach, QCryptographicHash::Md5).toHex());

    const QString enexPath = dir.filePath(QStringLiteral("export.enex"));
    {
        QFile f(enexPath);
        CHECK(f.open(QIODevice::WriteOnly), "enex fixture writable");
        QString enex = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<en-export application=\"Evernote\">\n"
            "<note><title>Trip Plan</title>\n"
            "<content><![CDATA[<?xml version=\"1.0\"?>"
            "<!DOCTYPE en-note SYSTEM \"http://xml.evernote.com/pub/enml2.dtd\">"
            "<en-note><div>Pack the <b>camera</b></div>"
            "<div><en-todo checked=\"false\"/>buy tickets</div>"
            "<div><en-todo checked=\"true\"/>book hotel</div>"
            "<en-media hash=\"%1\" type=\"image/png\"/>"
            "<en-media hash=\"%2\" type=\"application/pdf\"/>"
            "</en-note>]]></content>\n"
            "<resource><data encoding=\"base64\">%3</data><mime>image/png</mime>"
            "<resource-attributes><file-name>view.png</file-name></resource-attributes></resource>\n"
            "<resource><data encoding=\"base64\">%4</data><mime>application/pdf</mime>"
            "<resource-attributes><file-name>itinerary.pdf</file-name></resource-attributes></resource>\n"
            "</note>\n"
            "<note><title>Trip Plan</title>"
            "<content><![CDATA[<en-note><div>second note, same title</div></en-note>]]></content></note>\n"
            "</en-export>\n")
            .arg(pngMd5, attachMd5,
                 QString::fromLatin1(png.toBase64()),
                 QString::fromLatin1(attach.toBase64()));
        f.write(enex.toUtf8());
    }

    const QString dest = dir.filePath(QStringLiteral("out"));
    QString firstPath;
    const int n = Importer::importEnexToFolder(enexPath, dest, &firstPath);
    CHECK(n == 2, "two notes → two docs (%d)", n);
    CHECK(QFileInfo::exists(dest + QStringLiteral("/Trip Plan.mndb"))
              && QFileInfo::exists(dest + QStringLiteral("/Trip Plan-2.mndb")),
          "title collision → -2 suffix");

    BlockModel m;
    CHECK(m.openDocument(firstPath), "first doc opens");
    CHECK(m.typeForRow(0) == BlockModel::Heading
              && m.contentForRow(0) == QStringLiteral("Trip Plan"),
          "note title → H1");
    CHECK(m.contentForRow(1) == QStringLiteral("Pack the camera")
              && m.hasFormat(1, 9, 15, QStringLiteral("bold")),
          "ENML div text + bold span");
    CHECK(m.typeForRow(2) == BlockModel::TaskListItem
              && m.taskStateForRow(2) == BlockModel::TaskTodo
              && m.contentForRow(2) == QStringLiteral("buy tickets"),
          "en-todo unchecked → task todo");
    CHECK(m.typeForRow(3) == BlockModel::TaskListItem
              && m.taskStateForRow(3) == BlockModel::TaskDone,
          "en-todo checked → task done");
    int imgRow = -1, fileRow = -1;
    for (int r = 0; r < m.rowCountQml(); ++r) {
        if (m.typeForRow(r) != BlockModel::Media) continue;
        if (m.mediaKind(r) == QStringLiteral("file")) fileRow = r;
        else imgRow = r;
    }
    CHECK(imgRow >= 0 && QFileInfo::exists(m.mediaLocalPath(imgRow)),
          "image resource inlined via hash, file in the doc's sidecar");
    CHECK(fileRow >= 0 && m.mediaFileName(fileRow) == QStringLiteral("itinerary.pdf"),
          "non-image resource → file chip with its original name");
    {
        QFile f(m.mediaLocalPath(fileRow));
        CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == attach,
              "attachment bytes byte-exact in the sidecar");
    }
    m.closeDocument();
    dir.removeRecursively();
}

static void testNotionImport() {
    qInfo("[27] Notion zip import: pages + database → folder of docs");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_notion"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());

    QByteArray png;
    {
        QImage img(6, 5, QImage::Format_RGB32); img.fill(Qt::yellow);
        QBuffer b(&png); b.open(QIODevice::WriteOnly); img.save(&b, "PNG");
    }
    const QString zipPath = dir.filePath(QStringLiteral("notion.zip"));
    {
        // Notion shape: "Title <32hex>.md" + an asset dir of the same name;
        // image links inside the md are %-ENCODED while entry names carry
        // literal spaces.
        const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");
        mnpkg::PackageWriter w(zipPath);
        w.addCompressed(QStringLiteral("Meeting Notes %1.md").arg(id),
                        QStringLiteral("# Meeting Notes\n\nAgenda item **one**\n\n"
                                       "![view](Meeting%20Notes%20""%1/view.png)\n").arg(id).toUtf8());
        const QString picTmp = dir.filePath(QStringLiteral("view.png"));
        { QFile f(picTmp); if (f.open(QIODevice::WriteOnly)) f.write(png); }
        w.addStoredFile(QStringLiteral("Meeting Notes %1/view.png").arg(id), picTmp);
        w.addCompressed(QStringLiteral("Tasks %1.csv").arg(id),
                        QByteArray("Name,Status\nShip it,Done\n"));
        CHECK(w.finish(), "notion fixture zip wrote");
    }

    CHECK(Importer::formatForPath(zipPath) == QStringLiteral("notion"),
          "zip with md/csv classifies as notion");
    CHECK(Importer::formatForPath(QStringLiteral("/x/random.zip")).isEmpty(),
          "unreadable/other zips do NOT classify");

    const QString dest = dir.filePath(QStringLiteral("out"));
    QString firstPath;
    const int n = Importer::importNotionZipToFolder(zipPath, dest, &firstPath);
    CHECK(n == 2, "page + database → two docs (%d)", n);
    CHECK(QFileInfo::exists(dest + QStringLiteral("/Meeting Notes.mndb"))
              && QFileInfo::exists(dest + QStringLiteral("/Tasks.mndb")),
          "32-hex Notion ids stripped from doc names");

    BlockModel m;
    CHECK(m.openDocument(dest + QStringLiteral("/Meeting Notes.mndb")), "page opens");
    CHECK(m.typeForRow(0) == BlockModel::Heading, "md heading landed");
    int imgRow = -1;
    for (int r = 0; r < m.rowCountQml(); ++r)
        if (m.typeForRow(r) == BlockModel::Media) imgRow = r;
    CHECK(imgRow >= 0, "image block landed");
    const QString p = m.mediaLocalPath(imgRow);
    CHECK(!p.isEmpty() && QFileInfo::exists(p)
              && p.startsWith(dest + QStringLiteral("/.minnotes/")),
          "%%-encoded relative image resolved AND copied into the doc sidecar");
    m.closeDocument();

    BlockModel m2;
    CHECK(m2.openDocument(dest + QStringLiteral("/Tasks.mndb")), "database opens");
    CHECK(m2.typeForRow(0) == BlockModel::Table
              && m2.tableCell(0, 1, 0) == QStringLiteral("Ship it"),
          "csv database → Table doc");
    m2.closeDocument();
    dir.removeRecursively();
}

static void testDocxRoundTrip() {
    qInfo("[28] DOCX round-trip: our export reads back structurally intact");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_docx"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    { QImage img(10, 8, QImage::Format_RGB32); img.fill(Qt::darkRed);
      img.save(dir.filePath(QStringLiteral("pic.png")), "PNG"); }

    const QString docx = dir.filePath(QStringLiteral("round.docx"));
    {
        BlockModel m;
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        std::vector<BlockModel::BlockSpec> specs;
        {
            BlockModel::BlockSpec h; h.type = BlockModel::Heading; h.level = 2;
            h.text = QStringLiteral("Section title"); specs.push_back(h);
            BlockModel::BlockSpec p; p.text = QStringLiteral("hello bold world");
            p.spans.push_back({6, 10, BlockModel::SpanBold, {}}); specs.push_back(p);
            BlockModel::BlockSpec l; l.type = BlockModel::ListItem; l.depth = 1;
            l.text = QStringLiteral("nested bullet"); specs.push_back(l);
            BlockModel::BlockSpec o; o.type = BlockModel::OrderedListItem;
            o.text = QStringLiteral("first item"); specs.push_back(o);
            BlockModel::BlockSpec tk; tk.type = BlockModel::TaskListItem;
            tk.taskState = BlockModel::TaskDoing;
            tk.text = QStringLiteral("doing task"); specs.push_back(tk);
            BlockModel::BlockSpec cd; cd.type = BlockModel::Code;
            cd.text = QStringLiteral("int x;\nint y;"); specs.push_back(cd);
            TableGrid g = TableGrid::makeEmpty(2, 2);
            g.setCellText(1, 0, QStringLiteral("cell A"));
            BlockModel::BlockSpec tb; tb.type = BlockModel::Table;
            tb.tableJson = g.toJson(); specs.push_back(tb);
        }
        m.insertSpecs(0, specs, true);
        const QString threadId = m.addComment(1, 0, 5);
        m.addCommentMessage(threadId, QStringLiteral("check this wording"));
        m.insertImageFromUrl(m.rowCountQml() - 1,
            QUrl::fromLocalFile(dir.filePath(QStringLiteral("pic.png"))).toString());

        Exporter ex;
        ex.setModel(&m);
        CHECK(ex.exportDocx(docx, false), "our DOCX exported");
        m.closeDocument();
    }

    BlockModel m2;
    m2.newDocument();
    while (m2.rowCountQml() > 0) m2.removeBlock(0);
    m2.insertBlock(0);
    CHECK(Importer::importDocxFile(docx, &m2), "docx imported");

    // The export leads with a document-title header (ruling 2026-08-20) —
    // anchor the structural checks on the section heading, not row 0.
    int base = -1;
    for (int r2 = 0; r2 < m2.rowCountQml(); ++r2)
        if (m2.contentForRow(r2) == QStringLiteral("Section title")) { base = r2; break; }
    CHECK(base >= 1, "title header precedes the content (base=%d)", base);
    CHECK(m2.typeForRow(base) == BlockModel::Heading && m2.levelForRow(base) == 2,
          "direct-formatted heading level survives (size heuristic)");
    CHECK(m2.contentForRow(base + 1) == QStringLiteral("hello bold world")
              && m2.hasFormat(base + 1, 6, 10, QStringLiteral("bold")),
          "paragraph + bold span round-trips");
    CHECK(m2.typeForRow(base + 2) == BlockModel::ListItem && m2.depthForRow(base + 2) == 1,
          "nested bullet + depth");
    CHECK(m2.typeForRow(base + 3) == BlockModel::OrderedListItem,
          "ordered item (numId 2 → ordered)");
    CHECK(m2.typeForRow(base + 4) == BlockModel::TaskListItem
              && m2.taskStateForRow(base + 4) == BlockModel::TaskDoing
              && m2.contentForRow(base + 4) == QStringLiteral("doing task"),
          "tri-state task via glyph sniff (DOING survives!)");
    int codeRow = -1, tableRow = -1, mediaRow = -1;
    for (int r2 = 0; r2 < m2.rowCountQml(); ++r2) {
        if (m2.typeForRow(r2) == BlockModel::Code) codeRow = r2;
        if (m2.typeForRow(r2) == BlockModel::Table && tableRow < 0) tableRow = r2;
        if (m2.typeForRow(r2) == BlockModel::Media) mediaRow = r2;
    }
    CHECK(codeRow >= 0 && m2.contentForRow(codeRow) == QStringLiteral("int x;\nint y;"),
          "code block (Courier+EFEFEF) coalesced back to one block");
    CHECK(tableRow >= 0 && m2.tableCell(tableRow, 1, 0) == QStringLiteral("cell A"),
          "table cell text");
    CHECK(mediaRow >= 0 && QFileInfo::exists(m2.mediaLocalPath(mediaRow)),
          "image re-imported into the sidecar");
    // Comment thread → native thread with the body preserved.
    bool commentOk = false;
    const QVariantList threads = m2.commentThreads();
    for (const QVariant& tv : threads) {
        const QVariantMap t = tv.toMap();
        const QVariantList msgs = m2.commentMessages(t.value(QStringLiteral("id")).toString());
        for (const QVariant& mv : msgs)
            if (mv.toMap().value(QStringLiteral("body")).toString()
                    .contains(QStringLiteral("check this wording")))
                commentOk = true;
    }
    CHECK(commentOk, "Word comment → native thread with body");
    m2.closeDocument();
    dir.removeRecursively();
}

static void testRtfImport() {
    qInfo("[29] RTF import (macOS native converter + class inliner)");
    if (!mn::rtfImportSupported()) {
        qInfo("  (skipped: RTF unsupported on this platform)");
        return;
    }
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_rtf"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    const QString rtf = dir.filePath(QStringLiteral("t.rtf"));
    {
        QFile f(rtf);
        CHECK(f.open(QIODevice::WriteOnly), "rtf fixture writable");
        f.write("{\\rtf1\\ansi\\deff0 {\\fonttbl{\\f0 Helvetica;}}\n"
                "{\\b bold} normal {\\i ital} {\\ul under}\\par\n"
                "second paragraph\\par\n}");
    }
    CHECK(Importer::formatForPath(rtf) == QStringLiteral("rtf"),
          "rtf classifies where supported");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    CHECK(Importer::importRtfFile(rtf, &m), "rtf imported");
    CHECK(m.contentForRow(0) == QStringLiteral("bold normal ital under"),
          "text flattened correctly ('%s')", qPrintable(m.contentForRow(0)));
    CHECK(m.hasFormat(0, 0, 4, QStringLiteral("bold")), "bold span");
    CHECK(m.hasFormat(0, 12, 16, QStringLiteral("italic")), "italic span");
    CHECK(m.hasFormat(0, 17, 22, QStringLiteral("underline")),
          "underline recovered via the Cocoa class INLINER");
    CHECK(m.contentForRow(1) == QStringLiteral("second paragraph"),
          "second paragraph (rows=%d row1='%s' type=%d)",
          m.rowCountQml(), qPrintable(m.contentForRow(1)), m.typeForRow(1));
    m.closeDocument();
    dir.removeRecursively();
}

// MN_OPEN_PROBE=<dir> — diagnostic, not a test: builds (once) a package with
// three junk multi-GB "videos" in <dir>, then times each stage of the open
// path. For chasing "opening a package freezes" reports.
static int runOpenProbe(const QString& base) {
    QDir().mkpath(base);
    const QString pkg = base + QStringLiteral("/big.mnpkg");
    if (!QFileInfo::exists(pkg)) {
        for (int i = 0; i < 3; ++i) {
            QFile f(base + QStringLiteral("/clip%1.mp4").arg(i));
            if (f.open(QIODevice::WriteOnly)) {
                const QByteArray chunk(1 << 20, char('A' + i));
                for (int k = 0; k < 700; ++k) f.write(chunk);   // ~700MB each
            }
        }
        BlockModel m;
        m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        m.setContent(0, QStringLiteral("big test"));
        int at = 0;
        for (int i = 0; i < 3; ++i) {
            BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
            sp.mediaJson = QStringLiteral(
                "{\"src\":\"%1/clip%2.mp4\",\"w\":1920,\"h\":1080,"
                "\"kind\":\"video\",\"durMs\":3600000,\"frames\":86400,\"fps\":24}")
                .arg(base).arg(i);
            at = m.insertSpecs(at, {sp}, false).first;
        }
        QString err;
        QElapsedTimer tp; tp.start();
        if (!PackageExporter::packDocument(&m, pkg, true, &err)) {
            qCritical() << "probe pack failed" << err;
            return 1;
        }
        qInfo() << "probe: packed in" << tp.elapsed() << "ms";
        m.closeDocument();
    }
    QElapsedTimer t; t.start();
    BlockModel m2;
    const bool ok = m2.openDocument(pkg);
    qInfo() << "probe: openDocument" << t.elapsed() << "ms ok" << ok
            << "rows" << m2.rowCountQml();
    t.restart();
    for (int r = 0; r < m2.rowCountQml(); ++r) {
        m2.mediaUrl(r); m2.mediaViewPath(r); m2.mediaKind(r);
        m2.mediaExtracting(r); m2.mediaFps(r); m2.mediaDurationMs(r);
    }
    qInfo() << "probe: all display resolves" << t.elapsed() << "ms";
    t.restart();
    m2.closeDocument();
    qInfo() << "probe: close" << t.elapsed() << "ms";
    return 0;
}

// --- Test 30: importFile copy policy — local sources copy into .minnotes ---
// The 2026-08-19 ruling (the capture-app temp-path rot): every LOCAL image
// source copies into the doc's .minnotes (content-addressed raw bytes); only
// network shares stay referenced in place. Network mounts can't be conjured
// in a test, so this covers the local side: copy happens, is byte-exact
// (no re-encode), and dedups.
static void testImportCopyPolicy() {
    qInfo("[30] importFile copy policy: local sources copy into .minnotes");
    QDir dir(QCoreApplication::applicationDirPath()
             + QStringLiteral("/mn_copy_policy"));   // stable-LOCAL, not temp
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    const QString src = dir.filePath(QStringLiteral("shot.png"));
    { QImage img(9, 7, QImage::Format_RGB32); img.fill(Qt::magenta);
      img.save(src, "PNG"); }

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    m.setContent(0, QStringLiteral("anchor"));   // non-empty, or the insert consumes row 0
    const int r = m.insertImageFromUrl(0, QUrl::fromLocalFile(src).toString());
    CHECK(r == 1, "local image inserted");
    const QString p = m.mediaLocalPath(r);
    CHECK(!p.isEmpty() && QFileInfo::exists(p)
              && p.contains(QStringLiteral("/.minnotes/"))
              && QFileInfo(p).absoluteFilePath() != QFileInfo(src).absoluteFilePath(),
          "stable-LOCAL source COPIED into .minnotes (not referenced)");
    {   // raw-byte copy: JPEG-stays-JPEG class guarantee
        QFile a(src), b(p);
        CHECK(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly)
                  && a.readAll() == b.readAll(),
              "copy is byte-exact (raw bytes, no re-encode)");
    }
    const int r2 = m.insertImageFromUrl(r, QUrl::fromLocalFile(src).toString());
    CHECK(r2 == r + 1 && m.mediaLocalPath(r2) == p,
          "re-import of the same file dedups to the same asset");
    m.closeDocument();
    dir.removeRecursively();
}

// --- Test 31: Collect Media — external sources copy in, descriptors follow ---
// The collector rides the packer's walker: external abs-src media (doc image,
// table cell, video + .qcview sidecar) copies into .minnotes with readable
// names, descriptors rewrite as ONE undo entry, and a second collect finds
// nothing left to do.
static void testCollectMedia() {
    qInfo("[31] Collect Media: copy external refs into .minnotes, one undo entry");
    QDir dir(QCoreApplication::applicationDirPath()
             + QStringLiteral("/mn_collect_src"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    const QString pic = dir.filePath(QStringLiteral("board.png"));
    { QImage img(12, 10, QImage::Format_RGB32); img.fill(Qt::yellow);
      img.save(pic, "PNG"); }
    const QString clip = dir.filePath(QStringLiteral("take.mp4"));
    { QFile f(clip); if (f.open(QIODevice::WriteOnly)) f.write(QByteArray(256, 'V')); }
    QDir().mkpath(dir.filePath(QStringLiteral(".qcview/take.mp4")));
    { QFile f(dir.filePath(QStringLiteral(".qcview/take.mp4/notes.json")));
      if (f.open(QIODevice::WriteOnly)) f.write("{\"notes\":[]}"); }

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    m.setContent(0, QStringLiteral("hello"));
    {   // abs-src fixtures (the referenced-in-place shape collect exists for)
        BlockModel::BlockSpec img; img.type = BlockModel::Media;
        img.mediaJson = QStringLiteral("{\"src\":\"%1\",\"w\":12,\"h\":10}").arg(pic);
        m.insertSpecs(0, {img}, false);
        BlockModel::BlockSpec vid; vid.type = BlockModel::Media;
        vid.mediaJson = QStringLiteral(
            "{\"src\":\"%1\",\"w\":320,\"h\":240,\"kind\":\"video\","
            "\"durMs\":1000,\"frames\":24,\"fps\":24}").arg(clip);
        m.insertSpecs(1, {vid}, false);
    }
    const int imgRow = 1, vidRow = 2;
    const int tRow = m.insertTable(vidRow, 2, 2);
    m.tableSetCellMedia(tRow, 0, 0,
        QStringLiteral("{\"src\":\"%1\",\"w\":12,\"h\":10}").arg(pic));
    CHECK(m.mediaKind(imgRow) == QLatin1String("image")
              && m.mediaKind(vidRow) == QLatin1String("video") && tRow > 0,
          "collect fixtures in place");

    int copied = 0; QString err;
    CHECK(MediaCollector::collectDocument(&m, /*includeVideos*/true, &copied, &err),
          "collect succeeded (%s)", qPrintable(err));
    CHECK(copied == 2, "two external files collected (image deduped by source)");
    // The doc's scratch .minnotes persists across suite runs, so a re-run
    // legitimately lands board-2.png etc. — assert the readable STEM, not an
    // exact basename.
    const QString root = QDir::cleanPath(m.mediaStore()->docDir());
    const QString ip = m.mediaLocalPath(imgRow);
    const QString vp = m.mediaLocalPath(vidRow);
    CHECK(ip.startsWith(root + QStringLiteral("/.minnotes/board"))
              && ip.endsWith(QStringLiteral(".png")) && QFileInfo::exists(ip),
          "image collected under a readable name (%s)", qPrintable(ip));
    CHECK(vp.startsWith(root + QStringLiteral("/.minnotes/take"))
              && vp.endsWith(QStringLiteral(".mp4")) && QFileInfo::exists(vp)
              && QFileInfo::exists(root + QStringLiteral("/.minnotes/.qcview/")
                                   + QFileInfo(vp).fileName()
                                   + QStringLiteral("/notes.json")),
          "video + .qcview sidecar collected (%s)", qPrintable(vp));
    CHECK(m.tableCellMedia(tRow, 0, 0).contains(
              QStringLiteral(".minnotes/") + QFileInfo(ip).fileName()),
          "table cell descriptor followed the copy");
    {   // byte-exact, no re-encode
        QFile a(pic), b(ip);
        CHECK(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly)
                  && a.readAll() == b.readAll(), "collected copy byte-exact");
    }
    // ONE undo entry restores every source; redo re-applies (copies stay).
    m.undo();
    CHECK(m.mediaLocalPath(imgRow) == QFileInfo(pic).absoluteFilePath()
              && m.tableCellMedia(tRow, 0, 0).contains(pic),
          "one undo restores all original refs");
    m.redo();
    CHECK(m.mediaLocalPath(imgRow) == ip, "redo re-applies the collected refs");
    // Idempotent: nothing external remains.
    copied = -1;
    CHECK(MediaCollector::collectDocument(&m, true, &copied, &err) && copied == 0,
          "second collect finds nothing to do");
    m.closeDocument();
    dir.removeRecursively();
}

// --- Test 32: DocInk multi-anchor group-move undo -------------------------
// The canvas commits a cross-anchor gesture (departures/arrivals resolved
// internally) as beginGroup + one setBlockInk per touched blob + endGroup —
// drive the model in exactly that shape and prove ONE entry restores ALL
// touched blobs.
static void testDocInkGroupMove() {
    qInfo("[32] doc-ink group move: multi-anchor commit = ONE undo entry");
    auto env = [](const QString& shapes) {
        return QStringLiteral(
            "{\"version\":\"2.0\",\"coordinate_system\":\"block-local\","
            "\"space\":\"px\",\"shapes\":[%1]}").arg(shapes);
    };
    const QString sh1 = QStringLiteral(
        "{\"id\":\"s1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[-390.0,2.0],[10.0,44.0]]}");
    const QString sh2 = QStringLiteral(
        "{\"id\":\"s2\",\"type\":\"freehand\",\"color\":[0,1,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[0.0,0.0],[20.0,20.0]]}");
    const QString sh3 = QStringLiteral(
        "{\"id\":\"s3\",\"type\":\"freehand\",\"color\":[0,0,1,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[5.0,5.0],[30.0,10.0]]}");

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("alpha"));
    m.insertBlock(1); m.setContent(1, QStringLiteral("beta"));

    // Blob A carries s1+s2, blob B carries s3.
    const QString a0 = env(sh1 + QStringLiteral(",") + sh2);
    const QString b0 = env(sh3);
    m.setBlockInk(0, a0);
    m.setBlockInk(1, b0);

    // Group move: s2 departs A and arrives in B (both blobs change).
    const QString a1 = env(sh1);
    const QString b1 = env(sh3 + QStringLiteral(",") + sh2);
    const int entries = m.undoHistory().size();
    m.beginGroup(0, 1);
    m.setBlockInk(0, a1);
    m.setBlockInk(1, b1);
    m.endGroup();
    CHECK(m.inkForRow(0) == a1 && m.inkForRow(1) == b1, "group move landed");
    CHECK(m.undoHistory().size() == entries + 1,
          "cross-anchor move is ONE undo entry");
    m.undo();
    CHECK(m.inkForRow(0) == a0 && m.inkForRow(1) == b0,
          "one undo restores BOTH blobs");
    m.redo();
    CHECK(m.inkForRow(0) == a1 && m.inkForRow(1) == b1,
          "one redo re-applies BOTH blobs");

    // Cross-anchor DELETE commits the same shape (empty blob = clear).
    m.beginGroup(0, 1);
    m.setBlockInk(0, QString());
    m.setBlockInk(1, QString());
    m.endGroup();
    CHECK(m.inkBlockIds().isEmpty(), "group delete cleared both blobs");
    m.undo();
    CHECK(m.inkForRow(0) == a1 && m.inkForRow(1) == b1,
          "one undo restores both deleted blobs");

    // SCATTERED anchors (sparse-entry path, 2026-08-20): blobs on the band's
    // ends with untouched blocks between — the entry stores patches for the
    // two touched rows only; undo/redo must restore them and leave the
    // middle rows alone.
    m.insertBlock(2); m.setContent(2, QStringLiteral("gamma"));
    m.insertBlock(3); m.setContent(3, QStringLiteral("delta"));
    m.insertBlock(4); m.setContent(4, QStringLiteral("epsilon"));
    m.setBlockInk(4, b0);
    const int entries2 = m.undoHistory().size();
    m.beginGroup(0, 4);
    m.setBlockInk(0, a0);
    m.setBlockInk(4, b1);
    m.endGroup();
    CHECK(m.undoHistory().size() == entries2 + 1,
          "scattered-anchor group is ONE undo entry");
    m.undo();
    CHECK(m.inkForRow(0) == a1 && m.inkForRow(4) == b0
              && m.contentForRow(2) == QStringLiteral("gamma")
              && m.contentForRow(3) == QStringLiteral("delta"),
          "sparse undo restores the ends, middle rows untouched");
    m.redo();
    CHECK(m.inkForRow(0) == a0 && m.inkForRow(4) == b1,
          "sparse redo re-applies both ends");
    m.closeDocument();
}

// --- Test 33: mixed-kind sketch group DELETE undo -------------------------
// The canvas's group delete emits ONE edited() for strokes plus per-index
// image/text removals in DESCENDING order, bracketed — drive the mutators in
// that exact shape (brackets are QML-wired, so this is the model driver).
static void testSketchGroupDelete() {
    qInfo("[33] sketch group delete: strokes+image+text bracketed = ONE undo entry");
    const QString probePng = QDir::temp().filePath(QStringLiteral("mn_group_del.png"));
    { QImage probe(32, 24, QImage::Format_RGB32); probe.fill(Qt::darkCyan);
      probe.save(probePng, "PNG"); }

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("anchor"));
    const int row = m.insertSketch(0);
    CHECK(row == 1 && m.mediaKind(row) == QLatin1String("sketch"), "sketch inserted");
    m.sketchSetShapes(row, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
        "{\"id\":\"f1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[0.2,0.2],[0.4,0.4]]},"
        "{\"id\":\"f2\",\"type\":\"freehand\",\"color\":[0,1,0,1],\"stroke_width\":4,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[0.6,0.6],[0.8,0.8]]}]}"));
    CHECK(m.sketchAddImageFromUrl(row, QUrl::fromLocalFile(probePng).toString()),
          "image element placed");
    CHECK(m.sketchAddText(row, 0.5, 0.5, 0.3, QStringLiteral("label"), 20,
                          QStringLiteral("#FF8800")) == 0, "text element placed");
    const QString pre = m.contentForRow(row);

    // Group delete of everything: strokes fold to one empty edited(), then
    // image/text removals (descending — single elements here).
    const int entries = m.undoHistory().size();
    m.beginGroup(row, row);
    m.sketchSetShapes(row, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":[]}"));
    m.sketchRemoveImage(row, 0);
    m.sketchRemoveText(row, 0);
    m.endGroup();
    {
        const QJsonObject o = QJsonDocument::fromJson(
            m.contentForRow(row).toUtf8()).object();
        CHECK(o.value(QStringLiteral("images")).toArray().isEmpty()
                  && o.value(QStringLiteral("texts")).toArray().isEmpty(),
              "all three kinds deleted");
    }
    CHECK(m.undoHistory().size() == entries + 1,
          "bracketed mixed delete is ONE undo entry");
    const QString post = m.contentForRow(row);
    m.undo();
    CHECK(m.contentForRow(row) == pre,
          "one undo restores strokes + image + text verbatim");
    m.redo();
    CHECK(m.contentForRow(row) == post, "redo re-applies the group delete");
    m.closeDocument();
}

// --- Test 34: PDF page-ink undo cycle --------------------------------------
// Per-page ink rides the block's content JSON (BlockSnap.content), so txn
// undo, erase-to-empty key drops, and block-delete restore all come from the
// standard machinery — prove each leg.
static void testPdfPageInkUndo() {
    qInfo("[34] PDF page ink: set/undo/redo, key drop, block-delete restore");
    auto shapes = [](const char* id) {
        return QStringLiteral(
            "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
            "{\"id\":\"%1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":4,"
            "\"filled\":false,\"is_modeled\":true,\"points\":[[0.1,0.1],[0.3,0.3]]}]}")
            .arg(QLatin1String(id));
    };
    auto shapeCount = [](const QString& env) {
        return QJsonDocument::fromJson(env.toUtf8()).object()
            .value(QStringLiteral("shapes")).toArray().size();
    };

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("anchor"));
    {   // hand-built PDF descriptor: isPdf derives from kind (no file probe)
        BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
        sp.mediaJson = QStringLiteral(
            "{\"src\":\"/nowhere/spec.pdf\",\"w\":612,\"h\":792,"
            "\"kind\":\"pdf\",\"pages\":3}");
        m.insertSpecs(0, {sp}, false);
    }
    const int row = 1;
    CHECK(m.mediaKind(row) == QLatin1String("pdf"), "pdf row landed");

    // Set → undo → redo on page 0.
    m.pdfSetPageInk(row, 0, shapes("p0"));
    CHECK(shapeCount(m.pdfPageInk(row, 0)) == 1, "page-0 ink stored");
    m.undo();
    CHECK(m.pdfPageInk(row, 0).isEmpty(), "undo clears page-0 ink");
    m.redo();
    CHECK(shapeCount(m.pdfPageInk(row, 0)) == 1, "redo restores page-0 ink");

    // Second page is independent; erase-to-empty drops page 0's key only.
    m.pdfSetPageInk(row, 2, shapes("p2"));
    m.pdfSetPageInk(row, 0, QStringLiteral("{\"shapes\":[]}"));
    CHECK(m.pdfPageInk(row, 0).isEmpty() && shapeCount(m.pdfPageInk(row, 2)) == 1,
          "erase-to-empty drops page 0, page 2 untouched");
    m.undo();
    CHECK(shapeCount(m.pdfPageInk(row, 0)) == 1, "undo restores the dropped page key");

    // Block delete → undo: ink on BOTH pages rides BlockSnap.content back.
    m.removeBlock(row);
    CHECK(m.rowCountQml() == 1, "pdf block removed");
    m.undo();
    CHECK(m.mediaKind(row) == QLatin1String("pdf")
              && shapeCount(m.pdfPageInk(row, 0)) == 1
              && shapeCount(m.pdfPageInk(row, 2)) == 1,
          "undo of removeBlock restores the block AND both pages' ink");

    // --- Text chips on pages (2026-08-20) ---
    auto textCount = [](const QString& env) {
        return QJsonDocument::fromJson(env.toUtf8()).object()
            .value(QStringLiteral("texts")).toArray().size();
    };
    auto textAt = [](const QString& env, int i, const char* key) {
        return QJsonDocument::fromJson(env.toUtf8()).object()
            .value(QStringLiteral("texts")).toArray().at(i).toObject()
            .value(QLatin1String(key));
    };
    const int ti = m.pdfAddPageText(row, 0, 0.2, 0.3, 0.25,
                                    QStringLiteral("page note"), 18,
                                    QStringLiteral("#FF8800"));
    CHECK(ti == 0 && textCount(m.pdfPageInk(row, 0)) == 1,
          "chip added to page 0");
    // THE BLOCKER REGRESSION: a stroke commit must PRESERVE the page's chips
    // (edited() carries strokes only — the old rebuild erased texts).
    m.pdfSetPageInk(row, 0, shapes("p0b"));
    CHECK(shapeCount(m.pdfPageInk(row, 0)) == 1
              && textCount(m.pdfPageInk(row, 0)) == 1,
          "stroke commit preserves the page's chips");
    m.pdfSetPageText(row, 0, 0, QStringLiteral("edited note"));
    CHECK(textAt(m.pdfPageInk(row, 0), 0, "text").toString()
              == QStringLiteral("edited note"), "chip text edited");
    m.pdfSetPageTextBox(row, 0, 0, 0.5, 0.6, 0.001, 18);   // width → 2em floor
    CHECK(textAt(m.pdfPageInk(row, 0), 0, "w").toDouble() >= (2.0 * 18) / 612.0,
          "box move applies the 2em width floor");
    m.undo();   // box move
    m.undo();   // text edit
    CHECK(textAt(m.pdfPageInk(row, 0), 0, "text").toString()
              == QStringLiteral("page note"), "undo walks the chip edits back");
    m.redo(); m.redo();
    // Blank commit deletes; chips alone keep the page key alive.
    m.pdfSetPageInk(row, 0, QStringLiteral("{\"shapes\":[]}"));
    CHECK(shapeCount(m.pdfPageInk(row, 0)) == 0
              && textCount(m.pdfPageInk(row, 0)) == 1,
          "erasing all strokes keeps a chip-only page");
    m.pdfSetPageText(row, 0, 0, QStringLiteral("   "));
    CHECK(m.pdfPageInk(row, 0).isEmpty(),
          "blank commit deletes the last chip → page key drops");
    m.undo();
    CHECK(textCount(m.pdfPageInk(row, 0)) == 1, "undo restores the deleted chip");
    m.closeDocument();
}

// --- Test 35: export of PDF page ink + chips ------------------------------
// Poster + annotated pages (ruling 2026-08-20): page 1's page ink rides the
// HTML .ink stack; every OTHER page with ink/chips exports as its own
// captioned figure; clean pages stay out. Fixture generated headless with
// QPdfWriter itself.
static void testPdfPageInkExport() {
    qInfo("[35] export: PDF page ink + chips → overlays + annotated pages");
    QDir dir(QCoreApplication::applicationDirPath()
             + QStringLiteral("/mn_pdfink_export"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    const QString pdf = dir.filePath(QStringLiteral("spec.pdf"));
    {   // 3-page fixture
        QPdfWriter w(pdf);
        w.setResolution(96);
        QPainter p(&w);
        p.drawText(100, 100, QStringLiteral("page one"));
        w.newPage(); p.drawText(100, 100, QStringLiteral("page two"));
        w.newPage(); p.drawText(100, 100, QStringLiteral("page three"));
        p.end();
    }

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("anchor"));
    {
        BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
        sp.mediaJson = QStringLiteral(
            "{\"src\":\"%1\",\"w\":595,\"h\":842,\"kind\":\"pdf\",\"pages\":3}").arg(pdf);
        m.insertSpecs(0, {sp}, false);
    }
    const int row = 1;
    CHECK(m.mediaKind(row) == QLatin1String("pdf") && m.mediaPdfPages(row) == 3,
          "pdf fixture block landed");
    // Page 1: strokes. Page 3: chip ONLY (the chips-keep-page contract).
    m.pdfSetPageInk(row, 0, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
        "{\"id\":\"e1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":6,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[0.1,0.1],[0.6,0.6]]}]}"));
    CHECK(m.pdfAddPageText(row, 2, 0.2, 0.2, 0.3, QStringLiteral("chip on 3"),
                           24, QStringLiteral("#FF8800")) == 0, "chip on page 3");

    class NameSink : public Exporter::AssetSink {
    public:
        QStringList names; QList<QImage> images;
        QString addFile(const QString&, const QString& b) override {
            names << b; return QStringLiteral("assets/") + b; }
        QString addImage(const QImage& img, const QString& b) override {
            names << b; images << img;
            return QStringLiteral("assets/") + b + QStringLiteral(".png"); }
    };
    Exporter ex;
    ex.setModel(&m);
    NameSink sink;
    const QString html = ex.toHtml(Exporter::Options{}, sink);
    CHECK(html.contains(QStringLiteral("page 3 of 3")), "annotated page 3 figure emitted");
    CHECK(!html.contains(QStringLiteral("page 2 of 3")), "clean page 2 NOT exported");
    CHECK(html.contains(QStringLiteral("_page1_ink"))
              && html.contains(QStringLiteral("_page3_ink")),
          "page-ink overlays ride the .ink stack (toggle-aware)");
    CHECK(html.contains(QStringLiteral("id=\"mn-ink\"")), "Annotations toggle armed");
    CHECK(html.contains(QStringLiteral("<header class=\"doctitle\">")),
          "document-name header leads the HTML");
    bool anyLit = false;
    for (const QImage& im : sink.images)
        if (im.hasAlphaChannel())
            for (int yy = 0; yy < im.height() && !anyLit; yy += 3)
                for (int xx = 0; xx < im.width() && !anyLit; xx += 3)
                    if (qAlpha(im.pixel(xx, yy)) != 0) anyLit = true;
    CHECK(anyLit, "overlay rasters carry visible ink");
    CHECK(ex.scan().value(QStringLiteral("pdfInkPages")).toInt() == 2,
          "scan counts both annotated pages");

    NameSink mdSink;
    const QString md = ex.toMarkdown(Exporter::Options{}, mdSink);
    CHECK(md.contains(QStringLiteral("page 3")) && md.contains(QStringLiteral("_page3")),
          "markdown embeds the baked annotated page");
    m.closeDocument();
    dir.removeRecursively();
}

// --- Test 36: Export as PDF (QTextDocument → QPdfWriter) -------------------
// Headless assertions: %PDF magic, QPdfDocument round-trip (page count +
// extractable text — Qt writes ToUnicode cmaps), soft checks for the
// embedded body font and a link URI, and the annotated-PDF-pages leg over a
// QPdfWriter-generated fixture. Visual placement is the manual pass.
static void testExportPdf() {
    qInfo("[36] export as PDF: round-trip, text, font, links, annotated pages");
    QDir dir(QCoreApplication::applicationDirPath()
             + QStringLiteral("/mn_pdf_out"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    const QString fixture = dir.filePath(QStringLiteral("ref.pdf"));
    {   // 3-page reference PDF for the media block
        QPdfWriter w(fixture);
        w.setResolution(96);
        QPainter p(&w);
        p.drawText(100, 100, QStringLiteral("fixture page"));
        w.newPage(); w.newPage();
        p.end();
    }

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    int r = 0;
    m.insertBlock(r); m.setContent(r, QStringLiteral("Export Title"));
    m.setHeading(r, 1);
    m.insertBlock(++r); m.setContent(r, QStringLiteral("plain bolditalic linked"));
    m.setFormat(r, 6, 16, QStringLiteral("bold"), true);
    m.setFormat(r, 6, 16, QStringLiteral("italic"), true);
    m.setLink(r, 17, 23, QStringLiteral("https://example.com/spec"));
    m.insertBlock(++r); m.setContent(r, QStringLiteral("item one"));
    m.setBlockType(r, BlockModel::ListItem);
    m.insertBlock(++r); m.setContent(r, QStringLiteral("int x = 1;"));
    m.makeCodeBlock(r, QStringLiteral("cpp"));
    const int tRow = m.insertTable(r, 2, 2);
    m.tableSetCell(tRow, 0, 0, QStringLiteral("H1"));
    m.tableSetCell(tRow, 1, 0, QStringLiteral("cell body"));
    r = tRow;
    {   // pdf media block with ink on page 3
        BlockModel::BlockSpec sp; sp.type = BlockModel::Media;
        sp.mediaJson = QStringLiteral(
            "{\"src\":\"%1\",\"w\":595,\"h\":842,\"kind\":\"pdf\",\"pages\":3}")
            .arg(fixture);
        m.insertSpecs(r, {sp}, false);
    }
    const int pdfRow = r + 1;
    CHECK(m.mediaKind(pdfRow) == QLatin1String("pdf"), "pdf block landed");
    m.pdfSetPageInk(pdfRow, 2, QStringLiteral(
        "{\"version\":\"2.0\",\"coordinate_system\":\"normalized\",\"shapes\":["
        "{\"id\":\"k1\",\"type\":\"freehand\",\"color\":[1,0,0,1],\"stroke_width\":6,"
        "\"filled\":false,\"is_modeled\":true,\"points\":[[0.1,0.1],[0.7,0.7]]}]}"));
    // Oversized image: MUST scale into the column, never crop at the right
    // (the QTextDocument::print 2cm-margin bug, 2026-08-20).
    const QString bigPng = dir.filePath(QStringLiteral("big.png"));
    { QImage big(2400, 1200, QImage::Format_RGB32); big.fill(Qt::darkMagenta);
      big.save(bigPng, "PNG"); }
    CHECK(m.insertImageFromUrl(pdfRow, QUrl::fromLocalFile(bigPng).toString()) > 0,
          "oversized image inserted");

    Exporter ex;
    ex.setModel(&m);
    QBuffer buf;
    buf.open(QIODevice::ReadWrite);
    CHECK(ex.toPdf(Exporter::Options{}, buf), "toPdf produced bytes");
    CHECK(buf.data().startsWith("%PDF-"), "PDF magic present");

    const QString outPdf = dir.filePath(QStringLiteral("out.pdf"));
    { QFile pf(outPdf); CHECK(pf.open(QIODevice::WriteOnly), "out.pdf writable");
      pf.write(buf.data()); }
    QPdfDocument pdoc;
    CHECK(pdoc.load(outPdf) == QPdfDocument::Error::None, "QPdfDocument re-reads it");
    CHECK(pdoc.pageCount() >= 1, "at least one page (%d)", pdoc.pageCount());
    QString all;
    for (int pg = 0; pg < pdoc.pageCount(); ++pg)
        all += pdoc.getAllText(pg).text();
    CHECK(all.contains(QStringLiteral("Untitled")),
          "document-name header on page 1 (ruling 2026-08-20)");
    CHECK(all.contains(QStringLiteral("Export Title")), "heading text extracts");
    CHECK(all.contains(QStringLiteral("bolditalic")), "span text extracts");
    CHECK(all.contains(QStringLiteral("item one")), "list text extracts");
    CHECK(all.contains(QStringLiteral("cell body")), "table text extracts");
    CHECK(all.contains(QStringLiteral("page 3 of 3")), "annotated page caption present");
    // Nothing may paint into the right margin: render every page and demand
    // the rightmost 5% band (well inside the ~8.6% margin) stays paper-white.
    for (int pg = 0; pg < pdoc.pageCount(); ++pg) {
        const QSizeF pts = pdoc.pagePointSize(pg);
        const QImage pgImg = pdoc.render(pg, QSize(int(pts.width()), int(pts.height())));
        if (pgImg.isNull()) continue;
        pgImg.save(QDir::tempPath()   // per-page inspection artifacts
                   + QStringLiteral("/mn_export_probe_page%1.png").arg(pg + 1));
        bool clean = true;
        int worstX = -1, worstY = -1;
        for (int yy = 0; yy < pgImg.height() && clean; yy += 2)
            for (int xx = int(pgImg.width() * 0.95); xx < pgImg.width(); xx += 2) {
                const QRgb px = pgImg.pixel(xx, yy);
                if (qAlpha(px) != 0 && (qRed(px) < 245 || qGreen(px) < 245
                                        || qBlue(px) < 245)) {
                    clean = false; worstX = xx; worstY = yy; break;
                }
            }
        CHECK(clean, "page %d right margin is clean (dirty at %d,%d)",
              pg + 1, worstX, worstY);
    }
    // The inline-image dpi-scale bug (2026-08-20): the annotated fixture
    // page is inserted at 480 units, so its red stroke (ending at 0.7 of
    // the image) must stay in the LEFT HALF-ish of the page — the 96/72
    // scale drew it at ~0.63 of the page width. White-on-white overflow is
    // invisible to the margin sweep; the stroke pins the geometry.
    {
        int maxRedX = -1, redPageW = 1;
        for (int pg = 0; pg < pdoc.pageCount(); ++pg) {
            const QSizeF pts = pdoc.pagePointSize(pg);
            const QImage pgImg = pdoc.render(pg, QSize(int(pts.width()), int(pts.height())));
            if (pgImg.isNull()) continue;
            for (int yy = 0; yy < pgImg.height(); yy += 2)
                for (int xx = 0; xx < pgImg.width(); xx += 2) {
                    const QRgb px = pgImg.pixel(xx, yy);
                    if (qAlpha(px) != 0 && qRed(px) > 180 && qGreen(px) < 90
                        && qBlue(px) < 90 && xx > maxRedX) {
                        maxRedX = xx; redPageW = pgImg.width();
                    }
                }
        }
        CHECK(maxRedX > 0 && maxRedX < int(redPageW * 0.55),
              "annotated-page image drawn at its INTENDED size (red ends %d/%d)",
              maxRedX, redPageW);
    }
    // Soft checks (byte-level; engine details, not contracts):
    if (!buf.data().contains("Aspekta"))
        qInfo("  (note: body font descriptor not found — synthesized fallback?)");
    if (!buf.data().contains("https://example.com/spec"))
        qInfo("  (note: link URI not found raw — anchors may not emit annotations)");
    {   // Inspection artifact for the manual visual pass.
        QFile pf(QDir::tempPath() + QStringLiteral("/mn_export_probe.pdf"));
        if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            pf.write(buf.data());
    }
    m.closeDocument();
    dir.removeRecursively();
}

// --- Test 37: DT-2 inline choice chips --------------------------------------
// The span's TEXT is the selected LABEL (the load-bearing invariant): swaps
// are one-txn text+payload rewrites, neighbours shift, exporters flatten
// bare, and the chip survives save/reopen via the "choice" kind string.
static void testInlineChoice() {
    qInfo("[37] DT-2 inline choice: insert/select/options/remove, text==label");
    auto payloadOf = [](const QString& j) {
        return QJsonDocument::fromJson(j.toUtf8()).object();
    };
    auto idFor = [&](const QString& j, const QString& label) {
        for (const QJsonValue& v : payloadOf(j).value(QStringLiteral("o")).toArray())
            if (v.toObject().value(QStringLiteral("l")).toString() == label)
                return v.toObject().value(QStringLiteral("id")).toString();
        return QString();
    };

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("status: here"));

    const int s = m.insertChoiceAt(0, 8);
    CHECK(s == 8, "chip inserted at the caret (start=%d)", s);
    CHECK(m.contentForRow(0) == QStringLiteral("status: To dohere"),
          "default label spliced into the text");
    const QString pj = m.choiceAt(0, 9);
    CHECK(!pj.isEmpty()
              && payloadOf(pj).value(QStringLiteral("o")).toArray().size() == 3
              && payloadOf(pj).value(QStringLiteral("v")).toString()
                     == idFor(pj, QStringLiteral("To do")),
          "payload carries the tri-state set, To do selected");
    CHECK(m.choiceRangeAt(0, 9) == (QVariantList{8, 13}), "range covers the label");

    // A neighbour span must shift when the label swaps.
    m.setFormat(0, 13, 17, QStringLiteral("bold"), true);   // "here"
    m.setChoiceSelected(0, 8, idFor(pj, QStringLiteral("Done")));
    CHECK(m.contentForRow(0) == QStringLiteral("status: Donehere"),
          "selecting swaps the label text");
    CHECK(m.choiceRangeAt(0, 9) == (QVariantList{8, 12}), "span follows the new label");
    CHECK(m.hasFormat(0, 12, 16, QStringLiteral("bold")),
          "neighbour span shifted with the swap");
    CHECK(m.choiceRangesForRow(0).first().toMap()
              .value(QStringLiteral("color")).toString()
              == QStringLiteral("#58A65C"),
          "overlay feed reports the selected option's color");
    m.undo();
    CHECK(m.contentForRow(0) == QStringLiteral("status: To dohere")
              && m.hasFormat(0, 13, 17, QStringLiteral("bold")),
          "one undo restores text, payload AND the neighbour");
    m.redo();

    // Quick-add selects, labels sanitize (markdown-active chars stripped).
    const QString nid = m.choiceAddOption(0, 8, QStringLiteral("Blo*ck*ed"),
                                          QStringLiteral("#FF0000"));
    CHECK(!nid.isEmpty()
              && m.contentForRow(0) == QStringLiteral("status: Blockedhere"),
          "quick-add selects; label sanitized of markdown chars");

    // Options commit: drop the selected option → falls back to the first.
    {
        const QString cur = m.choiceAt(0, 9);
        QVariantList arr;
        for (const QJsonValue& v : payloadOf(cur).value(QStringLiteral("o")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toString() == nid) continue;   // delete it
            QVariantMap e;
            e.insert(QStringLiteral("id"), o.value(QStringLiteral("id")).toString());
            e.insert(QStringLiteral("label"), o.value(QStringLiteral("l")).toString());
            e.insert(QStringLiteral("color"), o.value(QStringLiteral("c")).toString());
            arr.append(e);
        }
        m.setChoiceOptions(0, 8, arr);
        CHECK(m.contentForRow(0) == QStringLiteral("status: To dohere"),
              "deleted-selected falls back to the first option");
    }

    // Save/reopen: the "choice" kind string must round-trip.
    const QString path = QDir::tempPath() + QStringLiteral("/mn_choice_rt.mndb");
    QFile::remove(path);
    CHECK(m.saveAs(path), "doc saved");
    m.closeDocument();
    BlockModel m2;
    CHECK(m2.openDocument(path), "doc reopened");
    CHECK(!m2.choiceAt(0, 9).isEmpty()
              && m2.contentForRow(0) == QStringLiteral("status: To dohere"),
          "chip survives save/reopen (kind string + label intact)");

    // Split inside the chip snaps to its end (no payload-sharing clones).
    m2.splitBlock(0, 10);
    CHECK(m2.contentForRow(0) == QStringLiteral("status: To do")
              && m2.contentForRow(1) == QStringLiteral("here")
              && !m2.choiceAt(0, 9).isEmpty() && m2.choiceAt(1, 0).isEmpty(),
          "split inside the chip snapped to its end");
    m2.undo();

    // Exporters flatten to the bare label (the text==label payoff).
    Exporter ex;
    ex.setModel(&m2);
    // The bold on "here" (set above) must emit normally while the chip
    // flattens to its bare label — the two coexist on one line.
    const QString mdout = ex.copyMarkdown(0, 0);
    CHECK(mdout.contains(QStringLiteral("status: To do**here**")),
          "markdown: chip flattens bare, neighbour bold emits (got '%s')",
          qPrintable(mdout.left(80)));

    // Clear = remove chip + label; one undo restores both.
    m2.removeChoiceAt(0, 8);
    CHECK(m2.contentForRow(0) == QStringLiteral("status: here")
              && m2.choiceRangesForRow(0).isEmpty(),
          "remove kills chip and label text");
    m2.undo();
    CHECK(!m2.choiceAt(0, 9).isEmpty(), "undo restores the chip whole");
    m2.closeDocument();
    QFile::remove(path);
}

// --- Test 38: undo restores keep MEASURED heights (the History-toggle bug) --
// applySnapshot used to drop restored rows to estimatedHeight — blind to a
// table's cell media — and the delegate correction only fires on an ACTUAL
// height change, so a jump landing at the already-rendered height left the
// estimate standing (rows didn't resize around pasted images). Restored rows
// now SEED from their pre-reset height; sparse patches keep theirs.
static void testUndoHeightSeeding() {
    qInfo("[38] undo height seeding: band + sparse restores keep real heights");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("alpha"));
    const int tRow = m.insertTable(0, 2, 2);
    CHECK(tRow == 1, "table inserted");
    auto rowH = [&](int r) {
        return (r + 1 < m.rowCountQml() ? m.yForRow(r + 1) : m.totalHeight())
               - m.yForRow(r);
    };

    // Band path: a cell edit + undo/redo must keep the measured 333, not
    // drop to the (media-blind) estimate.
    m.setMeasuredHeight(tRow, 333);
    m.tableSetCell(tRow, 1, 1, QStringLiteral("edited"));
    m.undo();
    CHECK(qAbs(rowH(tRow) - 333.0) < 0.5,
          "undo keeps the table's measured height (got %f)", rowH(tRow));
    CHECK(!m.rowMeasured(tRow), "restored row still re-measures (flag cleared)");
    m.redo();
    CHECK(qAbs(rowH(tRow) - 333.0) < 0.5, "redo keeps it too");

    // Sparse path: a grouped formatting sweep over scattered rows patches
    // in place — heights must survive there as well.
    m.insertBlock(2); m.setContent(2, QStringLiteral("beta"));
    m.insertBlock(3); m.setContent(3, QStringLiteral("gamma"));
    m.setMeasuredHeight(0, 101);
    m.setMeasuredHeight(2, 102);
    m.setMeasuredHeight(3, 103);
    m.beginGroup(0, 3);
    m.setFormat(0, 0, 5, QStringLiteral("bold"), true);
    m.setFormat(3, 0, 5, QStringLiteral("bold"), true);
    m.endGroup();
    m.undo();
    CHECK(qAbs(rowH(0) - 101.0) < 0.5 && qAbs(rowH(3) - 103.0) < 0.5,
          "sparse undo keeps patched rows' heights");
    CHECK(qAbs(rowH(2) - 102.0) < 0.5 && m.rowMeasured(2),
          "untouched row keeps height AND its measured flag");
    m.closeDocument();
}

// --- Test 39: new import formats — xlsx / ods / odt / source code ----------
// Fixtures are hand-authored OOXML/ODF parts via PackageWriter (the Notion
// test's pattern — no exporter exists for these formats).
static void testNewImportFormats() {
    qInfo("[39] import: xlsx sheets, ods repeats, odt styles, code files");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_imp_new"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());

    // Classifier: office containers beat the Notion zip sniff; code maps.
    CHECK(Importer::formatForPath(QStringLiteral("a.xlsx")) == QLatin1String("xlsx")
              && Importer::formatForPath(QStringLiteral("a.ods")) == QLatin1String("ods")
              && Importer::formatForPath(QStringLiteral("a.odt")) == QLatin1String("odt")
              && Importer::formatForPath(QStringLiteral("a.py")) == QLatin1String("code"),
          "classifier maps the new formats");

    // --- xlsx: 2 sheets, shared + inline strings, sparse row ---
    const QString xlsx = dir.filePath(QStringLiteral("book.xlsx"));
    {
        mnpkg::PackageWriter w(xlsx);
        w.addCompressed(QStringLiteral("xl/workbook.xml"), QByteArray(
            "<workbook xmlns:r=\"r\"><sheets>"
            "<sheet name=\"Alpha\" sheetId=\"1\" r:id=\"rId1\"/>"
            "<sheet name=\"Beta\" sheetId=\"2\" r:id=\"rId2\"/>"
            "</sheets></workbook>"));
        w.addCompressed(QStringLiteral("xl/_rels/workbook.xml.rels"), QByteArray(
            "<Relationships>"
            "<Relationship Id=\"rId1\" Target=\"worksheets/sheet1.xml\"/>"
            "<Relationship Id=\"rId2\" Target=\"worksheets/sheet2.xml\"/>"
            "</Relationships>"));
        w.addCompressed(QStringLiteral("xl/sharedStrings.xml"), QByteArray(
            "<sst><si><t>Name</t></si><si><t>Qty</t></si>"
            "<si><r><t>rich </t></r><r><t>run</t></r></si></sst>"));
        w.addCompressed(QStringLiteral("xl/worksheets/sheet1.xml"), QByteArray(
            "<worksheet xmlns:r=\"r\"><sheetData>"
            "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c>"
            "<c r=\"C1\" t=\"e\" vm=\"1\"><v>#VALUE!</v></c>"
            "<c r=\"Z1\" s=\"7\"/></row>"
            "<row r=\"3\"><c r=\"A3\" t=\"s\"><v>2</v></c><c r=\"C3\"><v>42</v></c></row>"
            "</sheetData><drawing r:id=\"rId9\"/></worksheet>"));
        w.addCompressed(QStringLiteral("xl/worksheets/sheet2.xml"), QByteArray(
            "<worksheet><sheetData>"
            "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>solo</t></is></c></row>"
            "</sheetData></worksheet>"));
        // Drawing-anchored picture ("place over cells") → lands at B2.
        w.addCompressed(QStringLiteral("xl/worksheets/_rels/sheet1.xml.rels"), QByteArray(
            "<Relationships>"
            "<Relationship Id=\"rId9\" Target=\"../drawings/drawing1.xml\"/>"
            "</Relationships>"));
        w.addCompressed(QStringLiteral("xl/drawings/drawing1.xml"), QByteArray(
            "<xdr:wsDr xmlns:xdr=\"x\" xmlns:a=\"a\" xmlns:r=\"r\">"
            "<xdr:twoCellAnchor>"
            "<xdr:from><xdr:col>1</xdr:col><xdr:row>1</xdr:row></xdr:from>"
            "<xdr:to><xdr:col>2</xdr:col><xdr:row>2</xdr:row></xdr:to>"
            "<xdr:pic><xdr:blipFill><a:blip r:embed=\"rId1\"/></xdr:blipFill></xdr:pic>"
            "</xdr:twoCellAnchor></xdr:wsDr>"));
        w.addCompressed(QStringLiteral("xl/drawings/_rels/drawing1.xml.rels"), QByteArray(
            "<Relationships>"
            "<Relationship Id=\"rId1\" Target=\"../media/image1.png\"/>"
            "</Relationships>"));
        // Rich-value in-cell image ("place in cell") → C1's vm=1 chain.
        w.addCompressed(QStringLiteral("xl/metadata.xml"), QByteArray(
            "<metadata xmlns:xlrd=\"d\">"
            "<futureMetadata name=\"XLRICHVALUE\" count=\"1\">"
            "<bk><extLst><ext><xlrd:rvb i=\"0\"/></ext></extLst></bk></futureMetadata>"
            "<valueMetadata count=\"1\"><bk><rc t=\"1\" v=\"0\"/></bk></valueMetadata>"
            "</metadata>"));
        w.addCompressed(QStringLiteral("xl/richData/rdrichvalue.xml"), QByteArray(
            "<rvData count=\"1\"><rv s=\"0\"><v>0</v><v>5</v></rv></rvData>"));
        w.addCompressed(QStringLiteral("xl/richData/richValueRel.xml"), QByteArray(
            "<richValueRels xmlns:r=\"r\"><rel r:id=\"rId1\"/></richValueRels>"));
        w.addCompressed(QStringLiteral("xl/richData/_rels/richValueRel.xml.rels"), QByteArray(
            "<Relationships>"
            "<Relationship Id=\"rId1\" Target=\"../media/image2.png\"/>"
            "</Relationships>"));
        {   // real PNG bytes for both images
            auto png = [](const QColor& col) {
                QImage img(6, 4, QImage::Format_RGB32); img.fill(col);
                QByteArray bytes; QBuffer buf(&bytes);
                buf.open(QIODevice::WriteOnly); img.save(&buf, "PNG");
                return bytes;
            };
            w.addCompressed(QStringLiteral("xl/media/image1.png"), png(Qt::red));
            w.addCompressed(QStringLiteral("xl/media/image2.png"), png(Qt::blue));
        }
        CHECK(w.finish(), "xlsx fixture wrote");
    }
    {
        BlockModel m; m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        CHECK(Importer::importXlsxFile(xlsx, &m), "xlsx imported");
        int tables = 0, headings = 0, tRow = -1;
        for (int r = 0; r < m.rowCountQml(); ++r) {
            if (m.typeForRow(r) == BlockModel::Table) { ++tables; if (tRow < 0) tRow = r; }
            if (m.typeForRow(r) == BlockModel::Heading) ++headings;
        }
        CHECK(tables == 2 && headings == 2, "two sheets → two headed tables");
        CHECK(m.tableCell(tRow, 0, 0) == QStringLiteral("Name")
                  && m.tableCell(tRow, 0, 1) == QStringLiteral("Qty"),
              "shared strings resolved");
        CHECK(m.tableCell(tRow, 2, 0) == QStringLiteral("rich run")
                  && m.tableCell(tRow, 2, 2) == QStringLiteral("42")
                  && m.tableCell(tRow, 1, 0).isEmpty(),
              "rich-run si, numeric cell, sparse row gap");
        CHECK(m.tableColumns(tRow) == 3,
              "style-only cell (Z1, formatting to the sheet edge) adds no column");
        CHECK(m.tableCellMedia(tRow, 1, 1).contains(QStringLiteral(".minnotes/")),
              "drawing-anchored image landed at its from-cell (B2)");
        CHECK(m.tableCellMedia(tRow, 0, 2).contains(QStringLiteral(".minnotes/"))
                  && m.tableCell(tRow, 0, 2).isEmpty(),
              "rich-value in-cell image resolved; #VALUE! placeholder cleared");
        m.closeDocument();
    }

    // --- ods: repeated columns expand, sheet-edge padding trimmed ---
    const QString ods = dir.filePath(QStringLiteral("calc.ods"));
    {
        mnpkg::PackageWriter w(ods);
        w.addCompressed(QStringLiteral("content.xml"), QByteArray(
            "<office:document-content xmlns:office=\"o\" xmlns:table=\"t\" xmlns:text=\"x\">"
            "<office:body><office:spreadsheet>"
            "<table:table table:name=\"Only\">"
            "<table:table-row>"
            "<table:table-cell table:number-columns-repeated=\"2\"><text:p>dup</text:p></table:table-cell>"
            "<table:table-cell><text:p>end</text:p></table:table-cell>"
            "<table:table-cell table:number-columns-repeated=\"1013\"/>"
            "</table:table-row>"
            "<table:table-row table:number-rows-repeated=\"500\"/>"
            "</table:table></office:spreadsheet></office:body>"
            "</office:document-content>"));
        CHECK(w.finish(), "ods fixture wrote");
    }
    {
        BlockModel m; m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        CHECK(Importer::importOdsFile(ods, &m), "ods imported");
        int tRow = -1, headings = 0;
        for (int r = 0; r < m.rowCountQml(); ++r) {
            if (m.typeForRow(r) == BlockModel::Table && tRow < 0) tRow = r;
            if (m.typeForRow(r) == BlockModel::Heading) ++headings;
        }
        CHECK(tRow >= 0 && headings == 0, "single sheet → table, NO heading");
        CHECK(m.tableColumns(tRow) == 3 && m.tableRows(tRow) == 1,
              "repeats expanded, sheet-edge padding trimmed (%dx%d)",
              m.tableRows(tRow), m.tableColumns(tRow));
        CHECK(m.tableCell(tRow, 0, 0) == QStringLiteral("dup")
                  && m.tableCell(tRow, 0, 1) == QStringLiteral("dup")
                  && m.tableCell(tRow, 0, 2) == QStringLiteral("end"),
              "column repeat duplicated the value");
        m.closeDocument();
    }

    // --- odt: heading, styled spans via automatic-styles, list nesting ---
    const QString odt = dir.filePath(QStringLiteral("doc.odt"));
    {
        mnpkg::PackageWriter w(odt);
        w.addCompressed(QStringLiteral("content.xml"), QByteArray(
            "<office:document-content xmlns:office=\"o\" xmlns:text=\"x\" xmlns:style=\"s\" xmlns:fo=\"f\">"
            "<office:automatic-styles>"
            "<style:style style:name=\"T1\" style:family=\"text\">"
            "<style:text-properties fo:font-weight=\"bold\"/></style:style>"
            "<text:list-style style:name=\"L1\">"
            "<text:list-level-style-number text:level=\"1\"/></text:list-style>"
            "</office:automatic-styles>"
            "<office:body><office:text>"
            "<text:h text:outline-level=\"2\">Section</text:h>"
            "<text:p>plain <text:span text:style-name=\"T1\">bolded</text:span> tail</text:p>"
            "<text:list text:style-name=\"L1\"><text:list-item>"
            "<text:p>first item</text:p></text:list-item></text:list>"
            "</office:text></office:body></office:document-content>"));
        CHECK(w.finish(), "odt fixture wrote");
    }
    {
        BlockModel m; m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        CHECK(Importer::importOdtFile(odt, &m), "odt imported");
        int h = -1, p = -1, li = -1;
        for (int r = 0; r < m.rowCountQml(); ++r) {
            if (m.typeForRow(r) == BlockModel::Heading && h < 0) h = r;
            if (m.typeForRow(r) == BlockModel::Paragraph
                && m.contentForRow(r).startsWith(QStringLiteral("plain"))) p = r;
            if (m.typeForRow(r) == BlockModel::OrderedListItem) li = r;
        }
        CHECK(h >= 0 && m.levelForRow(h) == 2
                  && m.contentForRow(h) == QStringLiteral("Section"),
              "text:h → level-2 heading");
        CHECK(p >= 0 && m.contentForRow(p) == QStringLiteral("plain bolded tail")
                  && m.hasFormat(p, 6, 12, QStringLiteral("bold")),
              "automatic-style span resolved to bold");
        CHECK(li >= 0 && m.contentForRow(li) == QStringLiteral("first item"),
              "numbered list style → ordered item");
        m.closeDocument();
    }

    // --- source code file → one Code block with the extension as lang ---
    const QString py = dir.filePath(QStringLiteral("tool.py"));
    { QFile f(py); CHECK(f.open(QIODevice::WriteOnly), "py fixture writable");
      f.write("def main():\n    return 42\n"); }
    {
        BlockModel m; m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        CHECK(Importer::importCodeFile(py, &m), "code file imported");
        int c = -1;
        for (int r = 0; r < m.rowCountQml(); ++r)
            if (m.typeForRow(r) == BlockModel::Code) { c = r; break; }
        CHECK(c >= 0 && m.languageForRow(c) == QStringLiteral("py")
                  && m.contentForRow(c) == QStringLiteral("def main():\n    return 42"),
              "one Code block, lang=py, trailing newline trimmed");
        m.closeDocument();
    }

    // --- csv: Excel-style trailing padding trims on import ---
    const QString csv = dir.filePath(QStringLiteral("pad.csv"));
    { QFile f(csv); f.open(QIODevice::WriteOnly);
      f.write("a,b,,,\n1,2,,,\n,,,,\n"); }
    {
        BlockModel m; m.newDocument();
        while (m.rowCountQml() > 0) m.removeBlock(0);
        m.insertBlock(0);
        CHECK(Importer::importCsvFile(csv, &m), "padded csv imported");
        const int tRow = findRowOfType(m, BlockModel::Table);
        CHECK(tRow >= 0 && m.tableColumns(tRow) == 2 && m.tableRows(tRow) == 2,
              "trailing empty cols+rows trimmed (%dx%d)",
              m.tableRows(tRow), m.tableColumns(tRow));
        m.closeDocument();
    }
    dir.removeRecursively();
}

// --- Test 40: tab-merge engine — fidelity, ONE undo entry, thread re-mint --
// Two live models, whole-document copy at a gap: every block type + spans
// with payloads + margin ink + a comment thread with history must arrive
// intact, as exactly one destination undo entry. Source untouched.
static void testMergeEngine() {
    qInfo("[40] merge engine: full fidelity, ONE undo entry, thread re-mint");
    mn::DocInkAnchor a;
    a.space = mn::DocInkAnchor::Px;
    qcv::ActiveStroke st;
    st.tool = qcv::DrawingTool::Freehand;
    st.points = { QPointF(-450.0, 2.0), QPointF(-420.0, 30.0) };
    st.strokeWidth = 4;
    a.strokes.push_back(st);
    const QString kInk = mn::docInkToJson(a);
    const QString kChoice = QStringLiteral(
        "{\"o\":[{\"id\":\"a\",\"l\":\"To do\"},{\"id\":\"b\",\"l\":\"Doing\"}],\"v\":\"b\"}");

    BlockModel src;
    src.newDocument();
    while (src.rowCountQml() > 0) src.removeBlock(0);
    src.insertBlock(0); src.setContent(0, QStringLiteral("Title"));
    src.setHeading(0, 2);
    src.insertBlock(1); src.setContent(1, QStringLiteral("hello world"));
    src.setFormat(1, 0, 5, QStringLiteral("bold"), true);
    src.insertBlock(2); src.makeCodeBlock(2, QStringLiteral("py"));
    src.setContent(2, QStringLiteral("x = 1\ny = 2"));
    const int tRow = src.insertTable(2, 2, 2);           // row 3
    src.tableSetCell(tRow, 0, 0, QStringLiteral("c00"));
    {   // choice chip: text == selected label, payload in the span
        BlockModel::BlockSpec ch;
        ch.type = BlockModel::Paragraph;
        ch.text = QStringLiteral("Doing");
        ch.spans.push_back({0, 5, BlockModel::SpanChoice, kChoice});
        src.insertSpecs(tRow, {ch}, false);              // row 4
    }
    src.insertBlock(5); src.setContent(5, QStringLiteral("note me"));
    const QString tid = src.addComment(5, 0, 4);
    src.addCommentMessage(tid, QStringLiteral("first"));
    src.addCommentMessage(tid, QStringLiteral("second"));
    src.insertBlock(6); src.setContent(6, QStringLiteral("inked"));
    src.setBlockInk(6, kInk);
    const int srcN = src.rowCountQml();
    CHECK(srcN == 7 && !tid.isEmpty(), "source fixture: 7 blocks + a thread");

    BlockModel dest;
    dest.newDocument();
    while (dest.rowCountQml() > 0) dest.removeBlock(0);
    dest.insertBlock(0); dest.setContent(0, QStringLiteral("top"));
    dest.insertBlock(1); dest.setContent(1, QStringLiteral("bottom"));
    const int entriesBefore = dest.undoHistory().size();

    int first = -1, last = -1; QString err;
    CHECK(DocumentMerger::mergeDocuments(&src, &dest, 1, &first, &last, &err),
          "merge succeeded (%s)", qPrintable(err));
    CHECK(first == 1 && last == 7 && dest.rowCountQml() == 2 + srcN,
          "gap-1 merge landed between top and bottom (%d..%d)", first, last);

    // Fidelity, row by row.
    CHECK(dest.typeForRow(1) == BlockModel::Heading && dest.levelForRow(1) == 2
              && dest.contentForRow(1) == QStringLiteral("Title"),
          "heading level rode along");
    CHECK(dest.contentForRow(2) == QStringLiteral("hello world")
              && dest.hasFormat(2, 0, 5, QStringLiteral("bold")),
          "bold span rode along");
    CHECK(dest.typeForRow(3) == BlockModel::Code
              && dest.languageForRow(3) == QStringLiteral("py")
              && dest.contentForRow(3) == QStringLiteral("x = 1\ny = 2"),
          "code block + language rode along");
    CHECK(dest.typeForRow(4) == BlockModel::Table
              && TableGrid::fromJson(dest.contentForRow(4)).cellText(0, 0)
                     == QStringLiteral("c00"),
          "table content rode along");
    {
        bool choiceOk = false;
        for (const QVariant& v : dest.spansForRow(5)) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("k")).toInt() == int(BlockModel::SpanChoice)
                && m.value(QStringLiteral("u")).toString() == kChoice)
                choiceOk = true;
        }
        CHECK(choiceOk && dest.contentForRow(5) == QStringLiteral("Doing"),
              "choice chip payload rode along");
    }
    const QString newTid = dest.commentAt(6, 1);
    CHECK(!newTid.isEmpty() && newTid != tid,
          "comment anchor arrived under a NEW thread id");
    {
        const QVariantList sm = src.commentMessages(tid);
        const QVariantList dm = dest.commentMessages(newTid);
        CHECK(dm.size() == 2
                  && dm[0].toMap().value("body") == sm[0].toMap().value("body")
                  && dm[1].toMap().value("body") == QStringLiteral("second")
                  && dm[0].toMap().value("created") == sm[0].toMap().value("created"),
              "thread history preserved (bodies + timestamps)");
    }
    CHECK(dest.inkForRow(7) == kInk && dest.contentForRow(7) == QStringLiteral("inked"),
          "margin ink rode along (same width: byte-equal)");

    // Exactly ONE undo entry; undo restores the destination wholesale.
    CHECK(dest.undoHistory().size() == entriesBefore + 1, "merge = ONE undo entry");
    dest.undo();
    CHECK(dest.rowCountQml() == 2 && dest.contentForRow(0) == QStringLiteral("top")
              && dest.contentForRow(1) == QStringLiteral("bottom"),
          "one undo removes the whole merge");
    dest.redo();
    CHECK(dest.rowCountQml() == 2 + srcN && dest.inkForRow(7) == kInk
              && dest.commentAt(6, 1) == newTid,
          "redo brings it back with ink + comment anchor");

    // The source was never touched; refusals + the pristine no-op.
    CHECK(src.rowCountQml() == srcN && src.commentAt(5, 1) == tid,
          "source untouched (copy semantics)");
    CHECK(!DocumentMerger::mergeDocuments(&src, &src, 0, nullptr, nullptr, &err),
          "self-merge refused (%s)", qPrintable(err));
    {
        BlockModel blank;
        blank.newDocument();   // pristine: one empty paragraph, no ink
        const int before = dest.rowCountQml();
        int f = -2, l = -2;
        CHECK(DocumentMerger::mergeDocuments(&blank, &dest, 0, &f, &l, &err)
                  && f == -1 && l == -1 && dest.rowCountQml() == before,
              "pristine-empty source is a success no-op");
        blank.closeDocument();
    }
    src.closeDocument();
    dest.closeDocument();
}

// --- Test 41: tab-merge assets, package sources, width migration, edges ----
// Asset DISPOSITION: rel .minnotes sources copy into the destination sidecar
// (collision-renamed, videos with their .qcview tree, identical same-name
// files reused without copy); absolute refs pass through. Package views
// splice entries out of the archive. Ink width-migrates between page
// measures. Gap 0, the blank-anchor fold, and save/reopen persistence.
static void testMergeAssetsAndEdges() {
    qInfo("[41] merge assets: disposition, collisions, .qcview, packages, width");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_merge"));
    dir.removeRecursively();
    QDir().mkpath(dir.filePath(QStringLiteral("s")));
    QDir().mkpath(dir.filePath(QStringLiteral("d")));
    const QString srcD = dir.filePath(QStringLiteral("s"));
    const QString destD = dir.filePath(QStringLiteral("d"));
    // saveAs copies the shared untitled-scratch .minnotes into the fixture
    // dirs (junk from prior suite runs included) — unique basenames keep the
    // collision/reuse assertions deterministic.
    const QString uniq = makeUlid().right(6).toLower();
    const QString picN = QStringLiteral("mp-") + uniq + QStringLiteral(".png");
    const QString vidN = QStringLiteral("mv-") + uniq + QStringLiteral(".mp4");
    const QString pic2 = QStringLiteral("mp-") + uniq + QStringLiteral("-2.png");
    const QString pic3 = QStringLiteral("mp-") + uniq + QStringLiteral("-3.png");
    const QString extPic = dir.filePath(QStringLiteral("ext.png"));
    { QImage img(8, 8, QImage::Format_RGB32); img.fill(Qt::green);
      img.save(extPic, "PNG"); }

    BlockModel src;
    src.newDocument();
    while (src.rowCountQml() > 0) src.removeBlock(0);
    src.insertBlock(0); src.setContent(0, QStringLiteral("assets"));
    CHECK(src.saveAs(srcD + QStringLiteral("/src.mndb")), "source doc anchored");
    QDir().mkpath(srcD + QStringLiteral("/.minnotes/.qcview/") + vidN);
    { QImage img(12, 10, QImage::Format_RGB32); img.fill(Qt::yellow);
      img.save(srcD + QStringLiteral("/.minnotes/") + picN, "PNG"); }
    { QFile f(srcD + QStringLiteral("/.minnotes/") + vidN);
      f.open(QIODevice::WriteOnly); f.write(QByteArray(256, 'V')); }
    { QFile f(srcD + QStringLiteral("/.minnotes/.qcview/") + vidN
              + QStringLiteral("/notes.json"));
      f.open(QIODevice::WriteOnly); f.write("{\"notes\":[]}"); }
    {
        BlockModel::BlockSpec img; img.type = BlockModel::Media;
        img.mediaJson = QStringLiteral("{\"src\":\".minnotes/%1\",\"w\":12,\"h\":10}").arg(picN);
        src.insertSpecs(0, {img}, false);                                  // row 1
        BlockModel::BlockSpec vid; vid.type = BlockModel::Media;
        vid.mediaJson = QStringLiteral(
            "{\"src\":\".minnotes/%1\",\"w\":320,\"h\":240,\"kind\":\"video\","
            "\"durMs\":1000,\"frames\":24,\"fps\":24}").arg(vidN);
        src.insertSpecs(1, {vid}, false);                                  // row 2
        BlockModel::BlockSpec ext; ext.type = BlockModel::Media;
        ext.mediaJson = QStringLiteral("{\"src\":\"%1\",\"w\":8,\"h\":8}").arg(extPic);
        src.insertSpecs(2, {ext}, false);                                  // row 3
    }
    const int tRow = src.insertTable(3, 2, 2);                             // row 4
    src.tableSetCellMedia(tRow, 0, 0,
        QStringLiteral("{\"src\":\".minnotes/%1\",\"w\":12,\"h\":10}").arg(picN));
    const int srcN = src.rowCountQml();

    BlockModel dest;
    dest.newDocument();
    while (dest.rowCountQml() > 0) dest.removeBlock(0);
    dest.insertBlock(0); dest.setContent(0, QStringLiteral("d"));
    CHECK(dest.saveAs(destD + QStringLiteral("/dest.mndb")), "dest doc anchored");
    QDir().mkpath(destD + QStringLiteral("/.minnotes"));
    { QFile f(destD + QStringLiteral("/.minnotes/") + picN);   // name collision
      f.open(QIODevice::WriteOnly); f.write(QByteArray(7, 'x')); }

    int f = -1, l = -1; QString err;
    CHECK(DocumentMerger::mergeDocuments(&src, &dest, dest.rowCountQml(), &f, &l, &err),
          "asset merge succeeded (%s)", qPrintable(err));
    CHECK(f == 1 && l == srcN && dest.rowCountQml() == 1 + srcN,
          "merged at the end (%d..%d)", f, l);
    CHECK(dest.contentForRow(2).contains(QStringLiteral(".minnotes/") + pic2),
          "collided image renamed -2 in the descriptor");
    {
        QFile a(srcD + QStringLiteral("/.minnotes/") + picN);
        QFile b(destD + QStringLiteral("/.minnotes/") + pic2);
        CHECK(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly)
                  && a.readAll() == b.readAll(), "copied image byte-exact");
    }
    CHECK(dest.contentForRow(3).contains(QStringLiteral(".minnotes/") + vidN)
              && QFileInfo::exists(destD + QStringLiteral("/.minnotes/") + vidN)
              && QFileInfo::exists(destD + QStringLiteral("/.minnotes/.qcview/")
                     + vidN + QStringLiteral("/notes.json")),
          "video copied with its .qcview sidecar tree");
    CHECK(dest.contentForRow(4).contains(extPic),
          "absolute (linked) ref passed through untouched");
    CHECK(dest.tableCellMedia(5, 0, 0).contains(QStringLiteral(".minnotes/") + pic2),
          "table cell descriptor followed the copy");

    // Package-view source: entries splice straight out of the archive; the
    // byte-identical same-name video is REUSED without a second copy.
    const QString pkg = dir.filePath(QStringLiteral("src.mnpkg"));
    CHECK(PackageExporter::packDocument(&src, pkg, true, &err),
          "source packed (%s)", qPrintable(err));
    {
        BlockModel pv;
        CHECK(pv.openDocument(pkg), "package view opens");
        const int before = dest.rowCountQml();
        CHECK(DocumentMerger::mergeDocuments(&pv, &dest, before, &f, &l, &err),
              "package merge succeeded (%s)", qPrintable(err));
        CHECK(dest.contentForRow(f + 1).contains(QStringLiteral(".minnotes/") + pic3)
                  && QFileInfo::exists(destD + QStringLiteral("/.minnotes/") + pic3),
              "package image spliced out under the -3 name");
        {
            QFile a(srcD + QStringLiteral("/.minnotes/") + picN);
            QFile b(destD + QStringLiteral("/.minnotes/") + pic3);
            CHECK(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly)
                      && a.readAll() == b.readAll(), "spliced image byte-exact");
        }
        CHECK(dest.contentForRow(f + 2).contains(QStringLiteral(".minnotes/") + vidN)
                  && !QFileInfo::exists(destD + QStringLiteral("/.minnotes/mv-")
                                        + uniq + QStringLiteral("-2.mp4")),
              "identical same-name video REUSED (no second copy)");
        pv.closeDocument();
    }

    // Save → reopen: the merged blocks + rewritten descriptors persist.
    CHECK(dest.save(), "destination saves");
    const int destCount = dest.rowCountQml();
    dest.closeDocument();
    {
        BlockModel m2;
        CHECK(m2.openDocument(destD + QStringLiteral("/dest.mndb")), "dest reopens");
        CHECK(m2.rowCountQml() == destCount
                  && m2.contentForRow(2).contains(QStringLiteral(".minnotes/") + pic2),
              "merged rows + rewritten descriptors persisted");
        m2.closeDocument();
    }
    src.closeDocument();

    // Width migration: right-margin ink on a 1600 page lands edge-anchored
    // on a 760 page — the exact migrateInkForWidth ground truth.
    {
        mn::DocInkAnchor a;
        a.space = mn::DocInkAnchor::Px;
        qcv::ActiveStroke st;
        st.tool = qcv::DrawingTool::Freehand;
        st.points = { QPointF(900.0, 10.0), QPointF(950.0, 40.0) };
        st.strokeWidth = 4;
        a.strokes.push_back(st);
        const QString wideInk = mn::docInkToJson(a);

        BlockModel wsrc;
        wsrc.newDocument();
        while (wsrc.rowCountQml() > 0) wsrc.removeBlock(0);
        wsrc.insertBlock(0); wsrc.setContent(0, QStringLiteral("wide"));
        wsrc.setPageWidth(1600);
        wsrc.setBlockInk(0, wideInk);
        BlockModel wdest;
        wdest.newDocument();
        while (wdest.rowCountQml() > 0) wdest.removeBlock(0);
        wdest.insertBlock(0); wdest.setContent(0, QStringLiteral("n"));

        CHECK(DocumentMerger::mergeDocuments(&wsrc, &wdest, 1, &f, &l, &err),
              "width merge succeeded (%s)", qPrintable(err));
        // Stroke ids regenerate on every serialization — compare the geometry
        // (the migration's actual output), not the JSON bytes.
        const QString expect = BlockModel::migrateInkForWidth(wideInk, 1600, 760);
        mn::DocInkAnchor got, exp;
        CHECK(!expect.isEmpty()
                  && mn::docInkFromJson(wdest.inkForRow(1), got)
                  && mn::docInkFromJson(expect, exp)
                  && got.strokes.size() == 1 && exp.strokes.size() == 1
                  && got.strokes[0].points == exp.strokes[0].points
                  && got.strokes[0].points[0] == QPointF(480.0, 10.0),
              "ink width-migrated 1600 -> 760 (edge affinity ground truth)");

        // Gap 0: the merge lands at the very top.
        CHECK(DocumentMerger::mergeDocuments(&wsrc, &wdest, 0, &f, &l, &err)
                  && f == 0 && wdest.contentForRow(0) == QStringLiteral("wide"),
              "gap 0 merges at the top of the document");
        wdest.closeDocument();

        // Blank-anchor consume fold: merging into a pristine doc replaces
        // the placeholder row instead of leaving a leading blank.
        BlockModel fdest;
        fdest.newDocument();
        const int hBefore = fdest.undoHistory().size();
        CHECK(DocumentMerger::mergeDocuments(&wsrc, &fdest, 1, &f, &l, &err)
                  && f == 0 && l == 0 && fdest.rowCountQml() == 1
                  && fdest.contentForRow(0) == QStringLiteral("wide"),
              "blank anchor consumed by the fold");
        CHECK(fdest.undoHistory().size() == hBefore + 1, "fold merge = ONE entry");
        fdest.undo();
        CHECK(fdest.rowCountQml() == 1 && fdest.contentForRow(0).isEmpty(),
              "undo restores the pristine blank row");
        fdest.closeDocument();
        wsrc.closeDocument();
    }
    if (qEnvironmentVariable("MN_MERGE_KEEP").isEmpty()) dir.removeRecursively();
}

// --- Test 42b: verbatim multi-line insert into a code block -----------------
// The paste-into-code path (doPaste's code branch) rides insertText: newlines
// and blank lines land verbatim, the line-count param recounts, and one undo
// removes the whole insert.
static void testCodeBlockInsert() {
    qInfo("[42b] code block: multi-line insertText verbatim + param recount");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
    m.makeCodeBlock(0, QStringLiteral("py"));
    m.setContent(0, QStringLiteral("def a():"));
    const QString pasted = QStringLiteral("\n    x = 1\n\n# not a heading\n\ty = 2");
    m.insertText(0, 8, pasted);
    CHECK(m.contentForRow(0) == QStringLiteral("def a():") + pasted,
          "newlines, blank lines, '#', and tabs land verbatim");
    CHECK(m.typeForRow(0) == BlockModel::Code && m.rowCountQml() == 1,
          "still ONE code block (no markdown/table hijack)");
    m.undo();
    CHECK(m.contentForRow(0) == QStringLiteral("def a():"),
          "one undo removes the whole paste");
    m.closeDocument();
}

// --- Test 42: table CELL images in md / DOCX / PDF exports ------------------
// Cell media must ride every exporter like block images do: md collects into
// .assets (absolute-path fallback when unreachable), DOCX embeds a drawing
// run in the cell, PDF paints the image in the cell.
static void testCellMediaExports() {
    qInfo("[42] exports: table cell images (md assets/fallback, DOCX, PDF)");
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/mn_cellexp"));
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    // A WIDE image (the video-poster shape) — wider than its PDF cell will
    // be, so the width-clamp/overlap regression (user PDF 2026-08-20) is in
    // play, in a many-column table with same-cell text + rows below it.
    const QString extPic = dir.filePath(QStringLiteral("cellpic.png"));
    { QImage img(1600, 900, QImage::Format_RGB32); img.fill(QColor(255, 0, 200));
      img.save(extPic, "PNG"); }

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("cells"));
    const int tRow = m.insertTable(0, 4, 6);
    m.tableSetCell(tRow, 0, 0, QStringLiteral("head"));
    m.tableSetCellMedia(tRow, 1, 0,
        QStringLiteral("{\"src\":\"%1\",\"w\":1600,\"h\":900}").arg(extPic));
    m.tableSetCell(tRow, 1, 0, QStringLiteral("caption under the shot"));
    m.tableSetCellMedia(tRow, 0, 1,   // SAME source again → must not duplicate
        QStringLiteral("{\"src\":\"%1\",\"w\":1600,\"h\":900}").arg(extPic));
    m.tableSetCell(tRow, 1, 1, QStringLiteral("txt"));
    m.tableSetCell(tRow, 2, 0, QStringLiteral("row below"));
    m.tableSetCell(tRow, 3, 0, QStringLiteral("last row"));
    Exporter ex;
    ex.setModel(&m);

    // Markdown: the cell image is collected into .assets like a block image.
    const QString mdPath = dir.filePath(QStringLiteral("out.md"));
    CHECK(ex.exportMarkdown(mdPath, false), "md exported");
    QString md;
    { QFile f(mdPath); f.open(QIODevice::ReadOnly); md = QString::fromUtf8(f.readAll()); }
    CHECK(md.contains(QStringLiteral("![](out.assets/cellpic.png)")),
          "cell image collected into .assets");
    CHECK(QFileInfo::exists(dir.filePath(QStringLiteral("out.assets/cellpic.png"))),
          ".assets copy exists");
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("out.assets/cellpic-2.png"))),
          "same source in two cells → ONE .assets copy (sink dedups by path)");
    CHECK(md.contains(QStringLiteral("| --- | --- |\n"))
              && !md.contains(QStringLiteral("| --- | --- | --- |")),
          "trailing empty columns dropped at export (6 authored -> 2 shipped)");

    // Markdown fallback: an unreachable source keeps an absolute link (the
    // block-image rule) instead of silently dropping the image.
    m.tableSetCellMedia(tRow, 1, 1,
        QStringLiteral("{\"src\":\"%1\",\"w\":8,\"h\":8}")
            .arg(dir.filePath(QStringLiteral("missing dir/gone.png"))));
    const QString md2Path = dir.filePath(QStringLiteral("out2.md"));
    CHECK(ex.exportMarkdown(md2Path, false), "md re-exported");
    QString md2;
    { QFile f(md2Path); f.open(QIODevice::ReadOnly); md2 = QString::fromUtf8(f.readAll()); }
    CHECK(md2.contains(QStringLiteral("![](file://"))
              && md2.contains(QStringLiteral("gone.png")),
          "unreachable cell image falls back to the absolute file URL");
    m.tableSetCellMedia(tRow, 1, 1, QString());
    m.tableSetCell(tRow, 1, 1, QStringLiteral("txt"));

    // HTML: authored widths past the page column → percentage shares +
    // fixed layout (text wraps inside instead of the table running off).
    m.tableSetColWidth(tRow, 0, 900);
    m.tableSetColWidth(tRow, 1, 300);
    const QString htmlPath = dir.filePath(QStringLiteral("out.html"));
    CHECK(ex.exportHtml(htmlPath, false), "html exported");
    {
        QFile f(htmlPath); f.open(QIODevice::ReadOnly);
        const QString html = QString::fromUtf8(f.readAll());
        CHECK(html.contains(QStringLiteral(
                  "table-layout:fixed;width:min(1200px,max(760px,calc(100vw - 152px)))"))
                  && html.contains(QStringLiteral("width:75.00%"))
                  && html.contains(QStringLiteral("width:25.00%")),
              "over-wide table: full width, viewport cap floored at the page");
        CHECK(html.contains(QStringLiteral("min-width:1000px")),
              "page never shrinks under the document width (ink stays aligned)");
        CHECK(html.contains(QStringLiteral(
                  "--sheetw:calc(min(1200px,max(760px,100vw - 152px)) + 240px)"))
                  && html.contains(QStringLiteral("--desk:#121211")),
              "dual-tone ground: sheet band follows the widest table");
        CHECK(html.contains(QStringLiteral("main td img"))
                  && !html.contains(QStringLiteral("mn-underlay")),
              "cell images join the lightbox; block-select stays removed");
    }
    m.tableSetColWidth(tRow, 0, 0);
    m.tableSetColWidth(tRow, 1, 0);

    // ufb deep links (2026-08-21, opt-in): a file-path-printing block gains
    // a <ufb:///os/…> autolink under its path only when the flag is set.
    {
        BlockModel::BlockSpec att; att.type = BlockModel::Media;
        att.mediaJson = QStringLiteral(
            "{\"src\":\"%1\",\"kind\":\"file\"}").arg(extPic);   // any real file
        m.insertSpecs(m.rowCountQml() - 1, {att}, false);
        const QString u1 = dir.filePath(QStringLiteral("ufb1.md"));
        const QString u2 = dir.filePath(QStringLiteral("ufb2.md"));
        CHECK(ex.exportMarkdown(u1, false) && ex.exportMarkdown(u2, false, true),
              "ufb-flag exports written");
        QFile f1(u1), f2(u2);
        f1.open(QIODevice::ReadOnly); f2.open(QIODevice::ReadOnly);
        const QString md1 = QString::fromUtf8(f1.readAll());
        const QString md2 = QString::fromUtf8(f2.readAll());
        CHECK(!md1.contains(QStringLiteral("ufb:///")),
              "default export carries NO ufb links");
        CHECK(md2.contains(QStringLiteral("<ufb:///mac/"))
                  && md2.contains(QStringLiteral("cellpic.png>")),
              "opt-in export carries the ufb autolink under the path");
        m.removeBlock(m.rowCountQml() - 1);   // the attachment is the LAST row
    }

    // DOCX: a drawing run lands inside the table cell; media part shipped.
    const QString docx = dir.filePath(QStringLiteral("out.docx"));
    CHECK(ex.exportDocx(docx, false), "docx exported");
    const QByteArray docXml = mnpkg::readEntry(docx, QStringLiteral("word/document.xml"));
    const int tblAt = docXml.indexOf("<w:tbl>");
    const int tblEnd = docXml.indexOf("</w:tbl>");
    CHECK(tblAt >= 0 && tblEnd > tblAt
              && docXml.mid(tblAt, tblEnd - tblAt).contains("<w:drawing>"),
          "DOCX cell carries a drawing run inside w:tbl");
    CHECK(!mnpkg::readEntry(docx, QStringLiteral("word/media/image1.png")).isEmpty(),
          "DOCX media part shipped");
    CHECK(docXml.count("<w:gridCol") == 2,
          "DOCX grid also drops the trailing empty columns");

    // PDF: the magenta cell image paints (scan page 1 for the fixture color).
    const QString pdf = dir.filePath(QStringLiteral("out.pdf"));
    CHECK(ex.exportPdf(pdf, false), "pdf exported");
    {
        QPdfDocument pd;
        CHECK(pd.load(pdf) == QPdfDocument::Error::None && pd.pageCount() >= 1,
              "pdf loads");
        const QImage page = pd.render(0, QSize(1000, 1414));
        int hits = 0;
        for (int y = 0; y < page.height() && hits < 20; ++y)
            for (int x = 0; x < page.width() && hits < 20; ++x) {
                const QColor px = page.pixelColor(x, y);
                if (px.red() > 200 && px.green() < 80 && px.blue() > 120) ++hits;
            }
        CHECK(hits >= 20, "cell image pixels present on the PDF page");
    }
    m.closeDocument();
    if (qEnvironmentVariable("MN_CELL_KEEP").isEmpty()) dir.removeRecursively();
}

// --- Test 43: table bulk ops over selection sets ---------------------------
// Every bulk invokable = ONE mutateTable lambda = ONE undo entry; index
// lists arrive unordered with dupes; deletes run descending; clear = contents
// only (colours stay); TSV/Html readers emit ascending order.
static void testTableBulkOps() {
    qInfo("[43] table bulk ops: sets, one entry each, descending deletes");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("anchor"));
    const int t = m.insertTable(0, 5, 3);
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 3; ++c)
            m.tableSetCell(t, r, c, QStringLiteral("r%1c%2").arg(r).arg(c));

    // NOTE on entry counting: undoHistory() is the ACTIVE PATH of the undo
    // tree — pushing after an undo collapses the redoable tail, so the size
    // is only a push-counter at a clean leaf. Count once there; every other
    // section proves the real guarantee semantically (ONE undo restores all).

    // Readers first (pristine 5x3): ascending order, column subsets.
    CHECK(m.tableRowsTSV(t, {4, 0}) ==
              QStringLiteral("r0c0\tr0c1\tr0c2\nr4c0\tr4c1\tr4c2"),
          "rowsTSV ascending");
    CHECK(m.tableColsTSV(t, {2, 0}).startsWith(QStringLiteral("r0c0\tr0c2\n")),
          "colsTSV column subset per row");
    CHECK(m.tableRowsHtml(t, {1}).contains(QStringLiteral("<td>r1c1</td>"))
              && m.tableColsHtml(t, {1}).contains(QStringLiteral("<td>r3c1</td>")),
          "html readers emit the members");

    // Rows colour: the one clean-leaf entry count + members only.
    {
        const int entries = m.undoHistory().size();
        m.tableSetRowsColor(t, {1, 3}, /*fg*/false, QStringLiteral("#224466"));
        CHECK(m.undoHistory().size() == entries + 1
                  && m.tableRowBg(t, 1) == QStringLiteral("#224466")
                  && m.tableRowBg(t, 3) == QStringLiteral("#224466")
                  && m.tableRowBg(t, 2).isEmpty(),
              "rows colour: one entry, members only");
        m.undo();
        CHECK(m.tableRowBg(t, 1).isEmpty(), "one undo clears both rows");
    }
    // Cols align + cols kind: members only, one undo reverts each fully.
    {
        m.tableSetColsAlign(t, {0, 2}, 2);
        CHECK(m.tableColAlign(t, 0) == 2 && m.tableColAlign(t, 2) == 2
                  && m.tableColAlign(t, 1) == 0,
              "cols align: members only");
        m.undo();
        CHECK(m.tableColAlign(t, 0) == 0 && m.tableColAlign(t, 2) == 0,
              "one undo reverts both aligns");
        m.tableSetColumnsKind(t, {1, 2}, 2);
        CHECK(m.tableColumnKind(t, 1) == 2 && m.tableColumnKind(t, 2) == 2
                  && m.tableColumnKind(t, 0) == 0
                  && m.tableCell(t, 1, 0) == QStringLiteral("r1c0"),
              "cols kind: members converted, text column's cells untouched");
        m.undo();
        CHECK(m.tableColumnKind(t, 1) == 0 && m.tableColumnKind(t, 2) == 0,
              "one undo reverts both kinds");
    }
    // Clear rows: contents (text/media/choice) go, colours stay.
    {
        m.tableSetCellColor(t, 1, 1, 1, 1, /*fg*/false, QStringLiteral("#aa3355"));
        m.tableSetCellMedia(t, 2, 0,
            QStringLiteral("{\"src\":\"/tmp/none.png\",\"w\":4,\"h\":4}"));
        const QString withFixtures = m.contentForRow(t);
        m.tableClearRows(t, {1, 2});
        CHECK(m.tableCell(t, 1, 1).isEmpty() && m.tableCell(t, 2, 0).isEmpty()
                  && m.tableCellMedia(t, 2, 0).isEmpty()
                  && m.tableCellBg(t, 1, 1) == QStringLiteral("#aa3355")
                  && m.tableCell(t, 0, 0) == QStringLiteral("r0c0"),
              "clearRows: contents cleared, colour stays, non-members untouched");
        m.undo();
        CHECK(m.contentForRow(t) == withFixtures,
              "one undo restores every cleared cell");
    }
    // Delete a disjoint, UNORDERED, duped set → survivors keep identity.
    {
        const QString pre = m.contentForRow(t);
        m.tableDeleteRows(t, {3, 1, 3});
        CHECK(m.tableRows(t) == 3
                  && m.tableCell(t, 0, 0) == QStringLiteral("r0c0")
                  && m.tableCell(t, 1, 0) == QStringLiteral("r2c0")
                  && m.tableCell(t, 2, 0) == QStringLiteral("r4c0"),
              "descending delete: survivors keep identity");
        m.undo();
        CHECK(m.contentForRow(t) == pre, "one undo restores the whole delete");
    }
    // Column floor: deleting every column leaves one.
    {
        m.tableDeleteColumns(t, {0, 1, 2});
        CHECK(m.tableColumns(t) == 1, "delete-all-columns floors at 1");
        m.undo();
        CHECK(m.tableColumns(t) == 3, "undo restores the columns");
    }
    // Media resize nudge-runs coalesce (2026-08-21): consecutive resizes of
    // the same target = ONE entry labeled "Resize"; undo returns to the
    // pre-run size in one step.
    {
        BlockModel::BlockSpec img; img.type = BlockModel::Media;
        img.mediaJson = QStringLiteral("{\"src\":\"/tmp/none.png\",\"w\":40,\"h\":30}");
        m.insertSpecs(0, {img}, false);
        const int mr = 1;
        const double h0 = m.heightForRow(mr);
        const int entries = m.undoHistory().size();
        m.setMediaWidth(mr, 300);
        m.setMediaWidth(mr, 400);
        m.setMediaWidth(mr, 500);
        CHECK(m.undoHistory().size() == entries + 1
                  && m.contentForRow(mr).contains(QStringLiteral("\"dw\":500")),
              "three block-image resizes coalesce into ONE entry");
        CHECK(m.undoHistory().last().toMap().value("label").toString()
                  == QStringLiteral("Resize"),
              "the run is labeled Resize in the history");
        const double h500 = m.heightForRow(mr);
        CHECK(h500 > h0 + 1, "the resize grew the row (estimate-derived)");
        m.undo();
        CHECK(!m.contentForRow(mr).contains(QStringLiteral("\"dw\"")),
              "one undo returns to the pre-run (intrinsic) size");
        // Media heights are estimate-authoritative (never measured back) —
        // the undo must RE-DERIVE the height, not seed the stale one
        // (user-caught 2026-08-21).
        CHECK(std::abs(m.heightForRow(mr) - h0) < 1.0,
              "undo re-derives the media row height (%.0f -> %.0f)",
              h500, m.heightForRow(mr));
        m.redo();
        CHECK(std::abs(m.heightForRow(mr) - h500) < 1.0,
              "redo re-derives it forward again");
        m.undo();
        // Cell image: same rule through mutateTable's coalesce.
        const int t2 = t + 1;   // the table shifted down by the inserted media row
        m.tableSetCellMedia(t2, 1, 1,
            QStringLiteral("{\"src\":\"/tmp/none.png\",\"w\":40,\"h\":30}"));
        const int entries2 = m.undoHistory().size();
        m.tableSetCellImageWidth(t2, 1, 1, 200);
        m.tableSetCellImageWidth(t2, 1, 1, 260);
        CHECK(m.undoHistory().size() == entries2 + 1
                  && m.tableCellMedia(t2, 1, 1).contains(QStringLiteral("\"dw\":260")),
              "cell-image resizes coalesce too");
    }
    m.closeDocument();
}

// --- Test 44: choice chips inside table TEXT cells ---------------------------
// The DT-2 chip in a cell's span list: same text==label invariant, same
// payload; ops are one mutateTable each; typed (choice/check) BODY cells
// refuse while typed columns' header cells (still text) accept.
static void testCellChoiceChips() {
    qInfo("[44] cell choice chips: insert/select/options/remove in cell spans");
    auto payloadOf = [](const QString& j) {
        return QJsonDocument::fromJson(j.toUtf8()).object();
    };
    auto idFor = [&](const QString& j, const QString& label) {
        for (const QJsonValue& v : payloadOf(j).value(QStringLiteral("o")).toArray())
            if (v.toObject().value(QStringLiteral("l")).toString() == label)
                return v.toObject().value(QStringLiteral("id")).toString();
        return QString();
    };

    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0); m.setContent(0, QStringLiteral("anchor"));
    const int t = m.insertTable(0, 3, 2);
    m.tableSetCell(t, 1, 0, QStringLiteral("status: here"));

    const int s = m.tableInsertChoiceAt(t, 1, 0, 8);
    CHECK(s == 8, "cell chip inserted at the caret (start=%d)", s);
    CHECK(m.tableCell(t, 1, 0) == QStringLiteral("status: To dohere"),
          "default label spliced into the cell text");
    const QString pj = m.tableChoiceAt(t, 1, 0, 9);
    CHECK(!pj.isEmpty()
              && payloadOf(pj).value(QStringLiteral("o")).toArray().size() == 3
              && payloadOf(pj).value(QStringLiteral("v")).toString()
                     == idFor(pj, QStringLiteral("To do")),
          "payload carries the tri-state set, To do selected");
    CHECK(m.tableChoiceRangeAt(t, 1, 0, 9) == (QVariantList{8, 13}),
          "range covers the label");

    // A neighbour cell span must shift when the label swaps.
    m.tableSetCellFormat(t, 1, 0, 13, 17, QStringLiteral("bold"), true);   // "here"
    m.tableSetChoiceSelected(t, 1, 0, 8, idFor(pj, QStringLiteral("Done")));
    CHECK(m.tableCell(t, 1, 0) == QStringLiteral("status: Donehere"),
          "selecting swaps the label text");
    CHECK(m.tableCellHasFormat(t, 1, 0, 12, 16, QStringLiteral("bold")),
          "neighbour cell span shifted with the swap");
    CHECK(m.tableChoiceRangesForCell(t, 1, 0).first().toMap()
              .value(QStringLiteral("color")).toString()
              == QStringLiteral("#58A65C"),
          "overlay feed reports the selected option's color");
    m.undo();
    CHECK(m.tableCell(t, 1, 0) == QStringLiteral("status: To dohere")
              && m.tableCellHasFormat(t, 1, 0, 13, 17, QStringLiteral("bold")),
          "one undo restores text, payload AND the neighbour");
    m.redo();

    // Quick-add selects, labels sanitize; options commit falls back to first
    // when the selected option is deleted (the block-chip rules verbatim).
    const QString nid = m.tableChoiceAddOption(t, 1, 0, 8,
        QStringLiteral("Blo*ck*ed"), QStringLiteral("#FF0000"));
    CHECK(!nid.isEmpty()
              && m.tableCell(t, 1, 0) == QStringLiteral("status: Blockedhere"),
          "quick-add selects; label sanitized");
    {
        const QString cur = m.tableChoiceAt(t, 1, 0, 9);
        QVariantList arr;
        for (const QJsonValue& v : payloadOf(cur).value(QStringLiteral("o")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toString() == nid) continue;   // delete it
            QVariantMap e;
            e.insert(QStringLiteral("id"), o.value(QStringLiteral("id")).toString());
            e.insert(QStringLiteral("label"), o.value(QStringLiteral("l")).toString());
            e.insert(QStringLiteral("color"), o.value(QStringLiteral("c")).toString());
            arr.append(e);
        }
        m.tableSetChoiceOptions(t, 1, 0, 8, arr);
        CHECK(m.tableCell(t, 1, 0) == QStringLiteral("status: To dohere"),
              "deleted-selected falls back to the first option");
    }

    // Remove: ONE clean entry; the label text dies with the span.
    {
        const int entries = m.undoHistory().size();
        m.tableRemoveChoiceAt(t, 1, 0, 8);
        CHECK(m.undoHistory().size() == entries + 1
                  && m.tableCell(t, 1, 0) == QStringLiteral("status: here")
                  && m.tableChoiceRangesForCell(t, 1, 0).isEmpty(),
              "remove: one entry, chip and label gone");
        m.undo();
        CHECK(m.tableCell(t, 1, 0) == QStringLiteral("status: To dohere")
                  && !m.tableChoiceRangesForCell(t, 1, 0).isEmpty(),
              "one undo revives the chip");
    }

    // Typed columns: body cells refuse (they render a widget); their header
    // cells are still text and accept.
    m.tableSetColumnsKind(t, {1}, 1);
    CHECK(m.tableInsertChoiceAt(t, 1, 1, 0) == -1,
          "choice-column body cell refuses a chip");
    CHECK(m.tableInsertChoiceAt(t, 0, 1, 0) >= 0,
          "typed column's header cell still takes one");
    m.closeDocument();
}

// --- Test 48: exports carry chips + cell spans; typed headers stay text ----
// The 2026-08-22 gap-close: inline/cell chips render in HTML (the .chip
// pill) and shade in DOCX/PDF; cell spans reach every emitter; typed
// (check/choice) columns render their HEADER cells as text (the app's
// isCheck/isChoice = !isHeader rule); the DOCX task glyph is a painted
// image whose docPr name ("mnTask<state>") round-trips the state without
// importing the raster as media.
static void testExportChipsTasksCells() {
    qInfo("[48] exports: chips, cell spans, typed-column header text");
    BlockModel m;
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);

    //                                                 0123456789
    m.insertBlock(0); m.setContent(0, QStringLiteral("status is  today"));
    const int cs = m.insertChoiceAt(0, 10);            // between the spaces
    CHECK(cs == 10, "inline chip inserted at col 10");
    m.choiceAddOption(0, cs, QStringLiteral("Urgent"), QStringLiteral("#804000"));

    m.insertBlock(1); m.setContent(1, QStringLiteral("doing thing"));
    m.setBlockType(1, BlockModel::TaskListItem);
    m.toggleTask(1);                                   // todo → doing

    m.insertTable(1, 3, 3);                            // → row 2
    const int t = 2;
    m.tableSetHeaderRows(t, 1);
    m.tableSetColumnKind(t, 0, 2);                     // check
    m.tableSetColumnKind(t, 1, 1);                     // choice
    m.tableSetCell(t, 0, 0, QStringLiteral("Done?"));
    m.tableSetCell(t, 0, 1, QStringLiteral("Status"));
    m.tableSetCell(t, 0, 2, QStringLiteral("Notes"));
    m.tableSetCellCheck(t, 1, 0, 1);                   // doing
    const QString oid = m.tableAddOption(t, 1, QStringLiteral("Ship"),
                                         QStringLiteral("#804000"));
    m.tableSetCellChoice(t, 1, 1, oid);
    m.tableSetCell(t, 1, 2, QStringLiteral("bold note"));
    m.tableSetCellFormat(t, 1, 2, 0, 4, QStringLiteral("bold"), true);
    const int ccs = m.tableInsertChoiceAt(t, 2, 2, 0); // cell chip, plain col
    CHECK(ccs == 0, "cell chip inserted in the plain column");
    m.tableChoiceAddOption(t, 2, 2, ccs, QStringLiteral("Blocked"),
                           QStringLiteral("#005080"));

    Exporter ex;
    ex.setModel(&m);

    // Markdown: chips flatten to their labels; cell bold carries; typed
    // headers are the header TEXT, not a glyph/label.
    RecordingSink msink;
    const QString md = ex.toMarkdown(Exporter::Options{}, msink);
    CHECK(md.contains(QStringLiteral("status is Urgent today")),
          "md: inline chip flattens to the selected label");
    CHECK(md.contains(QStringLiteral("| Done? | Status | Notes |")),
          "md: typed-column header cells stay text");
    CHECK(md.contains(QStringLiteral("| [/] | Ship | **bold** note |")),
          "md: check state + choice label + bold cell span");
    CHECK(md.contains(QStringLiteral("Blocked")),
          "md: cell chip flattens to its label");

    // HTML: chips are .chip pills with the option color at 0.28 alpha —
    // inline, in choice columns, and inside plain cells.
    RecordingSink hsink;
    const QString html = ex.toHtml(Exporter::Options{}, hsink);
    CHECK(html.contains(QStringLiteral(
              "<span class=\"chip\" style=\"background:rgba(128,64,0,0.28)\">Urgent</span>")),
          "html: inline chip renders as the colored pill");
    CHECK(html.contains(QStringLiteral(
              "<span class=\"chip\" style=\"background:rgba(128,64,0,0.28)\">Ship</span>")),
          "html: choice-column chip keeps its pill");
    CHECK(html.contains(QStringLiteral(
              "<span class=\"chip\" style=\"background:rgba(0,80,128,0.28)\">Blocked</span>")),
          "html: cell chip renders as a pill too");
    CHECK(html.contains(QStringLiteral("<td>Done?</td>")),
          "html: check column's header cell is its text");
    CHECK(html.contains(QStringLiteral("class=\"cb doing\"")),
          "html: body check cell renders the tri-state glyph");
    CHECK(html.contains(QStringLiteral("<strong>bold</strong> note")),
          "html: cell bold span carries");

    // DOCX: painted task glyphs (docPr-tagged, no Unicode fallback), chip
    // shading on choice cells, cell spans as real runs.
    const QString docxPath = QDir::temp().filePath(QStringLiteral("mn_chip_test.docx"));
    QFile::remove(docxPath);
    CHECK(ex.exportDocx(docxPath, true), "exportDocx wrote the file");
    {
        QZipReader zr(docxPath);
        const QByteArray doc = zr.fileData(QStringLiteral("word/document.xml"));
        CHECK(doc.contains("mnTask1"), "docx: task glyphs are docPr-tagged images");
        CHECK(!doc.contains("\xE2\x97\x90") && !doc.contains("\xE2\x98\x90"),
              "docx: no Unicode ◐/☐ glyph runs remain");
        CHECK(doc.contains("Done?"), "docx: check column's header cell is its text");
        // #804000 chip over paper = 0.28·color + 0.72·white = DBCAB8.
        CHECK(doc.contains("w:fill=\"DBCAB8\""),
              "docx: choice cell + inline chip shade with the paper blend");
        CHECK(doc.contains("<w:b/>"), "docx: cell bold span emits a real run");
    }
    // Round-trip: the painted glyph must re-import as task STATE, never as
    // a media block.
    {
        BlockModel m2;
        m2.newDocument();
        while (m2.rowCountQml() > 0) m2.removeBlock(0);
        m2.insertBlock(0);
        CHECK(Importer::importDocxFile(docxPath, &m2), "docx re-imported");
        int taskRow = -1, mediaRows = 0;
        for (int r = 0; r < m2.rowCountQml(); ++r) {
            if (m2.typeForRow(r) == BlockModel::TaskListItem) taskRow = r;
            if (m2.typeForRow(r) == BlockModel::Media) ++mediaRows;
        }
        CHECK(taskRow >= 0 && m2.taskStateForRow(taskRow) == BlockModel::TaskDoing
                  && m2.contentForRow(taskRow) == QStringLiteral("doing thing"),
              "docx round-trip: docPr sniff restores the task, spacer stripped");
        CHECK(mediaRows == 0, "docx round-trip: glyph rasters never become media");
        m2.closeDocument();
    }
    QFile::remove(docxPath);

    // PDF: the doc (chips + typed columns + cell spans) renders and writes.
    const QString pdfPath = QDir::temp().filePath(QStringLiteral("mn_chip_test.pdf"));
    QFile::remove(pdfPath);
    CHECK(ex.exportPdf(pdfPath, true), "exportPdf wrote the file");
    CHECK(QFileInfo(pdfPath).size() > 0, "pdf is non-empty");
    QFile::remove(pdfPath);
    m.closeDocument();
}

int main(int argc, char** argv) {
    // Uses the native platform (the test creates no windows). QGuiApplication —
    // not QCoreApplication — because BlockModel/MediaStore touch QImage/QPixmap.
    QGuiApplication app(argc, argv);
    app.setApplicationName("minNotes");
    app.setOrganizationName("minNotes");
    if (!qEnvironmentVariable("MN_OPEN_PROBE").isEmpty())
        return runOpenProbe(qEnvironmentVariable("MN_OPEN_PROBE"));
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
    testPageWidth();
    testUndoHistory();
    testComments();
    testExportMarkdown();
    testSketchResizeRenorm();
    testSketchFitToInk();
    testSketchExportCaps();
    testSketchEmbedWidth();
    testSketchTextElements();
    testDocInkTexts();
    testConsumeEmptyAnchor();
    testInsertSpecs();
    testImporterWalker();
    testImportFileCores();
    testPackageFormat();
    testPackageExporter();
    testPackageLifecycle();
    testAsyncPackagePaths();
    testPackageStreaming();
    testEnexImport();
    testNotionImport();
    testDocxRoundTrip();
    testRtfImport();
    testImportCopyPolicy();
    testCollectMedia();
    testDocInkGroupMove();
    testSketchGroupDelete();
    testPdfPageInkUndo();
    testPdfPageInkExport();
    testExportPdf();
    testInlineChoice();
    testUndoHeightSeeding();
    testNewImportFormats();
    testMergeEngine();
    testMergeAssetsAndEdges();
    testCodeBlockInsert();
    testCellMediaExports();
    testTableBulkOps();
    testCellChoiceChips();
    testExportChipsTasksCells();

    if (g_fail == 0) qInfo("=== ALL CHECKS PASSED ===");
    else             qCritical("=== %d CHECK(S) FAILED ===", g_fail);
    return g_fail;
}

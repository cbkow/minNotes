// Headless regression harness for the document-state fixes made during the
// Windows port (run: mn_regression_test). Drives BlockModel directly — the layer
// the fixes live in — so the checks are deterministic (no flaky GUI clicking).
// setMeasuredHeight() is the same seam the QML delegate uses to report a laid-out
// height, so we can reproduce the undo/redo height-preservation bug without a view.
//
// Built only when configured with -DMINNOTES_BUILD_TESTS=ON. Exit code = number
// of failed checks (0 = all pass).
#include "BlockModel.h"

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QtGlobal>

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

int main(int argc, char** argv) {
    // Uses the native platform (the test creates no windows). QGuiApplication —
    // not QCoreApplication — because BlockModel/MediaStore touch QImage/QPixmap.
    QGuiApplication app(argc, argv);
    app.setApplicationName("minNotes");
    app.setOrganizationName("minNotes");

    qInfo("=== minNotes regression pass ===");
    testCountNotify();
    testCommitMarkdown();
    testUndoRedoHeights();
    testSaveReopen();
    testUndoBranchCoalesce();
    testCanonicalizeAndStamp();
    testListsAndDepth();

    if (g_fail == 0) qInfo("=== ALL CHECKS PASSED ===");
    else             qCritical("=== %d CHECK(S) FAILED ===", g_fail);
    return g_fail;
}

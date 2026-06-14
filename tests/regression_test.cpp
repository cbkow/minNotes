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

    if (g_fail == 0) qInfo("=== ALL CHECKS PASSED ===");
    else             qCritical("=== %d CHECK(S) FAILED ===", g_fail);
    return g_fail;
}

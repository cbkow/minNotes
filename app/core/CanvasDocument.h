#pragma once
#include <QHash>
#include <QObject>
#include <QRectF>
#include <QStringList>
#include <QVariantList>
#include <optional>
#include <memory>
#include <vector>
#include "Converter.h"
#include "Document.h"

class BlockModel;
class MediaStore;

// The FILE (2.0 stage 2). A minNotes file is a canvas: a SQLite store holding page ELEMENTS
// (and, later, media / ink / shape / connector elements) plus every page's blocks, partitioned
// by `blocks.page_id`. CanvasDocument owns what used to be BlockModel's file half — the local
// working copy, the package extraction, the identity path, the conflict baseline, dirty + save
// state, the ONE MediaStore every page shares — and the pages: a BlockModel per page, created
// lazily on a page-scoped handle of the shared connection (Document::scoped). The QML-facing
// property names are the ones the tab strip already reads (documentName / untitled / dirty …).
//
// A page model may also OWN its canvas (BlockModel::newDocument / openDocument build one and
// attach to its first page) so every 1.0 call site — QML, tests, importers — keeps working with
// the forwarders in BlockModel; the DocumentManager inverts that ownership in a later step.
class CanvasDocument : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString documentPath READ documentPath NOTIFY documentChanged)
    Q_PROPERTY(QString documentName READ documentName NOTIFY documentChanged)
    Q_PROPERTY(bool untitled READ untitled NOTIFY documentChanged)
    Q_PROPERTY(bool documentOpen READ documentOpen NOTIFY documentChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY documentChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(int saveState READ saveState NOTIFY saveStateChanged)
    Q_PROPERTY(QString focusPage READ focusPage WRITE setFocusPage NOTIFY focusChanged)
    Q_PROPERTY(QStringList pageIds READ pageIds NOTIFY pagesChanged)
    Q_PROPERTY(int pagesRevision READ pagesRevision NOTIFY pagesChanged)
    // The canvas undo log (the second log; ⌘Z routes by focus). Same contract as BlockModel's.
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStackChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStackChanged)
    Q_PROPERTY(int undoRevision READ undoRevision NOTIFY undoStackChanged)
    // Element revisions: the SET (add / remove / z / json) · GEOMETRY (rects) · INK (ink elements and
    // media elements' frame ink). Culling keys on elementsRevision + geometryRevision.
    Q_PROPERTY(int elementsRevision READ elementsRevision NOTIFY elementsChanged)
    Q_PROPERTY(int geometryRevision READ geometryRevision NOTIFY geometryChanged)
    Q_PROPERTY(int inkRevision READ inkRevision NOTIFY inkChanged)
public:
    enum SaveState { SaveClean = 0, SaveSaving = 1, SaveFailed = 2, SaveConflict = 3 };
    Q_ENUM(SaveState)
    // A page element's rect is its content measure plus this inset each side (== BlockModel::kCardInset).
    static constexpr double kPageInset = 24.0;
    static constexpr double kPageGap = 64.0;       // new pages land this far right of the others
    static constexpr int kDefaultMeasure = 760;    // a new page's canvas measure

    explicit CanvasDocument(QObject* parent = nullptr);
    ~CanvasDocument() override;

    // App-local working-copy directory for THIS session (main() points it at a lockfile-guarded
    // per-session subdir so concurrent instances never delete each other's live copies).
    static QString scratchDir();
    static void setScratchRoot(const QString& dir);

    // --- The file ---
    bool newCanvas();                               // fresh untitled scratch with ONE page, focused
    bool open(const QString& pathOrUrl);            // .mnd (2.0; a 1.0 file opens read-only) or .mnpkg
    void closeCanvas();                             // detach every page, discard the working copy
    QString lastOpenError() const { return lastOpenError_; }
    Q_INVOKABLE bool save();                        // working copy → original; false if untitled/read-only/conflict/failed
    Q_INVOKABLE bool overwriteSave();               // save ignoring an external-change conflict
    Q_INVOKABLE bool saveAs(const QString& pathOrUrl);
    bool snapshotTo(const QString& path);           // clean stamped copy (the packaging source)

    QString documentPath() const { return docPath_; }
    QString documentName() const;
    bool untitled() const { return untitled_; }
    bool documentOpen() const { return db_.isOpen(); }
    bool readOnly() const { return readOnly_; }     // a converted 1.0 file: Save routes to Save As
    // What the 1.0 → 2.0 converter did on this open (converted == false for a 2.0 file).
    const mn::ConversionReport& conversionReport() const { return report_; }
    bool dirty() const { return dirty_; }
    int saveState() const { return saveState_; }
    void markDirty();                               // idempotent; pages call it on every edit
    // Where this file's sidecar media lives: the package extraction dir for .mnpkg, else the
    // original's folder.
    QString mediaAnchorDir() const;
    MediaStore* mediaStore() const { return mediaStore_.get(); }

    // --- Pages ---
    QStringList pageIds() const;                    // page elements in z order, then creation
    int pagesRevision() const { return pagesRevision_; }
    QString firstPageId() const;
    Q_INVOKABLE QString addPage();                  // one empty paragraph, right of the others; "" if closed
    Q_INVOKABLE bool removePage(const QString& id); // refuses the last page; the page's blocks cascade
    Q_INVOKABLE QString pageTitle(const QString& id) const;   // first heading, else first text line
    // The page's model, created on first use (owned by the canvas). nullptr for a non-page id.
    BlockModel* pageModel(const QString& id);
    BlockModel* pageModelIfLoaded(const QString& id) const { return pages_.value(id, nullptr); }
    // Registration used by BlockModel::attachPage / detachPage (a model attaching itself).
    void adoptPageModel(BlockModel* m, const QString& id);
    void forgetPageModel(BlockModel* m);
    // The focused page ("" = the canvas itself is in view). focusedModel() never returns null
    // while the file is open: it falls back to the first page.
    QString focusPage() const { return focusPage_; }
    void setFocusPage(const QString& id);
    QString focusedPageId() const;
    BlockModel* focusedModel();

    // --- Elements (read): a z-ordered in-memory mirror of the `elements` table ---
    const std::vector<Document::Element>& elements() const { return elements_; }
    std::optional<Document::Element> element(const QString& id) const;
    Q_INVOKABLE int elementCount() const { return static_cast<int>(elements_.size()); }
    Q_INVOKABLE QString elementIdAt(int i) const;
    Q_INVOKABLE QString elementKind(const QString& id) const;
    Q_INVOKABLE QRectF elementRect(const QString& id) const;
    Q_INVOKABLE QString elementJson(const QString& id) const;
    Q_INVOKABLE int elementZ(const QString& id) const;
    Q_INVOKABLE QStringList visibleElements(qreal x0, qreal y0, qreal x1, qreal y1) const;   // z order
    Q_INVOKABLE QRectF canvasBounds() const;                                                // union of rects
    Q_INVOKABLE QString mediaElementInk(const QString& id, int page) const;                 // "" = none

    // --- Element mutators. Each is ONE undo entry; a non-empty `coalesce` key merges a live run
    // (a drag: "move:<id>", "resize:<id>", "ink:<id>") into the current entry, like typing. ---
    Q_INVOKABLE QString addMediaElement(const QString& mediaDescriptorJson, const QRectF& rect);  // {media:{…}}
    Q_INVOKABLE QString addInkElement(const QString& inkJson, const QRectF& rect);                // element-local strokes
    Q_INVOKABLE QString addShapeElement(const QString& shapeJson, const QRectF& rect);
    Q_INVOKABLE QString addConnector(const QString& connectorJson);                               // endpoints resolve at render
    Q_INVOKABLE bool setElementRect(const QString& id, const QRectF& rect, const QString& coalesce = QString());
    Q_INVOKABLE bool setElementJson(const QString& id, const QString& json, const QString& coalesce = QString());
    Q_INVOKABLE bool setElementZ(const QString& id, int z);
    Q_INVOKABLE bool bringToFront(const QString& id);
    Q_INVOKABLE bool removeElement(const QString& id);            // a page follows removePage's rules
    Q_INVOKABLE bool setMediaElementInk(const QString& id, int page, const QString& envelope);   // "" clears
    // Pages are elements too: a page's canvas measure is its rect width less the insets. [400, 4000].
    static constexpr double kMinPageMeasure = 400.0;
    static constexpr double kMaxPageMeasure = 4000.0;

    // --- Canvas undo ---
    bool canUndo() const { return undoCur_ >= 0; }
    bool canRedo() const;
    int undoRevision() const { return undoRev_; }
    int elementsRevision() const { return elementsRevision_; }
    int geometryRevision() const { return geometryRevision_; }
    int inkRevision() const { return inkRevision_; }
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // The active path, oldest-first, {idx, label, ts, current, future} with an "Opened" baseline
    // at idx -1 (the shape the Inspector's History panel already renders).
    Q_INVOKABLE QVariantList undoHistory() const;
    Q_INVOKABLE void undoJumpTo(int target);
    void clearUndo();

    Document& db() { return db_; }
    const Document& db() const { return db_; }

signals:
    void documentChanged();
    void dirtyChanged();
    void saveStateChanged();
    void focusChanged();
    void pagesChanged();
    void undoStackChanged();
    void elementsChanged();
    void geometryChanged();
    void inkChanged();

private:
    bool loadFile(const QString& path, bool untitled);
    bool acceptOpenedFormat();   // the 2.0 gate: mnd2 ≤ v2 opens, mnd v1 adopts read-only, else refused
    void ensurePages();          // canvas schema present, ≥ 1 page element, no page-less rows
    void finishOpen();           // ensurePages + the 1.0 conversion when the file was adopted
    QString insertPageElement(); // a page element right of the others (no blocks yet)
    void seedPage(const QString& pageId);
    void reloadElements();       // refresh the mirror from the table
    // One undo entry = one element's state before and after. A page's snapshot on add / remove
    // carries its raw block rows, so undoing a removal puts the page back verbatim (same ids,
    // ranks, timestamps); the page's in-memory history is gone, as if its tab had been closed.
    struct ElementSnap {
        bool present = false;
        Document::Element el;
        std::vector<Document::RawBlock> blocks;
    };
    struct UndoEntry {
        QString id;
        ElementSnap before, after;
        QString coalesce;
        QString label;
        int parent = -1;
        qint64 ts = 0;
    };
    ElementSnap snapshot(const QString& id, bool withBlocks) const;
    void applySnap(const QString& id, const ElementSnap& s);   // the db + loaded models follow the snapshot
    void pushEntry(UndoEntry e, const QString& coalesce);
    QString addElement(const QString& kind, const QRectF& r, const QString& json, const QString& label);
    void noteChanged(const QString& kind, bool set, bool geometry, bool ink);
    void detachPageModel(const QString& id);
    int nextZ() const;
    bool writeBackToOriginal();
    static QString newScratchPath();
    void cleanupScratch();
    void recordOriginalStat();
    bool externalChangeDetected() const;
    void setSaveState(int s);
    void detachAllPages();

    Document db_;              // the ROOT handle; pages hold scoped copies on the same connection
    QString docPath_;          // canonical original (or the untitled identity path); media anchor + Save-As source
    QString scratchPath_;      // local working-copy DB the SQLite connection actually runs against
    QString pkgDir_;           // open .mnpkg: the per-document extraction dir (document.mnd + .minnotes/)
    QString lastOpenError_;
    bool untitled_ = true;
    bool readOnly_ = false;
    mn::ConversionReport report_;
    bool dirty_ = false;
    int saveState_ = SaveClean;
    qint64 origMtime_ = -1;    // original file mtime/size at load + each save (conflict baseline)
    qint64 origSize_ = -1;
    std::unique_ptr<MediaStore> mediaStore_;
    QHash<QString, BlockModel*> pages_;   // page id → its model (loaded pages only)
    QString focusPage_;
    int pagesRevision_ = 0;
    std::vector<Document::Element> elements_;   // z order, then creation (the table's order)
    QHash<QString, int> indexById_;
    std::vector<UndoEntry> undo_;
    int undoCur_ = -1;
    int undoRev_ = 0;
    int elementsRevision_ = 0, geometryRevision_ = 0, inkRevision_ = 0;
};

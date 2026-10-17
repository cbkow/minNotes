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
    // The file's camera (canvas_meta.camera on save): false until the user takes it.
    Q_PROPERTY(bool hasCamera READ hasCamera NOTIFY cameraChanged)
public:
    enum SaveState { SaveClean = 0, SaveSaving = 1, SaveFailed = 2, SaveConflict = 3 };
    Q_ENUM(SaveState)
    // A page element's rect is its content measure plus this inset each side (== BlockModel::kCardInset).
    static constexpr double kPageInset = 24.0;
    static constexpr double kPageGap = 64.0;       // new pages land this far right of the others
    static constexpr int kDefaultMeasure = 760;    // a new page's canvas measure
    // A page card is never shorter than this (world px): an empty page still reads as a page; a
    // page with more content grows past it (the cached height is floored here).
    static constexpr double kMinPageHeight = 400.0;
    // …and never taller than a PAGE LENGTH: measure × √2 (an A4 proportion; 1075 at 760). A longer
    // page shows its top on the canvas under a fade and a footer; the page view holds the rest
    // (ruling 2026-10-17: cap the card, never the document). The cache stores the capped height, so
    // bounds, fit, culling and the poster never see a 195,000 px card.
    static double pageHeightCap(double elementWidth);
    static double clampPageHeight(double elementWidth, double h);
    static constexpr double kDropMediaWidth = 480.0;   // a dropped image / video / PDF lands this wide

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
    Q_INVOKABLE QString firstPageId() const;
    Q_INVOKABLE QString addPage();                  // one empty paragraph, right of the others; "" if closed
    Q_INVOKABLE QString addPageAt(qreal wx, qreal wy);   // …at a world point (the canvas gesture)
    Q_INVOKABLE bool removePage(const QString& id); // refuses the last page; the page's blocks cascade
    // The page's title: first heading, else first text line, else "Untitled page". A LOADED page
    // answers from memory (the strip re-reads it per keystroke); an unloaded page's title comes
    // from the store once and is cached (it cannot change until the page loads).
    Q_INVOKABLE QString pageTitle(const QString& id) const;
    // Comment threads anchored in NO page (the Inspector's list on the canvas): the page models'
    // row shape {id, resolved, created, row: -1, excerpt: ""} so the same cards render them.
    Q_INVOKABLE QVariantList orphanThreads() const;
    // The page's model, created on first use (owned by the canvas). nullptr for a non-page id.
    Q_INVOKABLE BlockModel* pageModel(const QString& id);
    BlockModel* pageModelIfLoaded(const QString& id) const { return pages_.value(id, nullptr); }
    // Registration used by BlockModel::attachPage / detachPage (a model attaching itself).
    void adoptPageModel(BlockModel* m, const QString& id);
    void forgetPageModel(BlockModel* m);
    // The focused page ("" = the canvas itself is in view). focusedModel() never returns null
    // while the file is open: it falls back to the first page.
    QString focusPage() const { return focusPage_; }
    void setFocusPage(const QString& id);
    Q_INVOKABLE QString focusedPageId() const;
    BlockModel* focusedModel();
    // Every loaded page re-resolves its media (path mappings / a package extraction landed).
    void refreshMedia();
    // Collect Media: re-point media ELEMENTS whose resolved source is in the map (abs path → rel).
    // Direct writes, no undo entry (the page half is BlockModel::rewriteMediaSrcs). Returns the count.
    int rewriteMediaElementSrcs(const QHash<QString, QString>& absToRel);

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
    Q_INVOKABLE QList<int> visibleElementIndices(qreal x0, qreal y0, qreal x1, qreal y1) const;   // mirror indices (the slot pool)
    // A page card's laid-out height, written back to the element as a CACHE (culling, exporters):
    // no undo entry, no dirty flag; the next save carries it.
    Q_INVOKABLE void cachePageHeight(const QString& id, qreal h);
    // The camera: {x, y, z} pan/zoom; hasCamera false until set. Persisted with the file.
    bool hasCamera() const { return camValid_; }
    Q_INVOKABLE QVariantMap camera() const;
    Q_INVOKABLE void setCamera(qreal x, qreal y, qreal z);
    Q_INVOKABLE QRectF canvasBounds() const;                                                // union of rects
    Q_INVOKABLE QString mediaElementInk(const QString& id, int page) const;                 // "" = none
    // A media element's descriptor src through the file's MediaStore (the page model's rules).
    Q_INVOKABLE QString resolveMediaUrl(const QVariant& src) const;        // non-blocking display url ("" while pending)
    Q_INVOKABLE QString resolveMediaPath(const QVariant& src) const;       // its local path
    Q_INVOKABLE QString mediaPlaybackSource(const QVariant& src) const;
    Q_INVOKABLE bool mediaExtracting(const QVariant& src) const;

    // --- Element mutators. Each is ONE undo entry; a non-empty `coalesce` key merges a live run
    // (a drag: "move:<id>", "resize:<id>", "ink:<id>") into the current entry, like typing. ---
    Q_INVOKABLE QString addMediaElement(const QString& mediaDescriptorJson, const QRectF& rect);  // {media:{…}}
    Q_INVOKABLE QString addInkElement(const QString& inkJson, const QRectF& rect);                // element-local strokes
    Q_INVOKABLE QString addShapeElement(const QString& shapeJson, const QRectF& rect);
    Q_INVOKABLE QString addConnector(const QString& connectorJson);                               // endpoints resolve at render
    // A dropped file at a world point: video / PDF / image (through the file's MediaStore, like a
    // page's drop) or a generic file chip → a media element sized from the media; an importable
    // document raises importFileRequested instead (the host imports it into a new page there).
    Q_INVOKABLE QString addMediaElementFromUrl(const QString& fileUrl, qreal wx, qreal wy);
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
    void cameraChanged();
    void importFileRequested(const QString& fileUrl, qreal wx, qreal wy);
    void elementsChanged();
    void geometryChanged();
    void inkChanged();

private:
    bool loadFile(const QString& path, bool untitled);
    bool acceptOpenedFormat();   // the 2.0 gate: mnd2 ≤ v2 opens, mnd v1 adopts read-only, else refused
    void ensurePages();          // canvas schema present, ≥ 1 page element, no page-less rows
    void finishOpen();           // ensurePages + the 1.0 conversion when the file was adopted
    QString insertPageElement(); // a page element right of the others (no blocks yet)
    QString insertPageElementAt(const QRectF& r);
    QString addPageWithRect(const QRectF& r);
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
    void persistViewMeta();      // focusPage + camera → canvas_meta (the save paths)
    static QString newScratchPath();
    void cleanupScratch();
    void recordOriginalStat();
    bool externalChangeDetected() const;
    void setSaveState(int s);
    void detachAllPages();
    // An element's rect for culling / bounds: a page whose height is not cached yet (0) still
    // occupies its insets, and a zero-size rect would never intersect anything.
    static QRectF cullRect(const Document::Element& e);

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
    mutable QHash<QString, QString> titleCache_;   // unloaded pages' titles (cleared on open/close/page changes)
    QString focusPage_;
    int pagesRevision_ = 0;
    std::vector<Document::Element> elements_;   // z order, then creation (the table's order)
    QHash<QString, int> indexById_;
    std::vector<UndoEntry> undo_;
    int undoCur_ = -1;
    int undoRev_ = 0;
    int elementsRevision_ = 0, geometryRevision_ = 0, inkRevision_ = 0;
    bool camValid_ = false;
    qreal camX_ = 0, camY_ = 0, camZ_ = 1;
};

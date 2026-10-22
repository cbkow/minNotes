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
class QImage;
class QNetworkAccessManager;

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
    // The LIVE page (2.0 SPIKE S2): a page edited IN ITS CARD while the canvas stays in view. focusedModel()
    // prefers it, so `blockModel` and everything that follows it act on the live page with focusPage "".
    // Not persisted. A drill-in (setFocusPage), a close, an open, removing the page all end liveness.
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
    static constexpr double kBookmarkCardW = 320, kBookmarkCardH = 72;  // a bookmark element's card
    static constexpr double kAudioCardW = 360, kAudioCardH = 56;       // an audio element's card
    static constexpr double kFolderCardW = 320, kFolderCardH = 112;   // a folder element's card
    static constexpr double kFileCardW = 320, kFileCardH = 56;        // a file chip element's card (name + path)
    static constexpr double kDropGridGap = 24;                       // a batch drop's grid gap
    static constexpr int kSpreadCap = 48;                            // "Spread files here" takes this many children
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
    Q_INVOKABLE QString addPageAt(qreal wx, qreal wy, qreal measure = kDefaultMeasure);   // …at a world point, at a measure (the page tool)
    Q_INVOKABLE bool removePage(const QString& id); // refuses the last page; the page's blocks cascade
    // The page's title: first heading, else first text line, else "Untitled page". A LOADED page
    // answers from memory (the strip re-reads it per keystroke); an unloaded page's title comes
    // from the store once and is cached (it cannot change until the page loads).
    Q_INVOKABLE QString pageTitle(const QString& id) const;
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
    // The page's remembered scroll offset (phase C5) in `json.view.scrollY` — a VIEW fact like the camera: written
    // straight to the store and the mirror, no undo row, no revision bump, not dirty (the next save carries it).
    // A live card scrolls into it; drilling in opens the page view there.
    // Fixed-height pages (walk 7c): a drag on a page's top/bottom handle (setElementRect with a new height) pins
    // `json.fixedH`; the content height cache then leaves the card alone (content shorter = blank card below,
    // longer = the fade + footer, and a live card scrolls). fitPageHeight clears it (one history row).
    Q_INVOKABLE bool pageHeightFixed(const QString& id) const;
    Q_INVOKABLE bool fitPageHeight(const QString& id);
    static constexpr double kMinFixedPageHeight = 240.0;
    static constexpr double kMaxFixedPageHeight = 8192.0;
    Q_INVOKABLE void cachePageView(const QString& id, qreal scrollY);
    Q_INVOKABLE qreal pageScrollY(const QString& id) const;
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
    // The path the media occupies (or WOULD — packaged clips stream): the sidecar anchor + the
    // playhead / page-map key. Pure computation, no IO (MediaStore::anchorPathFor).
    Q_INVOKABLE QString mediaAnchorPath(const QVariant& src) const;
    // Media elements with a FULL-FRAME view (videos, PDFs), z order — the canvas-mode element tabs (phase A3).
    Q_INVOKABLE QStringList fullFrameMediaIds() const;
    Q_INVOKABLE QString mediaElementKind(const QString& id) const;   // the descriptor's kind ("" for non-media)
    Q_INVOKABLE QString mediaElementName(const QString& id) const;   // its display name: `name`, else the src's basename
    // A media element's file on disk — BLOCKING (packaged media extracts inline; the explicit-action
    // cost, like BlockModel::mediaLocalPath). "" for non-media elements, remote urls, unresolvable refs.
    Q_INVOKABLE QString elementLocalPath(const QString& id) const;
    // The preview pass (2026-10-21): the ORIGINAL behind a preview element (else elementLocalPath) — what reveal /
    // open / Open in ufb / Copy path / search act on; the loadable file (the preview PNG) stays elementLocalPath.
    Q_INVOKABLE QString elementSourcePath(const QString& id) const;
    Q_INVOKABLE QString elementPreviewFormat(const QString& id) const;   // "" when not a preview
    Q_INVOKABLE bool    elementPreviewStale(const QString& id) const;
    Q_INVOKABLE bool    refreshElementPreview(const QString& id);       // re-render → one history row
    Q_INVOKABLE QString ufbLink(const QString& id) const;            // mn::ufbLinkFor(elementLocalPath)
    Q_INVOKABLE void openElement(const QString& id) const;           // the platform's default app
    Q_INVOKABLE void openElementInUfb(const QString& id) const;
    Q_INVOKABLE void revealElement(const QString& id) const;         // select in Finder / Explorer

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
    // A BATCH of drops (phase A6): every url becomes its element (importables raise importFileRequested with
    // their grid cell), laid out as a reading-order grid from (wx, wy) — ONE history row. Returns the ids.
    Q_INVOKABLE QStringList addElementsFromUrls(const QStringList& fileUrls, qreal wx, qreal wy);
    // A folder element's direct children (files + subfolders, first 48) as elements in a grid below it.
    Q_INVOKABLE QStringList spreadFolder(const QString& id);
    Q_INVOKABLE void rescanFolder(const QString& id);              // refresh a folder element's cached snapshot
    Q_INVOKABLE bool folderIsStream(const QString& id) const;      // a QCView stream folder (holds notes.json) — "Open as stream"
    // Swap a media element's descriptor in place — NOT an undo entry (a view/cache fact: a folder rescan, a
    // fetched bookmark title, a localized remote image). Bumps elementsRevision, marks dirty.
    Q_INVOKABLE void patchMediaDescriptor(const QString& id, const QString& descriptorJson);
    // --- Phase A7: what a dropped / pasted URL or text becomes ---
    // A BOOKMARK element: {kind:"bookmark", src:url, host, title (= host until fetched), icon (a store src), fetched}.
    // The title + favicon arrive asynchronously (fetchBookmarkMeta) and patch the descriptor — no undo row.
    Q_INVOKABLE QString addBookmarkElement(const QString& url, qreal wx, qreal wy);
    Q_INVOKABLE void fetchBookmarkMeta(const QString& id);
    // <title> + the rel=icon href (resolved against `baseUrl`; /favicon.ico when none): {title, iconUrl}.
    Q_INVOKABLE static QVariantMap bookmarkMetaFromHtml(const QString& html, const QString& baseUrl);
    // A REMOTE IMAGE element: the page's recipe — a 480×300 placeholder on the http src, then the download lands
    // in the store and the descriptor + rect are patched to the real image (no undo row for the swap).
    Q_INVOKABLE QString addRemoteImageElement(const QString& url, qreal wx, qreal wy);
    Q_INVOKABLE void localizeRemoteMedia(const QString& id);
    bool applyLocalizedImage(const QString& id, const QImage& img);   // the swap (tests call it directly)
    // The clipboard's image → the store → an image element (480 wide at most, small images as-is).
    Q_INVOKABLE QString addClipboardImageElement(qreal wx, qreal wy);
    // The one http(s) <img> src of an HTML fragment that is nothing but that image ("" otherwise).
    Q_INVOKABLE static QString bareRemoteImageSrc(const QString& html);
    // --- Phase A8: another minNotes file dropped on the canvas COPIES its pages (and its other elements) in,
    // keeping their relative layout, translated to (wx, wy); no link (standing ruling). Pages merge through
    // DocumentMerger (ids re-minted, assets copied); sidecar media of other elements is
    // re-homed into this file's store. ONE history row. Returns the new element ids (pages first in z order).
    Q_INVOKABLE QStringList copyPagesFrom(const QString& fileUrl, qreal wx, qreal wy);
    Q_INVOKABLE bool setElementRect(const QString& id, const QRectF& rect, const QString& coalesce = QString());
    Q_INVOKABLE bool setElementJson(const QString& id, const QString& json, const QString& coalesce = QString());
    Q_INVOKABLE bool setElementZ(const QString& id, int z);
    Q_INVOKABLE bool bringToFront(const QString& id);
    Q_INVOKABLE bool sendToBack(const QString& id);
    // The LAYERS panel (2026-10-21): the z order as rows, FRONT-MOST FIRST, ink strokes collapsed into one row per
    // contiguous run (a run is derived here each call, never stored — a stroke sent behind another run simply reads
    // as part of it). Row: kind (page · image/video/pdf/audio/folder/file/bookmark · text/shape · ink · connector),
    // name, ids (back-to-front), count, id (the row's TOP element — the drop anchor).
    Q_INVOKABLE QVariantList layerRows() const;
    // Reorder as ONE history row: `ids` (their relative order kept, made contiguous) go directly ABOVE `anchorId`
    // ("" = the very bottom); every element's z re-packs to its index in the new order. False when the anchor is
    // among the moving ids or unknown.
    Q_INVOKABLE bool placeAbove(const QStringList& ids, const QString& anchorId);
    // One layer step (dir +1 = forward / -1 = back): a block that is not all ink steps over a WHOLE ink run (the
    // panel's row); an ink block steps one element (merging into a neighbouring run). False at the end of the stack.
    Q_INVOKABLE bool stepLayers(const QStringList& ids, int dir);
    // A copy beside the original (+24, +24), on top. Pages are not duplicated (their blocks would need new ids).
    // A frame duplicates WITH its children (one history row).
    Q_INVOKABLE QString duplicateElement(const QString& id);
    Q_INVOKABLE bool removeElement(const QString& id);            // a page follows removePage's rules; a frame takes its children (one row)
    // FRAMES (2026-10-22 — groups and boards, one element kind: the schema's `group`, honouring parentId). json:
    // title · tint ("" = none) · sizing "elastic" (a GROUP: its rect hugs its children + kFramePad, kFrameTitleH above)
    // | "fixed" (a BOARD: sized by hand) · layout "free" | "grid" (cols, gap — stage 2). Membership is EXPLICIT:
    // groupElements (⌘G), addToFrame, a drop onto a frame; ungroup (⌘⇧G) / removeFromFrame leave. No nesting: a
    // frame never has a parent, a frame among `ids` is skipped. Children keep ABSOLUTE coordinates. Z: a frame sits
    // directly UNDER its children and the block is contiguous (packFrame) — so moving / deleting / z-stepping
    // `withChildren(ids)` keeps it whole. A child's move (setElementRect) re-hugs an elastic parent in the same batch.
    Q_INVOKABLE QString addFrame(const QRectF& rect, bool fixed);               // an empty frame (the frame tool)
    Q_INVOKABLE QString groupElements(const QStringList& ids);                 // a group around them; "" when nothing to group
    Q_INVOKABLE bool ungroup(const QString& frameId);                           // the children stay, the frame goes
    Q_INVOKABLE bool addToFrame(const QStringList& ids, const QString& frameId);
    Q_INVOKABLE bool removeFromFrame(const QStringList& ids);
    Q_INVOKABLE QStringList childrenOf(const QString& frameId) const;          // z order
    Q_INVOKABLE QString parentOf(const QString& id) const;
    Q_INVOKABLE QStringList withChildren(const QStringList& ids) const;        // ids + every frame's children among them, in z order
    Q_INVOKABLE bool isFrame(const QString& id) const { return elementKind(id) == QLatin1String("group"); }
    Q_INVOKABLE bool frameElastic(const QString& id) const;
    Q_INVOKABLE QString frameTitle(const QString& id) const;
    Q_INVOKABLE bool setFrameTitle(const QString& id, const QString& title);
    Q_INVOKABLE bool setFrameField(const QString& id, const QString& key, const QVariant& value);   // tint · sizing
    Q_INVOKABLE QString frameAt(qreal wx, qreal wy) const;                       // the top-most frame under a world point (a drop there JOINS it)
    static constexpr double kFramePad = 24.0, kFrameTitleH = 28.0, kFrameMinW = 160.0, kFrameMinH = 120.0;
    Q_INVOKABLE bool setMediaElementInk(const QString& id, int page, const QString& envelope);   // "" clears
    // Re-style an INK element after the fact (walk 7d, 2026-10-18): every stroke's width / colour. A width change
    // grows the rect by the half-width delta and shifts the element-local points with it (an oval's radii point
    // stays). One history row (a batch of the json + rect edits); `coalesce` folds a slider drag.
    Q_INVOKABLE bool setInkWidth(const QString& id, qreal width, const QString& coalesce = QString());
    Q_INVOKABLE bool setInkColor(const QString& id, const QColor& color, const QString& coalesce = QString());
    Q_INVOKABLE qreal inkWidth(const QString& id) const;   // the widest stroke; 0 = not an ink element
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
    // A BATCH: every entry pushed between begin and end becomes ONE history row (a multi-element drag,
    // a selection's delete). Nested begins fold into the outer batch; a batch of one part is that part.
    Q_INVOKABLE void beginBatch(const QString& label);
    Q_INVOKABLE void endBatch();
    Q_INVOKABLE void discardBatch();   // end the batch WITHOUT a history row — for a run whose parts net to nothing (a chip added, sized and removed empty)

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
    void importMultiDocRequested(const QString& fileUrl, qreal wx, qreal wy);   // ENEX / Notion: Main.qml runs the importer onto this canvas
    void elementsAdded(const QStringList& ids);                                 // a copy-in / import landed (the view selects them)
    void importNotice(const QString& text);                                     // an outcome to toast
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
        std::vector<UndoEntry> parts;   // a batch: applied in order on redo, in reverse on undo
    };
    void applyEntry(const UndoEntry& e, bool before);
    ElementSnap snapshot(const QString& id, bool withBlocks) const;
    void applySnap(const QString& id, const ElementSnap& s);   // the db + loaded models follow the snapshot
    void pushEntry(UndoEntry e, const QString& coalesce);
    bool setParent(const QString& id, const QString& parentId);         // one undo entry; no notify beyond the set revision
    bool hugFrame(const QString& frameId, const QString& coalesce);     // an elastic frame's rect follows its children
    bool packFrame(const QString& frameId);                             // z: the frame directly under its children, contiguous
    bool applyOrder(const std::vector<QString>& order, const QString& label);   // z := index, the changed ones as one batch
    QString addElement(const QString& kind, const QRectF& r, const QString& json, const QString& label, const QString& parentId = QString());
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
    QNetworkAccessManager* net_ = nullptr;   // bookmark meta + remote images (created on first use)
    QNetworkAccessManager* net();
    QHash<QString, BlockModel*> pages_;   // page id → its model (loaded pages only)
    mutable QHash<QString, QString> titleCache_;   // unloaded pages' titles (cleared on open/close/page changes)
    QString focusPage_;
    int pagesRevision_ = 0;
    std::vector<Document::Element> elements_;   // z order, then creation (the table's order)
    QHash<QString, int> indexById_;
    std::vector<UndoEntry> undo_;
    int batchDepth_ = 0;
    UndoEntry batch_;                 // the open batch's parts
    int undoCur_ = -1;
    int undoRev_ = 0;
    int elementsRevision_ = 0, geometryRevision_ = 0, inkRevision_ = 0;
    bool camValid_ = false;
    qreal camX_ = 0, camY_ = 0, camZ_ = 1;
};

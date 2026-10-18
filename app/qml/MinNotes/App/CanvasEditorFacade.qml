// A page card's stand-in for the editor (2.0): BlockView and MediaBlock are duck-typed on
// `editor.*`, so a page element hands its delegates this object instead of the Editor. Three kinds
// of member: PAGE-LOCAL (the page's measure = its element's, lane geometry computed against THIS
// page's model — `blockModel` here is the loader's child-context model — the page's edge inside the
// card, the content span, no grid frame), FORWARDED (facts the real editor owns: the media chrome
// heights, the text-rect oracle), and INERT (every interaction flag the page's delegates react to:
// the caret, selection, drags, hovers, grips, menus — none of which exist on the canvas). A missed
// name shows up as a TypeError in the canvas probe's log; add it here, never in BlockView.
import QtQuick

QtObject {
    id: facade
    required property var editor          // the real Editor
    property real inset: 24               // == CanvasDocument::kPageInset
    required property real measure        // the page element's measure (its width less the insets)

    // --- page-local geometry (the editor's laneSpan / laneOf over this page's model; tables start at
    // the page edge — a wide one overflows the card and is clipped) ---
    readonly property real pageWidth: measure
    function laneSpan(record, lane) {
        const rev = blockModel.structureRevision
        const head = blockModel.tableHeadOf(record)
        if (head >= 0) {
            const lrev = blockModel.layoutRevision
            const grev = blockModel.geomRevision
            return { x: blockModel.tableColumnLeft(head, lane), w: blockModel.tableColumnWidth(head, lane) }
        }
        const ratios = blockModel.splitRatios(record)
        if (lane < 0 || lane >= ratios.length) return { x: 0, w: pageWidth }
        const gap = blockModel.laneGap
        const avail = Math.max(0, pageWidth - gap * (ratios.length - 1))
        let x = 0
        for (let k = 0; k < lane; ++k) x += avail * ratios[k] + gap
        return { x: x, w: avail * ratios[lane] }
    }
    function laneOf(row) {
        const rev = blockModel.structureRevision
        const lane = blockModel.laneForRow(row)
        return lane < 0 ? { x: 0, w: pageWidth } : laneSpan(blockModel.splitRowOf(row), lane)
    }

    // --- forwarded ---
    function selectionRects(te, sp, ep) { return editor.selectionRects(te, sp, ep) }
    readonly property real videoTransportH: editor.videoTransportH
    readonly property real pdfNavH: editor.pdfNavH
    readonly property int videoPlayheadRev: editor.videoPlayheadRev
    function videoPlayheadFor(row) { return editor.videoPlayheadFor(row) }
    readonly property int pdfPageRev: editor.pdfPageRev
    function pdfPageFor(row) { return editor.pdfPageFor(row) }
    readonly property bool perfLog: editor.perfLog

    // --- card-local ---
    readonly property real leftEdge: inset                       // the page's left edge inside the card
    function tableX(head) { return inset }                       // tables start at the page edge (wide ones overflow the card; clipped)
    readonly property real contentSpan: pageWidth + 2 * inset
    readonly property real viewW: contentSpan
    readonly property int frameLo: -1
    readonly property int frameHi: -1
    readonly property var flickItem: null

    readonly property bool cardPreview: true                     // the delegates are a picture: no mouse (BlockView's TextEdit reads this)
    // --- inert ---
    readonly property QtObject cursorObj: QtObject {
        readonly property int focusRow: -1
        readonly property int focusCol: 0
        readonly property int anchorRow: -1
        readonly property int anchorCol: 0
        readonly property int loRow: -1
        readonly property int loCol: 0
        readonly property int hiRow: -1
        readonly property int hiCol: 0
        readonly property bool hasSel: false
        readonly property bool active: false
    }
    readonly property bool caretOn: false
    property Item focusBlockItem: null                           // a delegate writes it when it believes it has focus (never, here)
    readonly property var tableSet: null
    readonly property var cellRect: null
    readonly property var menuGrid: null
    readonly property string menuHiScope: ""
    readonly property bool menuHiDanger: false
    readonly property int dropGridHead: -1
    readonly property int dropGridR: -1
    readonly property int dropGridC: -1
    readonly property bool pulling: false
    readonly property bool dividerDragging: false
    readonly property int dividerHoverRecord: -1
    readonly property int dividerHoverIndex: -1
    readonly property int pullHoverRow: -1
    readonly property int pullHoverSide: 0
    readonly property bool tableGripPressed: false
    readonly property string tableGripPressKind: ""
    readonly property int tableGripPressHead: -1
    readonly property int tableGripPressIndex: -1
    readonly property string tableGripKind: ""
    readonly property int tableGripHead: -1
    readonly property int tableGripIndex: -1
    readonly property bool tableColDragging: false
    readonly property int codeChipHoverRow: -1
    readonly property int videoPlayingRow: -1
    readonly property bool _videoSurfaceReady: false
    property int imagesReady: 0
    function noteRebind() {}
}

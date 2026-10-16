// The canvas card's stand-in for the editor (B3): BlockView and MediaBlock are duck-typed on
// `editor.*`, so a card hands its delegates this object instead of the Editor. Three kinds of
// member: FORWARDED (document-wide facts the real editor owns — page width, lane geometry, the
// media chrome heights, the text-rect oracle), CARD-LOCAL (the page's edge inside the card, the
// content span, no grid frame), and INERT (every interaction flag the page's delegates react to:
// the caret, selection, drags, hovers, grips, menus — none of which exist on a read-only canvas).
// A missed name shows up as a TypeError in the canvas probe's log; add it here, never in BlockView.
import QtQuick

QtObject {
    id: facade
    required property var editor          // the real Editor
    property real inset: 24               // == BlockModel::kCardInset

    // --- forwarded ---
    readonly property real pageWidth: editor.pageWidth
    function laneOf(row) { return editor.laneOf(row) }
    function laneSpan(record, lane) { return editor.laneSpan(record, lane) }
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

// NavHistory — browser-like BACK / FORWARD over the views the user visited (2026-10-22). A location is what the
// path bar names: the file (its tab id), the focused page ("" = the canvas) and the open frame ("" = none; a canvas
// element's full-frame view, or a page's table / PDF / video tab). The rules are a browser's: a navigation that is
// not Back / Forward truncates the forward tail and pushes; Back / Forward move the index and restore without
// pushing; consecutive duplicates collapse; a closed file's entries drop out; capped at 100; per session. The
// location is a binding over the three sources (docs.activeIndex, canvas.focusPage, the editor's activeFrameId) and
// commits through Qt.callLater, so a navigation that passes through intermediate states (setActiveTab focuses the
// page, then opens the frame) records only where it landed. A live card in place is not a location (ruling).
import QtQuick

Item {
    id: nav
    visible: false
    property var entries: []          // [{tab, page, frame}]
    property int index: -1
    property bool navigating: false
    readonly property bool canBack: index > 0
    readonly property bool canForward: index >= 0 && index < entries.length - 1
    property int pushes: 0            // probe
    readonly property var editor: win.activeEditor
    readonly property string locKey: {
        const ed = editor
        if (!ed || !blockModel.documentOpen || docs.activeIndex < 0) return ""
        const f = ed.activeFrameId
        return docs.tabIdAt(docs.activeIndex) + "|" + canvas.focusPage + "|" + (f === "canvas" ? "" : f)
    }
    onLocKeyChanged: if (!navigating && locKey !== "") Qt.callLater(commit)
    function parse(key) { const p = key.split("|"); return { tab: parseInt(p[0]), page: p[1], frame: p[2] } }
    function same(a, b) { return !!a && !!b && a.tab === b.tab && a.page === b.page && a.frame === b.frame }
    function commit() {
        if (navigating || locKey === "") return
        const loc = parse(locKey)
        if (index >= 0 && index < entries.length && same(entries[index], loc)) return
        const a = entries.slice(0, index + 1)
        a.push(loc)
        while (a.length > 100) a.shift()
        entries = a; index = a.length - 1; ++pushes
    }
    function tabIndexOf(tabId) { for (let i = 0; i < docs.count; ++i) if (docs.tabIdAt(i) === tabId) return i; return -1 }
    function valid(e) {
        const ti = tabIndexOf(e.tab)
        if (ti < 0) return false
        if (e.page !== "") { const c = docs.canvasAt(ti); if (!c || c.elementKind(e.page) !== "page") return false }
        return true
    }
    function go(delta) {
        let i = index + delta
        while (i >= 0 && i < entries.length && !valid(entries[i])) i += delta   // a deleted page's entry is skipped
        if (i < 0 || i >= entries.length) return false
        navigating = true
        index = i
        restore(entries[i])
        Qt.callLater(function() {   // after the deferred commits this navigation queued (they saw `navigating`)
            navigating = false
            if (locKey === "") return
            const loc = parse(locKey)
            if (!same(entries[index], loc)) { const a = entries.slice(); a[index] = loc; entries = a }   // landed short (a gone frame → its page): the entry becomes where we are
        })
        return true
    }
    function back() { return go(-1) }
    function forward() { return go(1) }
    function restore(e) {
        const ti = tabIndexOf(e.tab)
        if (ti >= 0 && ti !== docs.activeIndex) docs.setActive(ti)
        const ed = editor
        if (!ed) return
        ed.setActiveTab(e.page === "" ? "canvas" : "")   // close any frame; the canvas, or the page view
        if (e.page !== "") canvas.focusPage = e.page
        if (e.frame !== "" && (canvas.elementKind(e.frame) === "media" || blockModel.rowForId(e.frame) >= 0)) ed.setActiveTab(e.frame)
    }
    function prune() {   // a closed file's entries go; adjacent duplicates that leaves collapse; the index follows
        const a = []; let idx = -1
        for (let i = 0; i < entries.length; ++i) {
            const e = entries[i]
            const keep = tabIndexOf(e.tab) >= 0 && !(a.length && same(a[a.length - 1], e))
            if (keep) a.push(e)
            if (i === index) idx = a.length - 1
        }
        if (a.length !== entries.length) { entries = a; index = Math.min(Math.max(idx, a.length ? 0 : -1), a.length - 1) }
    }
    Connections { target: docs; function onTabsChanged() { nav.prune() } }
    // The destination's name, for the buttons' tooltips: "Back to Canvas", "Forward to Meeting notes".
    function label(e) {
        if (!e) return ""
        const ti = tabIndexOf(e.tab), c = ti >= 0 ? docs.canvasAt(ti) : null
        let s = e.page === "" ? qsTr("Canvas") : (c ? c.pageTitle(e.page) : qsTr("Page"))
        if (e.frame !== "" && c) { const n = c.elementKind(e.frame) === "media" ? c.mediaElementName(e.frame) : ""; s = n !== "" ? n : qsTr("Frame") + " · " + s }
        if (ti >= 0 && ti !== docs.activeIndex && c) s = c.documentName + " › " + s
        return s
    }
    function nextValid(delta) { let i = index + delta; while (i >= 0 && i < entries.length && !valid(entries[i])) i += delta; return i >= 0 && i < entries.length ? entries[i] : null }
    readonly property string backLabel: (entries, index, docs.count, canBack ? label(nextValid(-1)) : "")
    readonly property string forwardLabel: (entries, index, docs.count, canForward ? label(nextValid(1)) : "")
}

// RTF import via the OS-native converter (user ruling 2026-08-18: per-OS
// native beats a hand-rolled RTF subset). macOS: NSAttributedString reads the
// RTF and writes Cocoa HTML — real <b>/<i> tags, but underline/colors/fonts
// hide behind CSS classes Qt's reader can't resolve, so the class rules are
// INLINED as style="" attributes before handing over. Windows: not yet
// implemented (RichEdit route planned); Linux: ships without RTF. Where
// unsupported, rtfImportSupported() is false and every UI arm (classifier,
// filters, intercepts) hides automatically.
#pragma once
#include <QString>

namespace mn {
bool rtfImportSupported();
// RTF file → Qt-consumable HTML ("" on failure/unsupported).
QString rtfFileToHtml(const QString& path);
}

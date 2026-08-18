#include "RtfConvert.h"

// Non-macOS builds: RTF import unavailable (Windows RichEdit route is a
// future arc; Linux ships without by ruling). The classifier returning ""
// hides every RTF UI arm.
namespace mn {
bool rtfImportSupported() { return false; }
QString rtfFileToHtml(const QString&) { return {}; }
}

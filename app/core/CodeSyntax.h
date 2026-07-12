#pragma once
#include <QString>

namespace KSyntaxHighlighting { class Repository; class Definition; }

// Shared KSyntaxHighlighting plumbing for everything that colours code — the
// app's CodeHighlighter (QML) and the HTML export. Quick-free on purpose so
// the headless targets (exporter tests) can use it without QtQuick.
//
// codeHighlightRepo(): the process-wide definition/theme repository.
// resolveCodeDefinition(): lenient fence-tag → definition ("js"/"bash"/
// "C++"/"py" all land somewhere sane; invalid = no highlighting).
KSyntaxHighlighting::Repository& codeHighlightRepo();
KSyntaxHighlighting::Definition resolveCodeDefinition(const QString& lang);

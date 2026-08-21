#pragma once
#include <QString>
#include <QStringList>

namespace KSyntaxHighlighting { class Repository; class Definition; }

// Shared KSyntaxHighlighting plumbing for everything that colours code — the
// app's CodeHighlighter (QML) and the HTML export. Quick-free on purpose so
// the headless targets (exporter tests) can use it without QtQuick.
//
// codeHighlightRepo(): the process-wide definition/theme repository.
// resolveCodeDefinition(): lenient fence-tag → definition ("js"/"bash"/
// "C++"/"py" all land somewhere sane; invalid = no highlighting).
// codeLanguageNames(): every visible definition's name, sorted — the
// language chip's picker list (2026-08-21).
// codeLanguageDisplayName(): a stored fence tag → the definition's proper
// name for display ("js" → "JavaScript"; "" if it resolves to nothing).
KSyntaxHighlighting::Repository& codeHighlightRepo();
KSyntaxHighlighting::Definition resolveCodeDefinition(const QString& lang);
QStringList codeLanguageNames();
QString codeLanguageDisplayName(const QString& lang);

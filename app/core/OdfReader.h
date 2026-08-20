// OdfReader — hand-rolled OpenDocument readers (odt text, ods spreadsheet)
// in the DocxReader lane: mnpkg::readEntry("content.xml") + flat
// QXmlStreamReader loops. Unlike DOCX, ODF hides all formatting behind
// style-name references into <office:automatic-styles>, so odt runs a
// style-resolution pass first; and ODF element names MUST be compared
// namespace-qualified ("text:p"), unlike DocxReader's unprefixed compares.
// v1 lossiness (documented): cell/character colors, ordered-list numbering
// restarts, and footnotes flatten; ods repeated columns/rows expand with
// clamps (LibreOffice writes 1024-wide repeats to the sheet edge).
#pragma once

#include "BlockModel.h"

#include <QString>
#include <vector>

class MediaStore;

class OdfReader {
public:
    static std::vector<BlockModel::BlockSpec> readOds(const QString& path);
    static std::vector<BlockModel::BlockSpec> readOdt(const QString& path,
                                                      MediaStore* store);
};

// DocxReader — .docx → BlockSpecs. The DOCX EMITTER'S
// MIRROR: it must read back what we write (direct-formatted headings — bold +
// half-pt sizes 36/32/28/26/24/22, no styles part; numId 1 = bullets, 2 =
// ordered; tri-state tasks as ☐/◐/☑ glyph bullets; code runs = Courier New +
// EFEFEF shading — the w:shd OVERLOAD: that exact pair is code, any other
// fill is a highlight) AND make a decent job of foreign Word documents
// (pStyle Heading1–6 / outlineLvl headings, numbering.xml bullet-vs-decimal,
// named highlight colors, hyperlinks with their underline/color chrome
// suppressed, images by relationship with intrinsic dims from the bytes).
// Word review comments are not read: their text imports plain.
#pragma once

#include "BlockModel.h"

class MediaStore;

class DocxReader {
public:
    struct Result {
        std::vector<BlockModel::BlockSpec> specs;
        bool ok = false;
    };
    // `store` receives embedded images (null = images drop).
    static Result read(const QString& docxPath, MediaStore* store);
};

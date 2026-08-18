#include "RtfConvert.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#include <QRegularExpression>

namespace {

// Cocoa's HTML writer puts formatting in flat class rules (`p.p1 {…}`,
// `span.s1 {text-decoration: underline}`). Qt's rich-text reader has no CSS
// class resolution, so inline them: parse the <style> block into a class →
// rules map, then swap every class="X" for style="rules".
QString inlineCocoaClasses(QString html) {
    static const QRegularExpression styleBlock(
        QStringLiteral("<style[^>]*>(.*?)</style>"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch sb = styleBlock.match(html);
    if (!sb.hasMatch()) return html;
    QHash<QString, QString> rules;
    static const QRegularExpression rule(
        QStringLiteral("[a-z]+\\.([a-zA-Z0-9_-]+)\\s*\\{([^}]*)\\}"));
    QRegularExpressionMatchIterator it = rule.globalMatch(sb.captured(1));
    while (it.hasNext()) {
        const QRegularExpressionMatch mt = it.next();
        rules.insert(mt.captured(1), mt.captured(2).simplified());
    }
    static const QRegularExpression classAttr(
        QStringLiteral("class=\"([a-zA-Z0-9_-]+)\""));
    QString out;
    int last = 0;
    QRegularExpressionMatchIterator ci = classAttr.globalMatch(html);
    while (ci.hasNext()) {
        const QRegularExpressionMatch mt = ci.next();
        out += html.mid(last, mt.capturedStart() - last);
        const QString r = rules.value(mt.captured(1));
        out += r.isEmpty() ? mt.captured(0)
                           : QStringLiteral("style=\"%1\"").arg(r);
        last = mt.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

} // namespace

namespace mn {

bool rtfImportSupported() { return true; }

QString rtfFileToHtml(const QString& path) {
    @autoreleasepool {
        NSData* data = [NSData dataWithContentsOfFile:path.toNSString()];
        if (!data) return {};
        NSError* err = nil;
        NSAttributedString* attr = [[NSAttributedString alloc]
            initWithData:data
                 options:@{NSDocumentTypeDocumentAttribute : NSRTFTextDocumentType}
      documentAttributes:nil
                   error:&err];
        if (!attr || err) return {};
        NSData* htmlData = [attr
              dataFromRange:NSMakeRange(0, attr.length)
         documentAttributes:@{NSDocumentTypeDocumentAttribute : NSHTMLTextDocumentType}
                      error:&err];
        if (!htmlData || err) return {};
        NSString* html = [[NSString alloc] initWithData:htmlData
                                               encoding:NSUTF8StringEncoding];
        if (!html) return {};
        return inlineCocoaClasses(QString::fromNSString(html));
    }
}

} // namespace mn

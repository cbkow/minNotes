#!/usr/bin/env python3
"""Extract the parse-free subset of LanguageTool's English grammar.xml for minNotes.

    python3 scripts/trim-lt-rules.py --tag v6.8 [--file grammar.xml] [--drop RULE_ID ...]

Downloads (or reads) grammar.xml, keeps only rules whose patterns need neither a
part-of-speech tagger nor morphology (the only postag values admitted are the SENT_START /
SENT_END pseudo-tokens), restricted to the categories minNotes enables, and writes:

    app/spell/en/grammar-rules.xml   the rules (LGPL-2.1+ data, LanguageTool project)
    tests/fixtures/lt-examples.xml   the rules' own example sentences (the regression corpus)

Everything the interpreter (app/spell/GrammarRules.cpp) does not implement makes this script
ABORT — the shipped file is guaranteed to be within the supported subset by construction.
Manual tool: the build never runs it. Stdlib only.
"""
import argparse, collections, copy, os, re, sys, urllib.request
import xml.etree.ElementTree as ET

CATEGORIES = ['TYPOS', 'GRAMMAR', 'CONFUSED_WORDS', 'PUNCTUATION', 'CASING', 'TYPOGRAPHY',
              'COMPOUNDING', 'COLLOCATIONS', 'NONSTANDARD_PHRASES', 'SEMANTICS', 'PROPER_NOUNS']
RULE_TAGS = {'antipattern', 'pattern', 'regexp', 'message', 'suggestion', 'url', 'short', 'example'}
PATTERN_TAGS = {'token', 'marker', 'exception', 'match'}
TOKEN_ATTRS = {'regexp', 'negate', 'skip', 'min', 'max', 'spacebefore', 'case_sensitive', 'postag', 'inflected'}
EXC_ATTRS = {'regexp', 'negate', 'scope', 'case_sensitive', 'spacebefore', 'postag', 'inflected'}
MATCH_ATTRS = {'no', 'case_conversion', 'regexp_match', 'regexp_replace', 'include_skipped', 'postag', 'postag_regexp', 'postag_replace', 'setpos'}
REGEXP_ATTRS = {'mark', 'case_sensitive', 'type'}
POS_ATTRS = ('postag', 'postag_regexp', 'chunk', 'chunk_re')

def is_free(rule):
    for el in rule.iter():
        if el.tag in ('token', 'exception', 'match'):
            if el.attrib.get('inflected') == 'yes' or 'negate_pos' in el.attrib: return False
            for a in POS_ATTRS:
                if a in el.attrib and not (a == 'postag' and el.attrib[a] in ('SENT_START', 'SENT_END')):
                    return False
            if el.tag == 'match' and ('postag' in el.attrib or 'postag_replace' in el.attrib or 'setpos' in el.attrib):
                return False
        if el.tag in ('unify', 'unify-ignore', 'filter', 'and', 'or', 'phraseref', 'equivalence', 'disambig'):
            return False
    return True

VIOLATIONS = []
def check_subset(rule, rid):
    """Record anything outside the interpreter's subset (the run aborts at the end)."""
    for el in rule.iter():
        t = el.tag
        if el is rule: continue
        if t in RULE_TAGS or t in PATTERN_TAGS: pass
        else: VIOLATIONS.append(f"{rid}: unsupported element <{t}>"); continue
        allowed = {'token': TOKEN_ATTRS, 'exception': EXC_ATTRS, 'match': MATCH_ATTRS, 'regexp': REGEXP_ATTRS,
                   'pattern': {'case_sensitive', 'raw_pos'}, 'antipattern': {'case_sensitive'},
                   'suggestion': {'suppress_misspelled'}, 'message': {'suppress_misspelled'}, 'example': {'correction', 'type', 'reason'},
                   'marker': set(), 'url': set(), 'short': set()}[t]
        for a in el.attrib:
            if a not in allowed: VIOLATIONS.append(f"{rid}: unsupported attribute {t}@{a}")
        if t == 'match' and el.attrib.get('case_conversion', '') not in ('', 'startupper', 'firstupper', 'startlower', 'allupper', 'alllower', 'preserve'):
            VIOLATIONS.append(f"{rid}: unsupported case_conversion {el.attrib['case_conversion']}")

def java_to_pcre_replacement(s):
    return re.sub(r'\$(\d+)', r'\\\1', s)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', default='v6.8'); ap.add_argument('--file'); ap.add_argument('--drop', nargs='*', default=[])
    ap.add_argument('--no-proper-nouns', action='store_true')
    args = ap.parse_args()
    if args.file: xml = open(args.file, 'rb').read()
    else:
        url = f"https://raw.githubusercontent.com/languagetool-org/languagetool/{args.tag}/languagetool-language-modules/en/src/main/resources/org/languagetool/rules/en/grammar.xml"
        print("downloading", url); xml = urllib.request.urlopen(url, timeout=120).read()
    src = ET.fromstring(xml)
    cats = [c for c in CATEGORIES if not (args.no_proper_nouns and c == 'PROPER_NOUNS')]
    out = ET.Element('rules', {'lang': 'en', 'source': f'LanguageTool {args.tag}', 'subset': 'parse-free'})
    fx = ET.Element('examples', {'source': f'LanguageTool {args.tag}'})
    census = collections.Counter(); kept = 0; total = 0; percat = collections.Counter(); dropped = 0
    for cat in src.iter('category'):
        cid = cat.attrib.get('id') or ''
        if cid not in cats: continue
        ocat = ET.SubElement(out, 'category', {'id': cid, 'name': cat.attrib.get('name', cid)})
        def emit(rule, gid, gname, gdefault, ganti, idx):
            nonlocal kept, total, dropped
            total += 1
            rid = rule.attrib.get('id') or (f"{gid}_{idx}" if gid else f"{cid}_R{total}")
            if rule.attrib.get('default') == 'off' or gdefault == 'off': return
            if rid in args.drop or (gid and gid in args.drop): dropped += 1; return
            if not is_free(rule): return
            r = copy.deepcopy(rule)
            for e in list(r):
                if e.tag in ('url', 'short'): r.remove(e)
            for ap_ in r.findall('antipattern'):
                for e in ap_.findall('example'): ap_.remove(e)
            for ex in r.findall('example'):
                fe = ET.SubElement(fx, 'example', {'rule': rid, **{k: v for k, v in ex.attrib.items() if k in ('correction', 'type')}})
                fe.text = ex.text
                for ch in ex: fe.append(copy.deepcopy(ch))
                r.remove(ex)
            for a in ganti: r.insert(0, copy.deepcopy(a))
            for m in r.iter('match'):
                if 'regexp_replace' in m.attrib: m.attrib['regexp_replace'] = java_to_pcre_replacement(m.attrib['regexp_replace'])
            r.attrib = {'id': rid, 'name': rule.attrib.get('name') or gname or rid}
            check_subset(r, rid)
            for el in r.iter():
                for a in el.attrib:
                    if el.tag in ('token', 'exception', 'match', 'regexp'): census[f"{el.tag}@{a}"] += 1
                if el.tag == 'token' and el.attrib.get('postag'): census['SENT_' + el.attrib['postag'][5:]] += 1
                if el.tag == 'regexp': census['regexp-rule'] += 1
            ocat.append(r); kept += 1; percat[cid] += 1
        for child in cat:
            if child.tag == 'rule':
                emit(child, None, None, None, [], 0)
            elif child.tag == 'rulegroup':
                ganti = [a for a in child.findall('antipattern') if is_free(a)]
                if len(ganti) != len(child.findall('antipattern')):
                    continue   # a group whose shared antipattern needs a tagger: skip the group whole
                for i, r in enumerate(child.findall('rule'), 1):
                    emit(r, child.attrib.get('id'), child.attrib.get('name'), child.attrib.get('default'), ganti, i)
        if len(ocat) == 0: out.remove(ocat)
    if VIOLATIONS:
        print('\n'.join(sorted(set(VIOLATIONS)))); sys.exit(f"{len(set(VIOLATIONS))} subset violation(s) — extend the interpreter or --drop the rules")
    ET.indent(out, space=' '); ET.indent(fx, space=' ')
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    hdr = (f"<!-- Parse-free subset of LanguageTool's English grammar.xml ({args.tag}), extracted by\n"
           f"     scripts/trim-lt-rules.py. Copyright the LanguageTool contributors; LGPL-2.1-or-later\n"
           f"     (LICENSES/LanguageTool-LGPL-2.1.txt). {kept} rules of {total}; examples live in\n"
           f"     tests/fixtures/lt-examples.xml. Do not edit by hand. -->\n")
    with open(os.path.join(root, 'app', 'spell', 'en', 'grammar-rules.xml'), 'wb') as f:
        f.write(b'<?xml version="1.0" encoding="UTF-8"?>\n' + hdr.encode() + ET.tostring(out, encoding='utf-8'))
    with open(os.path.join(root, 'tests', 'fixtures', 'lt-examples.xml'), 'wb') as f:
        f.write(b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(fx, encoding='utf-8'))
    print(f"kept {kept} of {total} rules ({dropped} dropped by --drop); examples {len(fx)}")
    print("per category:", dict(percat))
    print("census:", dict(sorted(census.items())))

if __name__ == '__main__': main()

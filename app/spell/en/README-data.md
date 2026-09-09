# Spell / grammar data

Committed outputs of two manual scripts; the build never runs them.

| File | Source | Regenerate |
|---|---|---|
| `en.aff`, `en.dic` | SCOWL Hunspell dictionaries, release **2026.02.25**: `hunspell-en_US-2026.02.25.zip` + `hunspell-en_GB-ise-2026.02.25.zip` from https://sourceforge.net/projects/wordlist/files/speller/ | `python3 scripts/build-merged-dictionary.py <en_US.zip> <en_GB-ise.zip>` |
| `grammar-rules.xml` (+ `tests/fixtures/lt-examples.xml`) | LanguageTool `grammar.xml`, tag **v6.8** | `python3 scripts/trim-lt-rules.py --tag v6.8` |

The dictionary is the union of the US and UK word lists (the two affix files
are identical apart from a comment). The rule file is the parse-free subset of
LanguageTool's English rules — anything needing a part-of-speech tagger or
morphology is left out, and the trim script aborts on any XML construct the
interpreter (`app/spell/GrammarRules.cpp`) does not implement. Licences:
`LICENSES/SCOWL-LICENSE.txt`, `LICENSES/LanguageTool-LGPL-2.1.txt`.

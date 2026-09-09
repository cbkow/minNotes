---
title: Licenses & Credits
permalink: /licenses/
nav_order: 15
---

# Licenses & Credits

minNotes is free software under the
[GNU GPL-3.0-or-later](https://github.com/cbkow/minNotes/blob/main/LICENSE).
It ships with the third-party material below. The full license texts live in
[`LICENSES/`](https://github.com/cbkow/minNotes/tree/main/LICENSES) in the
source repository and inside the app (`Contents/Resources/Licenses` on macOS,
the `Licenses` folder beside the executable on Windows).

| Component | Role | License |
|---|---|---|
| Qt 6 | application framework | LGPL-3.0 |
| FFmpeg | video decoding | LGPL-2.1+ |
| KSyntaxHighlighting | code block colouring | MIT |
| ink-stroke-modeler | stroke smoothing | Apache-2.0 |
| SoundTouch | constant-pitch review speeds | LGPL-2.1 |
| miniz | package (.mnpkg) zip backend | MIT |
| SQLite | note storage | public domain |
| Sparkle / WinSparkle | updates | MIT |
| Phosphor Icons | icons | MIT |
| Aspekta | typeface | SIL OFL 1.1 |
| Hunspell 1.7.3 | spelling engine | LGPL-2.1 (of its MPL 1.1 / GPL 2.0 / LGPL 2.1 tri-license) |
| SCOWL word lists (2026.02.25) | the US + UK dictionary | SCOWL notice (below) |
| LanguageTool English rules (v6.8) | grammar rule data | LGPL-2.1+ |

## Spelling and grammar

**Hunspell** — Copyright (C) 2002–2022 Németh László and the Hunspell
contributors. Compiled into minNotes as a static library; minNotes as a whole
is distributed under the GPL, which the LGPL permits.
[hunspell.github.io](https://hunspell.github.io/)

**SCOWL (Spell Checker Oriented Word Lists)** — the dictionary is the union of
SCOWL's `en_US` and `en_GB-ise` Hunspell dictionaries. Its notice, reproduced
as the license requires:

> Copyright 2000–2026 by Kevin Atkinson
>
> Permission to use, copy, modify, distribute, and sell any part of SCOWLv2,
> or word lists created from it, is hereby granted without fee, provided that
> the above copyright notice appears in all copies and that both the above
> copyright notice and this notice appear in supporting documentation. Kevin
> Atkinson makes no representations about the suitability of this database
> for any purpose. It is provided "as is" without express or implied
> warranty.
>
> The affix file is a heavily modified version of the original english.aff
> file released as part of Geoff Kuenning's Ispell, Copyright 1993 Geoff
> Kuenning, covered by his BSD license.

SCOWL draws on 12dicts and ENABLE2K (public domain, with special credit to
Alan Beale) and on data from the Corpus of Contemporary American English.
[wordlist.aspell.net](https://wordlist.aspell.net/)

**LanguageTool** — the grammar checker interprets the subset of LanguageTool's
English `grammar.xml` that needs no part-of-speech tagger, about 1,700 rules.
Copyright (C) 2001–2026 Daniel Naber, Marcin Miłkowski and the LanguageTool
contributors, LGPL-2.1-or-later. Rule data only: no LanguageTool code is part
of minNotes. [languagetool.org](https://languagetool.org)

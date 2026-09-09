---
title: Spelling & Grammar
permalink: /spelling/
nav_order: 5
---

# Spelling & Grammar

minNotes checks your writing as you go, entirely on your machine. Nothing is
sent anywhere: the dictionary and the grammar rules ship inside the app.

## What gets checked

- **Text blocks** — paragraphs, headings, quotes, bullet, numbered and task
  lists — and the **text cells of tables**.
- Not checked: code blocks, inline `code`, choice chips, links' addresses,
  emails, and anything that looks like an identifier — words with digits or
  underscores (`A001_C002`, `HDR10`), ALLCAPS initialisms, camelCase names,
  timecodes (`01:02:15:04`).
- **US and UK spellings both pass.** The dictionary is a merged American +
  British list, so *color* and *colour*, *organize* and *organise* are all fine.

## Reading the underlines

| Underline | Meaning |
|---|---|
| Red dots | a possible spelling mistake |
| Blue dots | a grammar or typography issue (a doubled word, a confused pair such as *their is*, *could of*, a missing space, an odd capital) |

A word is never flagged while you are still typing it — the underline appears
once the caret leaves the word and the block has been quiet for a moment.

## Fixing things

Right-click an underlined word:

- **Suggestions** sit at the top of the menu (up to five). Click one to replace
  the word; it is a single undo step, and bold, links or colour on the word
  survive the fix.
- **Add to dictionary** teaches the word for good. Your words live in
  `spelling/user.dic` inside the app's data folder, one per line, so you can
  edit or share the list.
- **Ignore** hides that word until you quit.
- **Ignore rule: …** (grammar issues) switches that one rule off permanently.
  *Document ▸ Reset Ignored Rules* brings them all back.

## Turning it on and off

*Document ▸ Check Spelling* and *Document ▸ Check Grammar* are independent
toggles and remember their state.

## The grammar checker, honestly

The grammar tier is a rule set of about 1,700 pattern rules — typos that
spell a different word, confused pairs, punctuation spacing, casing of
well-known names, common phrase slips. It does not parse sentences, so it
will not tell you a clause is malformed, and it is quiet on the fragment-heavy
notes people actually write. When it does speak up it is usually right.

The dictionary comes from SCOWL and the rules from the LanguageTool project;
see [Licenses & Credits](/licenses/).

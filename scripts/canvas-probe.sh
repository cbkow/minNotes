#!/usr/bin/env bash
# Run the canvas probe offscreen (canvas B10): `scripts/canvas-probe.sh [fixture.md]`. Prints the
# probe's PHASE/FAIL/DONE lines and any TypeError/ReferenceError (a facade name BlockView reads that
# the CanvasEditorFacade doesn't provide). Exit code = the probe's fail count (capped at 100), or 1
# on QML errors.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${MN_BIN:-$ROOT/build/debug/app/minNotes.app/Contents/MacOS/minNotes}"
FIX="${1:-$ROOT/tests/fixtures/canvas-probe.md}"
LOG="$(mktemp -t canvas-probe)"
QT_QPA_PLATFORM=offscreen "$BIN" --canvas-probe="$FIX" >"$LOG" 2>&1
CODE=$?
grep -E "CANVAS-PROBE" "$LOG"
ERR=$(grep -E "TypeError|ReferenceError|is not defined|Cannot assign|Unable to assign" "$LOG" | grep -v "Cannot read property 'revision'\|formatFor\|currentItem\|'running' of null\|'progress' of null" | sort | uniq -c | sort -rn | head -10)
if [ -n "$ERR" ]; then echo "QML errors:"; echo "$ERR"; [ $CODE -eq 0 ] && CODE=1; fi
echo "log: $LOG"
exit $CODE

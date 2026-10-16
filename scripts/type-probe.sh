#!/usr/bin/env bash
# Run the keystroke type probe offscreen and tabulate its PHASE lines (A0 speed programme).
#
#   scripts/type-probe.sh [fixture.md] [label]
#
# Env: MN_BIN overrides the binary (default: the debug .app's inner executable).
# Prints one row per phase: keys, QML wall p50/p95 per key, the model's C++ splits, frame p95,
# and per-key counter rates (rehighlights, highlightBlocks, inkPaints, dbWrites). Exit code is the
# probe's (its failure count, capped at 100).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${MN_BIN:-$ROOT/build/debug/app/minNotes.app/Contents/MacOS/minNotes}"
FIX="${1:-$ROOT/tests/fixtures/type-probe.md}"
LABEL="${2:-$(date +%Y-%m-%d_%H%M)}"
LOG="$(mktemp -t type-probe)"

QT_QPA_PLATFORM=offscreen MN_TYPE_PROBE=1 "$BIN" --type-probe="$FIX" >"$LOG" 2>&1
CODE=$?

python3 - "$LOG" "$LABEL" <<'EOF'
import json, re, sys
log, label = sys.argv[1], sys.argv[2]
rows = []
for line in open(log, encoding="utf-8", errors="replace"):
    m = re.search(r"TYPE-PROBE PHASE (\S+) (\{.*\})\s*$", line)
    if m:
        rows.append((m.group(1), json.loads(m.group(2))))
    elif "TYPE-PROBE FAIL" in line or "TYPE-PROBE DONE" in line or "TYPE-PROBE START" in line:
        print(line.rstrip())
def pq(s): return f"{s['p50']:>6}/{s['p95']:<6}"
print(f"\n{label}")
z = {'p50':0,'p95':0}
print(f"{'phase':<11}{'keys':>5} {'key µs p50/p95':>15} {'insertText':>13} {'setCaret':>13} {'sc.anchor':>13} {'sc.focusRow':>13} {'sc.focusCol':>13} {'sc.sync':>13} {'sig.textIns':>13} {'sig.dataChg':>13} {'teOps n':>8} {'cascade':>13} {'casc n':>7} {'signal':>13} {'persist':>13} {'snapshot':>13} {'endTxn':>13} {'frame p95':>10} {'rehl/k':>7} {'hlBlk/k':>8} {'ink/k':>6} {'db/k':>5}")
for name, s in rows:
    k = max(1, s.get("keys", 0)); c = s.get("counters", {})
    print(f"{name:<11}{s.get('keys',0):>5} {pq(s.get('key', z)):>15} {pq(s.get('insertText', z)):>13} {pq(s.get('setCaret', z)):>13} {pq(s.get('sc.anchor', z)):>13} {pq(s.get('sc.focusRow', z)):>13} {pq(s.get('sc.focusCol', z)):>13} {pq(s.get('sc.sync', z)):>13} {pq(s.get('sig.textIns', z)):>13} {pq(s.get('sig.dataChg', z)):>13} {s.get('teOps', {'n':0})['n']:>8} {pq(s.get('cascade', z)):>13} {s.get('cascade', {'n':0})['n']:>7} {pq(s['signal']):>13} {pq(s['persist']):>13} {pq(s['snapshot']):>13} {pq(s['endTxn']):>13} {s.get('frame',{'p95':0})['p95']:>10} "
          f"{c.get('rehighlights',0)/k:>7.1f} {c.get('highlightBlocks',0)/k:>8.1f} {c.get('inkPaints',0)/k:>6.1f} {c.get('dbWrites',0)/k:>5.1f}")
if not rows:
    print("no PHASE lines — tail of the log:")
    print("".join(open(log, encoding="utf-8", errors="replace").readlines()[-30:]))
EOF
echo "log: $LOG"
exit $CODE

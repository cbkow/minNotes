#!/usr/bin/env bash
# Run the video probe offscreen (2026-10-18): a real clip (made here with the vendored ffmpeg) as a page block → its
# studio → play / Space; then as a CANVAS element → its studio; then selected on the canvas → the bar + the in-place
# surface; deselect stops it. Prints VIDEO-PROBE lines; exit code = the fail count. Capped at 40 s.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${MN_BIN:-$ROOT/build/debug/app/minNotes.app/Contents/MacOS/minNotes}"
CLIP="$(mktemp -t video-probe).mp4"
"$ROOT/external/ffmpeg/bin/ffmpeg" -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=24 \
  -f lavfi -i sine=frequency=440:sample_rate=48000 -t 12 -c:v h264_videotoolbox -pix_fmt yuv420p -c:a aac -shortest -y "$CLIP" || { echo "no clip"; exit 2; }
LOG="$(mktemp -t video-probe)"
QT_QPA_PLATFORM=offscreen "$BIN" --video-probe="$CLIP" >"$LOG" 2>&1 &
PID=$!
for i in $(seq 1 40); do sleep 1; kill -0 $PID 2>/dev/null || break; done
kill $PID 2>/dev/null
grep "VIDEO-PROBE" "$LOG" | grep -v " state "
echo "log: $LOG"
FAILS=$(grep -c "VIDEO-PROBE FAIL" "$LOG")
grep -q "VIDEO-PROBE DONE" "$LOG" || { echo "VIDEO-PROBE did not finish"; exit 1; }
exit "$FAILS"

#!/bin/bash
# Distance test: how much of a near and a far talker reaches the final
# transcript, in a simulated meeting room. See README.md.
#
#   core/engine/tools/distance/run.sh [label] [scribatic_replay]
#
# label names the results (default "now"); the replay tool defaults to
# build/core/scribatic_replay, so build it first. Everything generated goes to
# build/distance/; reruns reuse the speech and recordings already there.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
HERE="$ROOT/core/engine/tools/distance"
WORK="$ROOT/build/distance"
LABEL="${1:-now}"
REPLAY="${2:-$ROOT/build/core/scribatic_replay}"
mkdir -p "$WORK"

if [ ! -x "$WORK/venv/bin/python" ]; then
  python3 -m venv "$WORK/venv"
  "$WORK/venv/bin/pip" install -q -r "$HERE/requirements.txt"
fi
PY="$WORK/venv/bin/python"
"$PY" "$HERE/fetch_speech.py" "$WORK"
[ -f "$WORK/refs.json" ] || "$PY" "$HERE/make.py" "$WORK"

OUT="$WORK/results-$LABEL"
mkdir -p "$OUT"
for wav in "$WORK"/recordings/*.wav; do
  out="$OUT/$(basename "$wav" .wav).txt"
  [ -s "$out" ] && continue
  (cd "$ROOT/build/core" && SCRIBATIC_ROOT="$ROOT" "$REPLAY" "$wav" > "$out" 2>&1) || true
done
echo "== $LABEL"
"$PY" "$HERE/score.py" "$WORK" "$OUT"

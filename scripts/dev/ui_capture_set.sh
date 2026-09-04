#!/usr/bin/env bash
# Capture every UI state at every shape the app is actually seen in, into one
# directory — the unit a before/after comparison works on.
#
# This exists because "the UI still looks fine" is not a claim anyone can check
# later. A performance change that is supposed to alter nothing on screen has a
# testable form: the same pixels. So take the shots before the change, take
# them after, and diff. ui_diff.sh does the second half.
#
# The fixture is not optional. On a machine with no music the grid is empty,
# five states report themselves unreachable, and — the part that actually bites
# — the per-tile text layout never runs, so a diff of those PNGs would call a
# broken text change identical. See PlayerWindow::captureLoadFixture.
#
#   scripts/dev/ui_capture_set.sh /tmp/before
#   ...change something...
#   scripts/dev/ui_capture_set.sh /tmp/after
#   scripts/dev/ui_diff.sh /tmp/before /tmp/after
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:?usage: ui_capture_set.sh OUTPUT_DIR [ALBUM_COUNT]}"
ALBUMS="${2:-240}"
BIN="$REPO/build/linux_debug/gui/matrix_ui_capture"

[ -x "$BIN" ] || {
    echo "no matrix_ui_capture at $BIN" >&2
    echo "build it:  cmake --build build/linux_debug --target matrix_ui_capture" >&2
    exit 1
}

# The four shapes that exercise genuinely different layout code, not four
# arbitrary numbers: a desktop window, a HiDPI desktop window (metrics_.scale
# above 1.0), the phone in Vertical (bar A on top — the orientation that has
# already produced one layout bug), and the phone rotated.
FRAMES=(1920x1080 2560x1440 720x1640 1640x720)

mkdir -p "$OUT"
for f in "${FRAMES[@]}"; do
    # cd so the tool finds its assets/, fonts/ and eq_profiles.json, which it
    # resolves relative to its own executable.
    # A non-zero exit means "some state was unreachable", not "the run broke":
    # 3a-playlists-row-hover reads its rows from the DB's play history rather
    # than from albums_, so a fixture cannot reach it and a dev harness has no
    # business writing rows into a database to fix that. What must not happen
    # silently is a state that used to draw and now does not — ui_diff.sh
    # catches that as MISSING, which is the right place for it.
    (cd "$(dirname "$BIN")" && "$BIN" --out "$OUT/$f" --frame "$f" --fixture "$ALBUMS") \
        > "$OUT/$f.log" 2>&1 || true
    n=$(find "$OUT/$f" -name '*.png' 2>/dev/null | wc -l)
    [ "$n" -gt 0 ] || { echo "capture produced nothing at $f — see $OUT/$f.log" >&2; exit 1; }
    echo "  $f: $n states"
done
echo "captured into $OUT"

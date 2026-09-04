#!/usr/bin/env bash
# Compare two ui_capture_set.sh output directories and say, per state, whether
# a single pixel moved.
#
# This is the pixel-identity gate for the performance work: every change that
# is supposed to be invisible has to come through here clean. A difference is
# not automatically a bug — but it is automatically something to explain before
# the change lands.
#
#   scripts/dev/ui_diff.sh /tmp/before /tmp/after
set -euo pipefail

A="${1:?usage: ui_diff.sh BEFORE_DIR AFTER_DIR}"
B="${2:?usage: ui_diff.sh BEFORE_DIR AFTER_DIR}"

same=0; differ=0; missing=0
declare -a changed=()

# Compare on the shot, not on the log: the logs carry timings and paths that
# differ between runs by design and would drown the signal.
while IFS= read -r -d '' pa; do
    rel="${pa#"$A"/}"
    pb="$B/$rel"
    if [ ! -f "$pb" ]; then
        echo "MISSING  $rel"; missing=$((missing + 1)); continue
    fi
    if cmp -s "$pa" "$pb"; then
        same=$((same + 1))
    else
        differ=$((differ + 1)); changed+=("$rel")
    fi
done < <(find "$A" -name '*.png' -print0 | sort -z)

# Shots that exist only in the AFTER run — a state that used to be unreachable
# and now draws is a change too, and one worth noticing.
while IFS= read -r -d '' pb; do
    rel="${pb#"$B"/}"
    [ -f "$A/$rel" ] || { echo "NEW      $rel"; missing=$((missing + 1)); }
done < <(find "$B" -name '*.png' -print0 | sort -z)

for c in "${changed[@]}"; do echo "DIFFERS  $c"; done

echo "----"
echo "identical: $same   differs: $differ   missing/new: $missing"
[ "$differ" -eq 0 ] && [ "$missing" -eq 0 ] && { echo "PIXEL-IDENTICAL"; exit 0; }
exit 1

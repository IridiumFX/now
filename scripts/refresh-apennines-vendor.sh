#!/usr/bin/env bash
# refresh-apennines-vendor.sh
#
# now's vendored apennines sources are flattened — every canonical
# `apennines/src/main/c/<tier>/<sub>/<file>.c` lives at
# `now/lib/apennines/src/main/c/<file>.c`. Headers keep the tier
# structure under `now/lib/apennines/src/main/h/apennines/<tier>/<sub>/<file>.h`.
#
# This script re-fetches every vendored file from the canonical sibling
# repo, mapping paths as above. Run it whenever canonical apennines
# moves and the vendored copy needs to catch up.
#
# Usage:
#   scripts/refresh-apennines-vendor.sh [--canonical PATH] [--dry-run]
#
# Defaults to ../apennines (sibling-of-now layout). Exits non-zero if
# any vendored file has no canonical match (i.e. apennines deleted or
# renamed it — surfaces drift that needs manual attention).

set -euo pipefail

CANONICAL="../apennines"
DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --canonical) CANONICAL="$2"; shift 2;;
        --dry-run)   DRY=1; shift;;
        -h|--help)
            sed -n '2,/^$/p' "$0"; exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NOW_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CANONICAL="$(cd "$CANONICAL" 2>/dev/null && pwd || echo "$CANONICAL")"

if [ ! -d "$CANONICAL/src/main/c" ]; then
    echo "error: $CANONICAL/src/main/c not found (use --canonical to point at the apennines repo)" >&2
    exit 2
fi

VENDOR_C="$NOW_ROOT/lib/apennines/src/main/c"
VENDOR_H="$NOW_ROOT/lib/apennines/src/main/h"

if [ ! -d "$VENDOR_C" ] || [ ! -d "$VENDOR_H" ]; then
    echo "error: vendored apennines tree missing under $NOW_ROOT/lib/apennines" >&2
    exit 2
fi

copy() {
    local src="$1" dst="$2"
    if [ "$DRY" = 1 ]; then
        if ! diff -q "$src" "$dst" > /dev/null 2>&1; then
            echo "would refresh: ${dst#$NOW_ROOT/}"
        fi
    else
        cp "$src" "$dst"
    fi
}

# --- .c files (flattened) ---
n_c=0
miss_c=0
for vendored in "$VENDOR_C"/*.c; do
    [ -f "$vendored" ] || continue
    bn="$(basename "$vendored")"
    matches=$(find "$CANONICAL/src/main/c" -name "$bn" -type f 2>/dev/null)
    count=$(printf "%s\n" "$matches" | grep -c .)
    if [ "$count" = 0 ]; then
        echo "MISSING in canonical: $bn (no matching path)" >&2
        miss_c=$((miss_c + 1))
        continue
    fi
    if [ "$count" -gt 1 ]; then
        echo "AMBIGUOUS in canonical: $bn matches $count files — pick manually" >&2
        printf "  %s\n" $matches >&2
        miss_c=$((miss_c + 1))
        continue
    fi
    copy "$matches" "$vendored"
    n_c=$((n_c + 1))
done

# --- .h files (tier-structured) ---
n_h=0
miss_h=0
while IFS= read -r vendored; do
    rel="${vendored#$VENDOR_H/}"
    src="$CANONICAL/src/main/h/$rel"
    if [ ! -f "$src" ]; then
        echo "MISSING in canonical: src/main/h/$rel" >&2
        miss_h=$((miss_h + 1))
        continue
    fi
    copy "$src" "$vendored"
    n_h=$((n_h + 1))
done < <(find "$VENDOR_H" -name "*.h" -type f)

echo
echo "refreshed: $n_c .c files, $n_h .h files"
if [ "$miss_c" -gt 0 ] || [ "$miss_h" -gt 0 ]; then
    echo "warnings: $miss_c .c misses, $miss_h .h misses" >&2
    exit 1
fi

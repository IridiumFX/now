#!/usr/bin/env bash
# refresh-apennines-vendor.sh
#
# Vendored apennines mirrors the canonical repo's tier layout
# (t1/.../*, t2/.../*, ...) verbatim under lib/apennines/src/main/{c,h}/.
# Refreshing is just per-file copy along identical relative paths — no
# basename remapping, no flattening. Cherry-picking a single canonical
# file becomes a one-line cp.
#
# Usage:
#   scripts/refresh-apennines-vendor.sh [--canonical PATH] [--dry-run]
#
# Defaults to ../apennines (sibling-of-now layout). Exits non-zero if
# any vendored file is missing from canonical (rename or deletion that
# needs manual attention).

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

VENDOR_ROOT="$NOW_ROOT/lib/apennines/src/main"
if [ ! -d "$VENDOR_ROOT/c" ] || [ ! -d "$VENDOR_ROOT/h" ]; then
    echo "error: vendored apennines tree missing under $NOW_ROOT/lib/apennines" >&2
    exit 2
fi

copy_one() {
    local rel="$1" src="$2" dst="$3"
    if [ "$DRY" = 1 ]; then
        if ! diff -q "$src" "$dst" > /dev/null 2>&1; then
            echo "would refresh: $rel"
        fi
    else
        cp "$src" "$dst"
    fi
}

refresh_tree() {
    local sub="$1"
    local n=0 miss=0
    while IFS= read -r vendored; do
        local rel="${vendored#$VENDOR_ROOT/$sub/}"
        local src="$CANONICAL/src/main/$sub/$rel"
        if [ ! -f "$src" ]; then
            echo "MISSING in canonical: src/main/$sub/$rel" >&2
            miss=$((miss + 1))
            continue
        fi
        copy_one "lib/apennines/src/main/$sub/$rel" "$src" "$vendored"
        n=$((n + 1))
    done < <(find "$VENDOR_ROOT/$sub" -type f \( -name "*.c" -o -name "*.h" \))
    echo "  $sub: refreshed $n files, $miss misses"
    return $miss
}

echo "refreshing vendored apennines from $CANONICAL"
miss_total=0
refresh_tree c || miss_total=$((miss_total + $?))
refresh_tree h || miss_total=$((miss_total + $?))

if [ "$miss_total" -gt 0 ]; then
    echo "warnings: $miss_total files missing in canonical (renamed or deleted)" >&2
    exit 1
fi

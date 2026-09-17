#!/usr/bin/env bash
#
# Fetches the OpenFlights reference data used by the demo.
#
# The files are NOT committed. OpenFlights is published under the Open Database
# License, whose share-alike terms apply to derived databases that are made
# public; downloading on demand sidesteps redistribution entirely, and keeps a
# 4 MB dataset out of the repository.
#
#   ./scripts/fetch-openflights.sh              # fetch what is missing
#   ./scripts/fetch-openflights.sh --force      # re-fetch everything
#   ./scripts/fetch-openflights.sh --dir path   # somewhere other than data/openflights
#
# Data © OpenFlights contributors, Open Database License (ODbL).
# https://openflights.org/data.php
# ---------------------------------------------------------------------------
set -euo pipefail

BASE_URL="https://raw.githubusercontent.com/jpatokal/openflights/master/data"
FILES=(airports airlines routes planes countries)

# Sanity floor per file, so a captive-portal HTML page or a truncated transfer
# is caught here rather than surfacing as a parse error 60k rows later.
declare -A MIN_BYTES=(
    [airports]=500000
    [airlines]=200000
    [routes]=1000000
    [planes]=4000
    [countries]=3000
)

target_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/data/openflights"
force=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) force=1; shift ;;
        --dir) target_dir="$2"; shift 2 ;;
        -h|--help) sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL --retry 3 --retry-delay 2 -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q --tries=3 -O "$2" "$1"; }
else
    echo "error: neither curl nor wget is installed" >&2
    exit 1
fi

mkdir -p "$target_dir"
echo "OpenFlights data -> $target_dir"

for name in "${FILES[@]}"; do
    dest="$target_dir/$name.dat"

    if [[ -s "$dest" && $force -eq 0 ]]; then
        printf '  %-10s skipped (already present; --force to refresh)\n' "$name"
        continue
    fi

    tmp="$dest.part"
    if ! fetch "$BASE_URL/$name.dat" "$tmp"; then
        rm -f "$tmp"
        echo "  $name FAILED to download" >&2
        exit 1
    fi

    size=$(wc -c < "$tmp")
    if (( size < ${MIN_BYTES[$name]} )); then
        rm -f "$tmp"
        echo "  $name looks truncated ($size bytes, expected at least ${MIN_BYTES[$name]})" >&2
        exit 1
    fi

    mv "$tmp" "$dest"
    printf '  %-10s %7d rows  %8d bytes\n' "$name" "$(wc -l < "$dest")" "$size"
done

# A record of what is actually on disk, since the upstream files change without
# notice and carry no version of their own.
{
    echo "# OpenFlights data fetched $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# source: $BASE_URL"
    echo "# licence: Open Database License (ODbL) -- https://openflights.org/data.php"
    echo "#"
    printf '%-12s %10s %10s\n' "file" "rows" "bytes"
    for name in "${FILES[@]}"; do
        printf '%-12s %10d %10d\n' "$name.dat" "$(wc -l < "$target_dir/$name.dat")" \
            "$(wc -c < "$target_dir/$name.dat")"
    done
} > "$target_dir/MANIFEST"

cat <<'NOTE'

Files are headerless and use \N for null, so read them with:

    read_csv_file<Airport>(path, {.has_header = false, .null_marker = "\\N"});

Data (c) OpenFlights contributors, Open Database License (ODbL).
Acknowledge the source in anything published from it.
NOTE

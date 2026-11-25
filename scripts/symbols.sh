#!/usr/bin/env bash
# Rebuilds recomp/amp_symbols.txt (SCUS_972.58 has no symbols) with BozoEngine's matcher, from reference executables
# that keep theirs and the names read by hand in recomp/amp_symbols_reviewed.txt. Report: build/bozo/amp_symbols_report.txt
# Usage: scripts/symbols.sh <reference.elf>...   (the map used Dark Cloud's SCUS_971.11, Twisted Metal: Black's
#        SCUS_971.01 and Onimusha: Warlords' SLUS_200.18: the player's own discs, never committed)
source "$(dirname "$0")/common.sh"
[ $# -gt 0 ] || { echo "usage: $0 <reference.elf>..."; exit 1; }
mkdir -p "$ROOT/build/bozo"
refs=()
for ref in "$@"; do refs+=(--ref "$ref"); done
python -I "$ROOT/BozoEngine/tools/symbolmap/bozo_symbolmap.py" engine-names "$ROOT/BozoEngine" "$ROOT/build/bozo/want.txt"
python -I "$ROOT/BozoEngine/tools/symbolmap/bozo_symbolmap.py" match "$ELF" "${refs[@]}" \
    --reviewed "$ROOT/recomp/amp_symbols_reviewed.txt" --want "$ROOT/build/bozo/want.txt" \
    --out "$ROOT/recomp/amp_symbols.txt" --report "$ROOT/build/bozo/amp_symbols_report.txt"
head -2 "$ROOT/build/bozo/amp_symbols_report.txt"

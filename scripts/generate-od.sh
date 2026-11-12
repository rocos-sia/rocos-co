#!/usr/bin/env bash
# Rebuild the committed dictionary using the official CANopenEditor v4.2.3 CLI.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${EDSSHARP_DLL:?Set EDSSHARP_DLL to the release net8.0/EDSSharp.dll}"
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT
TZ=UTC LANG=C.UTF-8 "${DOTNET:-dotnet}" "$EDSSHARP_DLL" --infile od/master.xdd --outfile "$temp_dir/OD.c" --type CanOpenNodeV4
for name in OD.c OD.h; do
    test -s "$temp_dir/$name"
    sed -e 's/[[:space:]]*$//' "$temp_dir/$name" > "od/generated/$name"
done

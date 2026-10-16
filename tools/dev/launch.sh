#!/usr/bin/env bash
# Rebuilds and runs the J4 launcher (aa.host.Launcher) on an APK with the
# service trace, then prints the end of the output and the exception chain.
#
#   tools/dev/launch.sh [apk] [package]
#
# Default: the basic-activity sample. Environment: AA_BUILD (build directory,
# default build), AA_INPUTS (output of tools/fetch-android-inputs.sh: libcore17/
# and aosp17/system.img), TAIL (lines of stdout shown, default 40).
set -euo pipefail
build=${AA_BUILD:-build}
inputs=${AA_INPUTS:?AA_INPUTS: dossier produit par tools/fetch-android-inputs.sh}
apk=${1:-$build/samples/basic-activity.apk}
pkg=${2:-com.anyandroid.samples.basic}
z() { echo "Z:$(realpath "$1")"; }
ninja -C "$build" 2>&1 | grep -E "error|warning" -A3 | head -20 || true
data=$(mktemp -d)
WINEDEBUG=-all timeout 900 wine "$build/apps/cli/anyandroid.exe" exec \
  --bootclasspath "$(z "$inputs/libcore17")" --system-image "$(z "$inputs/aosp17/system.img")" \
  --data-dir "$(z "$data")" --trace-services "$apk" aa.host.Launcher "$pkg" \
  > "$data/out.txt" 2> "$data/err.txt" || true
tail -"${TAIL:-40}" "$data/out.txt"
grep -E "ERROR|Exception|Caused|^\s+at " "$data/err.txt" | head -"${ERR:-40}" || true
echo "(sorties complètes : $data)"

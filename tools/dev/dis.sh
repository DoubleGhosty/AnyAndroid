#!/usr/bin/env bash
# Disassembles one class of a jar of the Android image with AnyAndroid's own
# disassembler (anyandroid inspect --disasm), cached per class.
#
#   tools/dev/dis.sh <jar> <java class> [method substring]
#
# Example: tools/dev/dis.sh "$AA_INPUTS/dex/framework.jar" android.graphics.Typeface init
# Environment: AA_CLI (default "wine build/apps/cli/anyandroid.exe"),
#              AA_DIS_CACHE (default /tmp/aa-dis-cache).
# The jar comes from the image (`anyandroid image cat <system.img>
# /system/framework/framework.jar --output <file>`); never commit it.
set -euo pipefail
[ $# -ge 2 ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
jar=$1 cls=$2 pat=${3:-}
read -r -a cli <<< "${AA_CLI:-wine build/apps/cli/anyandroid.exe}"
cache=${AA_DIS_CACHE:-/tmp/aa-dis-cache}/$(basename "$jar")
mkdir -p "$cache"
out=$cache/$cls.txt
path=$jar
[ "${cli[0]}" = wine ] && path=Z:$(realpath "$jar")
if [ ! -s "$out" ]; then
  WINEDEBUG=-all "${cli[@]}" inspect "$path" --disasm "$cls" 2>/dev/null | tr -d '\r' |
    awk -v c="classe $cls " 'index($0,c)==1{p=1} p&&/^classe /&&index($0,c)!=1{p=0} p' > "$out"
fi
if [ -n "$pat" ]; then
  awk -v m="$pat" '/^  méthode /{p=index($0,m)>0} p' "$out"
else
  grep '^  méthode ' "$out"
fi

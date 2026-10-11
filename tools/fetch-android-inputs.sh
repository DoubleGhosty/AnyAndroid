#!/usr/bin/env bash
# Downloads the Android inputs AnyAndroid is tested against, OUTSIDE the
# repository (ADR-002, ADR-004: nothing extracted from an Android image and no
# Google binary is ever committed):
#   - the official Android 17 GSI x86_64 (AOSP) from developer.android.com:
#     system.img and the boot class path extracted by `anyandroid image`;
#   - optionally the GSI x86_64 with GMS, from which the GApps (GmsCore, GSF,
#     Play Store) and their permission files are extracted;
#   - optionally microG from its official F-Droid repository (microg.org);
#   - optionally R8/D8 from Google's Maven repository, to build AnyAndroid's
#     host code in Java (ADR-006; -DAA_R8_JAR=<dir>/tools/r8-<version>.jar).
#
# Running it means accepting the "GSI License Agreement" shown on
# https://developer.android.com/topic/generic-system-image/releases.
#
#   tools/fetch-android-inputs.sh <output dir> [--gms] [--microg] [--r8]
#
# Needs curl, unzip, sha256sum, python3 and the AnyAndroid CLI: set AA_CLI to
# its path (default build/apps/cli/anyandroid.exe; on Linux, AA_CLI="wine
# /path/anyandroid.exe" — paths are then passed as Z:/...).
set -euo pipefail

GSI_BASE=https://dl.google.com/developers/android/cinnamonbun/images/gsi
# Android 17 QPR3 (CP3A.260905.010), with the SHA-256 published by Google.
AOSP_ZIP=aosp_x86_64-exp-CP3A.260905.010-16176746-81e37ae9.zip
AOSP_SHA=81e37ae9e9280497c92fbda758b998a7dfab91ab9de000021622afa868a43b70
GMS_ZIP=gsi_gms_x86_64-exp-CP3A.260905.010-16176746-3eb28f1b.zip
GMS_SHA=3eb28f1b234d0b7cea80d9cb890cc96eb84d530e2b91ceefb9009461269ae58a
MICROG_REPO=https://microg.org/fdroid/repo
R8_VERSION=9.5.23
R8_URL=https://dl.google.com/dl/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar
R8_SHA=748ab0ef66e85b54cec5ad83d168999db9e3dad8c8f63170e836eff195cbd09b

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ $# -ge 1 ] || usage
OUT=$1; shift
WANT_GMS=0; WANT_MICROG=0; WANT_R8=0
for a in "$@"; do
  case $a in
    --gms) WANT_GMS=1 ;;
    --microg) WANT_MICROG=1 ;;
    --r8) WANT_R8=1 ;;
    *) usage ;;
  esac
done
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
read -r -a CLI <<< "${AA_CLI:-build/apps/cli/anyandroid.exe}"

# Path as the CLI sees it (Wine maps / to Z:).
cli_path() { if [ "${CLI[0]}" = wine ]; then echo "Z:$1"; else echo "$1"; fi; }
aa() { "${CLI[@]}" "$@"; }

fetch() {  # fetch <url> <file> <sha256>
  local url=$1 file=$2 sha=$3
  if [ -f "$file" ] && echo "$sha  $file" | sha256sum -c --status; then return; fi
  for i in 1 2 3 4 5; do
    curl -fsS -C - --retry 3 -o "$file" "$url" && break
    sleep $((i * 4))
  done
  echo "$sha  $file" | sha256sum -c --quiet || { echo "empreinte invalide : $file" >&2; exit 1; }
}

gsi() {  # gsi <zip> <sha> <dir>: downloads, keeps system.img + build.prop, drops the zip
  local zip=$1 sha=$2 dir=$3
  [ -f "$dir/system.img" ] && return
  fetch "$GSI_BASE/$zip" "$OUT/$zip" "$sha"
  mkdir -p "$dir"
  unzip -o -q "$OUT/$zip" system.img build.prop -d "$dir"
  rm -f "$OUT/$zip"
}

echo "== GSI AOSP x86_64 (Android 17)"
gsi "$AOSP_ZIP" "$AOSP_SHA" "$OUT/aosp17"
if [ ! -f "$OUT/libcore17/bootclasspath.txt" ]; then
  aa image bootclasspath "$(cli_path "$OUT/aosp17/system.img")" --output "$(cli_path "$OUT/libcore17")"
fi

if [ $WANT_GMS = 1 ]; then
  echo "== GSI GMS x86_64 (Android 17) et GApps"
  gsi "$GMS_ZIP" "$GMS_SHA" "$OUT/gms17"
  img=$(cli_path "$OUT/gms17/system.img")
  mkdir -p "$OUT/gapps17"
  for d in /system/product/priv-app/PrebuiltGmsCore /system/product/priv-app/Phonesky \
           /system/system_ext/priv-app/GoogleServicesFramework /system/product/priv-app/ConfigUpdater \
           /system/product/app/GoogleCalendarSyncAdapter /system/product/app/GoogleContacts; do
    for f in $(aa image ls "$img" "$d" | awk '$1 == "-" { print $NF }' | tr -d '\r'); do
      aa image cat "$img" "$d/$f" --output "$(cli_path "$OUT/gapps17/$f")" > /dev/null
    done
  done
  for d in /system/product/etc/permissions /system/product/etc/sysconfig \
           /system/product/etc/default-permissions /system/system_ext/etc/permissions; do
    dest="$OUT/gapps17/etc${d#/system}"
    mkdir -p "$dest"
    for f in $(aa image ls "$img" "$d" | awk '$1 == "-" { print $NF }' | tr -d '\r'); do
      aa image cat "$img" "$d/$f" --output "$(cli_path "$dest/$f")" > /dev/null
    done
  done
fi

if [ $WANT_MICROG = 1 ]; then
  echo "== microG (dépôt F-Droid officiel)"
  mkdir -p "$OUT/microg"
  curl -fsS -o "$OUT/microg/index-v1.jar" "$MICROG_REPO/index-v1.jar"
  python3 - "$OUT/microg" "$MICROG_REPO" <<'EOF'
import hashlib, json, os, sys, urllib.request, zipfile
out, repo = sys.argv[1], sys.argv[2]
index = json.loads(zipfile.ZipFile(os.path.join(out, "index-v1.jar")).read("index-v1.json"))
for pkg, versions in index["packages"].items():
    v = max(versions, key=lambda x: x.get("versionCode", 0))
    path = os.path.join(out, v["apkName"])
    if not os.path.exists(path) or hashlib.sha256(open(path, "rb").read()).hexdigest() != v["hash"]:
        urllib.request.urlretrieve(f"{repo}/{v['apkName']}", path)
    ok = hashlib.sha256(open(path, "rb").read()).hexdigest() == v["hash"]
    print(f"{v['apkName']} : {'ok' if ok else 'EMPREINTE INVALIDE'}")
    if not ok:
        sys.exit(1)
EOF
fi

if [ $WANT_R8 = 1 ]; then
  echo "== R8/D8 $R8_VERSION (Google Maven)"
  mkdir -p "$OUT/tools"
  fetch "$R8_URL" "$OUT/tools/r8-$R8_VERSION.jar" "$R8_SHA"
fi

echo "Terminé : $OUT"
echo "Tests G2/G3 : cmake -S . -B build -DAA_LIBCORE_DIR=$OUT/libcore17 -DAA_SYSTEM_IMAGE=$OUT/aosp17/system.img"
[ $WANT_R8 = 1 ] && echo "Code d'hôte Java : ajouter -DAA_R8_JAR=$OUT/tools/r8-$R8_VERSION.jar (JDK >= 17 requis)"

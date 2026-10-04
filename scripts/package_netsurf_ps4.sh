#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN must point to OpenOrbis/PS4Toolchain}"

TITLE="PS4 Browser NetSurf PoC"
VERSION="01.00"
TITLE_ID="PBNF00001"
CONTENT_ID="IV0000-PBNF00001_00-NETSURFPS4POC001"

ELF="$ROOT/build/independent/netsurf-ps4.elf"
OUTDIR="$ROOT/build/package"
PKGROOT="$OUTDIR/root"
TOOLBIN="$OO_PS4_TOOLCHAIN/bin/linux"
MODULE_DATA="$OO_PS4_TOOLCHAIN/src/modules"

test -s "$ELF"
rm -rf "$OUTDIR"
mkdir -p "$PKGROOT/sce_sys/about" "$PKGROOT/sce_module" "$PKGROOT/res"

echo "==> create eboot.bin from independent NetSurf ELF"
"$TOOLBIN/create-fself"   -in="$ELF"   -out="$OUTDIR/netsurf-ps4.oelf"   --eboot "$PKGROOT/eboot.bin"   --paid 0x3800000000000011

test -s "$PKGROOT/eboot.bin"

cp "$MODULE_DATA/right.sprx" "$PKGROOT/sce_sys/about/right.sprx"
cp "$MODULE_DATA/libSceFios2.prx" "$PKGROOT/sce_module/libSceFios2.prx"
cp "$MODULE_DATA/libc.prx" "$PKGROOT/sce_module/libc.prx"

python3 "$ROOT/tools/generate_icon.py" "$PKGROOT/sce_sys/icon0.png"

SFO="$PKGROOT/sce_sys/param.sfo"
"$TOOLBIN/PkgTool.Core" sfo_new "$SFO"
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" APP_TYPE --type Integer --maxsize 4 --value 1
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4 --value 0
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" CATEGORY --type Utf8 --maxsize 4 --value gd
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4 --value 0
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$TOOLBIN/PkgTool.Core" sfo_setentry "$SFO" VERSION --type Utf8 --maxsize 8 --value "$VERSION"

# NetSurf was compiled with NETSURF_FB_RESPATH=/app0/res.
# Copy the actual dereferenced files from the cross-build artifact.
cp -a "$ROOT/build/independent/res/." "$PKGROOT/res/"

cd "$PKGROOT"
mapfile -t files < <(find . -type f -printf '%P\n' | LC_ALL=C sort)
test "${#files[@]}" -gt 6

file_list=""
for file in "${files[@]}"; do
  file_list+="${file} "
done

echo "==> create GP4 with ${#files[@]} files"
"$TOOLBIN/create-gp4"   -out "$PKGROOT/pkg.gp4"   --content-id="$CONTENT_ID"   --files "$file_list"

echo "==> build PKG"
"$TOOLBIN/PkgTool.Core" pkg_build "$PKGROOT/pkg.gp4" "$OUTDIR"

pkg="$(find "$OUTDIR" -maxdepth 1 -type f -name '*.pkg' -print -quit)"
test -n "$pkg"
cp "$pkg" "$ROOT/PS4_Browser_NetSurf_PoC_v1.0.pkg"
sha256sum "$ROOT/PS4_Browser_NetSurf_PoC_v1.0.pkg" > "$ROOT/PS4_Browser_NetSurf_PoC_v1.0.pkg.sha256"

echo "SUCCESS: $ROOT/PS4_Browser_NetSurf_PoC_v1.0.pkg"

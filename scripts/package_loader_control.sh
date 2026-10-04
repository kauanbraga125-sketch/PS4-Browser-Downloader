#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN required}"

TOOLBIN="$OO_PS4_TOOLCHAIN/bin/linux"
MODULE_DATA="$OO_PS4_TOOLCHAIN/src/modules"
OUTDIR="$ROOT/build/loader-control-package"
PKGROOT="$OUTDIR/root"
ELF="$ROOT/build/loader-control/control.elf"

TITLE="PS4 Loader Control"
VERSION="01.00"
TITLE_ID="PBCN00001"
CONTENT_ID="IV0000-PBCN00001_00-LOADERCONTROL0001"

test -s "$ELF"
rm -rf "$OUTDIR"
mkdir -p "$PKGROOT/sce_sys/about" "$PKGROOT/sce_module"

"$TOOLBIN/create-fself" -in="$ELF" -out="$OUTDIR/control.oelf" --eboot "$PKGROOT/eboot.bin" --paid 0x3800000000000011
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

cd "$PKGROOT"
files="eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libSceFios2.prx sce_module/libc.prx"
"$TOOLBIN/create-gp4" -out pkg.gp4 --content-id="$CONTENT_ID" --files "$files"
"$TOOLBIN/PkgTool.Core" pkg_build pkg.gp4 "$OUTDIR"

pkg="$(find "$OUTDIR" -maxdepth 1 -type f -name '*.pkg' -print -quit)"
test -n "$pkg"
cp "$pkg" "$ROOT/PS4_Loader_Control_v1.0.pkg"
(
  cd "$ROOT"
  sha256sum PS4_Loader_Control_v1.0.pkg > PS4_Loader_Control_v1.0.pkg.sha256
)

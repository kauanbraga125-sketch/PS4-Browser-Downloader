#!/usr/bin/env bash
set -euo pipefail

# Reproducible first-stage NetSurf -> PS4 cross build.
# This intentionally starts with a reduced feature set (no JS/images/video)
# so the first hardware milestone is a real HTML/CSS browser, not a mock UI.
# Once this links and runs on hardware, features are re-enabled one by one.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN must point to OpenOrbis/PS4Toolchain}"

WORK="${WORK:-/tmp/netsurf-ps4}"
SRC="$WORK/src"
PREFIX="$WORK/prefix"
TOOLS="$WORK/tools"
HOST="x86_64-pc-freebsd12-elf"
BUILD="x86_64-linux-gnu"
NSBUILD="$SRC/buildsystem"

rm -rf "$WORK"
mkdir -p "$SRC" "$PREFIX" "$TOOLS"

fetch_repo() {
    local repo="$1" sha="$2" dst="$3"
    echo "==> fetch $repo @ $sha"
    curl -fL --retry 5 --retry-all-errors       "https://github.com/${repo}/archive/${sha}.tar.gz"       -o "$WORK/${dst}.tar.gz"
    mkdir -p "$SRC/$dst"
    tar xzf "$WORK/${dst}.tar.gz" -C "$SRC/$dst" --strip-components=1
}

fetch_repo netsurf-browser/buildsystem 0005ae300283ff01c2e2b05e7376b3e55dea21f7 buildsystem
fetch_repo netsurf-browser/libwapcaplet c7c128d3eb3223b216c974471f82e9337fbcf4ba libwapcaplet
fetch_repo netsurf-browser/libparserutils 6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca libparserutils
fetch_repo netsurf-browser/libhubbub 6651b8cf87a4aa87bcdb2ff024a02659cd3f9402 libhubbub
fetch_repo netsurf-browser/libcss 499f1c4601ad39942fd1b2204053a387bec9b989 libcss
fetch_repo netsurf-browser/libdom f69781e1f062444b5af3f62d431d7d94018da53b libdom
fetch_repo netsurf-browser/libnsutils 0bd39060740b6163bd50875326654a722df97eb2 libnsutils
fetch_repo netsurf-browser/libnsfb b701cdce7241c3747ccd78658a365db0983ebe24 libnsfb
fetch_repo netsurf-browser/netsurf 39da3c3a40af4566d86500ff3052dfdc7f9a0378 netsurf

cat > "$TOOLS/ps4-pkg-config" <<EOF
#!/usr/bin/env bash
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=
exec pkg-config "\$@"
EOF
chmod +x "$TOOLS/ps4-pkg-config"

cat > "$TOOLS/ps4-gcc" <<EOF
#!/usr/bin/env bash
set -e
compile=0
for a in "\$@"; do
  case "\$a" in
    -c|-E|-S|-M|-MM|-MMD) compile=1 ;;
  esac
done
COMMON=(--target=$HOST -fPIC -funwind-tables -isysroot "$OO_PS4_TOOLCHAIN" -isystem "$OO_PS4_TOOLCHAIN/include" -I"$PREFIX/include" -I"$ROOT/ports/compat")
if [[ "\$compile" == 1 ]]; then
  exec clang-18 "\${COMMON[@]}" "\$@"
fi
exec clang-18 "\${COMMON[@]}" -fuse-ld=lld -nostdlib -Wl,-pie -Wl,--script="$OO_PS4_TOOLCHAIN/link.x" -Wl,--eh-frame-hdr -L"$PREFIX/lib" -L"$OO_PS4_TOOLCHAIN/lib" "\$@" "$OO_PS4_TOOLCHAIN/lib/crt1.o" -Wl,--start-group -lSDL2 -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lc -lkernel -Wl,--end-group
EOF
chmod +x "$TOOLS/ps4-gcc"

cat > "$TOOLS/ps4-g++" <<EOF
#!/usr/bin/env bash
exec "$TOOLS/ps4-gcc" -isystem "$OO_PS4_TOOLCHAIN/include/c++/v1" "\$@" -lc++
EOF
chmod +x "$TOOLS/ps4-g++"

# The NetSurf build system detects *gcc/*g++ under GCCSDK_INSTALL_CROSSBIN.
ln -sf ps4-gcc "$TOOLS/$HOST-gcc"
ln -sf ps4-g++ "$TOOLS/$HOST-g++"

export PKG_CONFIG="$TOOLS/ps4-pkg-config"
export PKGCONFIG="$TOOLS/ps4-pkg-config"

COMMON_CFLAGS="-isysroot $OO_PS4_TOOLCHAIN -isystem $OO_PS4_TOOLCHAIN/include -I$PREFIX/include -I$ROOT/ports/compat -fPIC -DORBIS -D_GNU_SOURCE"
COMMON_LDFLAGS="-L$PREFIX/lib -L$OO_PS4_TOOLCHAIN/lib"

build_lib() {
    local name="$1"
    echo
    echo "================ $name ================"
    cd "$SRC/$name"
    # OpenOrbis' public headers are still incomplete compared with a full
    # FreeBSD sysroot; warnings from those headers must not become hard errors.
    sed -i 's/-Werror//g' Makefile || true
    make -j2 install       PREFIX="$PREFIX"       NSSHARED="$NSBUILD"       HOST="$HOST"       BUILD="$BUILD"       CC="$TOOLS/ps4-gcc"       CXX="$TOOLS/ps4-g++"       AR=llvm-ar-18       BUILD_CC=cc       PKGCONFIG="$TOOLS/ps4-pkg-config"       CFLAGS="$COMMON_CFLAGS"       LDFLAGS="$COMMON_LDFLAGS"
}

build_lib libwapcaplet
build_lib libparserutils
build_lib libhubbub
build_lib libcss

# NetSurf requires the Hubbub DOM binding. The Expat and libxml bindings are
# unnecessary for the browser and would pull in unrelated libraries.
cat > "$SRC/libdom/Makefile.config.override" <<'EOF'
WITH_LIBXML_BINDING := no
WITH_EXPAT_BINDING := no
WITH_HUBBUB_BINDING := yes
EOF
build_lib libdom
build_lib libnsutils

# Replace libnsfb's SDL1-only optional surface with our SDL2/OpenOrbis surface.
cp "$ROOT/ports/libnsfb/ps4_sdl2.c" "$SRC/libnsfb/src/surface/ps4_sdl2.c"
cat > "$SRC/libnsfb/src/surface/Makefile" <<'EOF'
# PS4 port: keep RAM surface for offscreen bitmaps and use our SDL2 surface
# for the actual display. Do not compile the upstream SDL 1.2 backend.
DIR_SOURCES := surface.c ram.c ps4_sdl2.c
include $(NSBUILD)/Makefile.subdir
EOF
build_lib libnsfb

echo
echo "================ patch NetSurf ================"
cd "$SRC/netsurf"
cp "$ROOT/ports/netsurf/fetch_ps4.c" content/fetchers/ps4.c
cp "$ROOT/ports/netsurf/fetch_ps4.h" content/fetchers/ps4.h

python3 - <<'PY'
from pathlib import Path

p = Path("content/fetchers/Makefile")
s = p.read_text()
s = s.replace("S_FETCHERS_YES := data.c resource.c", "S_FETCHERS_YES := data.c resource.c ps4.c")
p.write_text(s)

p = Path("content/fetch.c")
s = p.read_text()
needle = '#include "content/fetchers/curl.h"'
if '#include "content/fetchers/ps4.h"' not in s:
    s = s.replace(needle, needle + '\n#include "content/fetchers/ps4.h"')
needle = '''nserror fetcher_init(void)
{
\tnserror ret;
'''
replacement = '''nserror fetcher_init(void)
{
\tnserror ret;

\tret = fetch_ps4_register();
\tif (ret != NSERROR_OK) {
\t\treturn ret;
\t}
'''
if "fetch_ps4_register();" not in s:
    s = s.replace(needle, replacement)
p.write_text(s)

# PS4-specific default framebuffer surface and resolution. Command-line
# overrides remain possible, but hardware boots directly into our SDL2 surface.
p = Path("frontends/framebuffer/gui.c")
s = p.read_text()
s = s.replace("static const char *fename;", 'static const char *fename = "ps4";')
# Do not allow auto-selection to replace the explicit PS4 surface.
s = s.replace(
'''static void
framebuffer_pick_default_fename(void *ctx, const char *name, enum nsfb_type_e type)
{
\tif (type < fetype) {''',
'''static void
framebuffer_pick_default_fename(void *ctx, const char *name, enum nsfb_type_e type)
{
\tif (fename != NULL && strcmp(fename, "ps4") == 0) return;
\tif (type < fetype) {''')
p.write_text(s)
PY

cat > Makefile.config <<EOF
# First real-browser hardware milestone.
override NETSURF_USE_CURL := NO
override NETSURF_USE_OPENSSL := NO
override NETSURF_USE_DUKTAPE := NO
override NETSURF_USE_BMP := NO
override NETSURF_USE_GIF := NO
override NETSURF_USE_JPEG := NO
override NETSURF_USE_JPEGXL := NO
override NETSURF_USE_PNG := NO
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_WEBP := NO
override NETSURF_USE_NSSVG := NO
override NETSURF_USE_NSPSL := NO
override NETSURF_USE_NSLOG := NO
override NETSURF_USE_UTF8PROC := NO
override NETSURF_USE_HARU_PDF := NO
override NETSURF_FS_BACKING_STORE := NO
override NETSURF_FB_FONTLIB := internal
override NETSURF_HOMEPAGE := "https://example.com/"
NETSURF_FB_RESPATH := /app0/res
NETSURF_FB_FONTPATH := /app0/res
EOF

# frontends/framebuffer/Makefile.tools discovers these wrappers.
export GCCSDK_INSTALL_ENV="$PREFIX"
export GCCSDK_INSTALL_CROSSBIN="$TOOLS"

# Our native SceHttp implementation supplies HTTP/HTTPS, so libcurl is
# intentionally absent. SDL2 is supplied by OpenOrbis.
make -j2 TARGET=framebuffer   CC="$TOOLS/ps4-gcc" CXX="$TOOLS/ps4-g++"   PKG_CONFIG="$TOOLS/ps4-pkg-config"   CFLAGS="$COMMON_CFLAGS"   LDFLAGS="$COMMON_LDFLAGS -Wl,--start-group -lSDL2 -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lc -lkernel -Wl,--end-group"

test -s nsfb
mkdir -p "$ROOT/build/independent"
cp nsfb "$ROOT/build/independent/netsurf-ps4.elf"

# Preserve the exact resource set required by the framebuffer frontend.
mkdir -p "$ROOT/build/independent/res"
cp -a frontends/framebuffer/res/. "$ROOT/build/independent/res/"

echo "SUCCESS: $ROOT/build/independent/netsurf-ps4.elf"
file "$ROOT/build/independent/netsurf-ps4.elf" || true

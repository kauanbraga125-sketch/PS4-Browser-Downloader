#!/usr/bin/env bash
set -euo pipefail

# Reproducible first-stage NetSurf -> PS4 cross build.
# Milestone 1 deliberately disables optional JS/image/video support.
# The produced program is still the real NetSurf HTML/CSS engine and does not
# invoke sceWebBrowserDialog or the Sony browser.

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
    local repo="$1"
    local sha="$2"
    local dst="$3"
    echo "==> fetch $repo @ $sha"
    curl -fL --retry 5 --retry-all-errors "https://github.com/${repo}/archive/${sha}.tar.gz" -o "$WORK/${dst}.tar.gz"
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
fetch_repo madler/zlib refs/tags/v1.3.1 zlib
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
exec clang-18 "\${COMMON[@]}" -fuse-ld=lld -nostdlib -Wl,-pie -Wl,--no-dynamic-linker -Wl,--script="$OO_PS4_TOOLCHAIN/link.x" -Wl,--eh-frame-hdr -L"$PREFIX/lib" -L"$OO_PS4_TOOLCHAIN/lib" "\$@" "$OO_PS4_TOOLCHAIN/lib/crt1.o" -Wl,--start-group -lSDL2 -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lc -lkernel -Wl,--end-group
EOF
chmod +x "$TOOLS/ps4-gcc"

cat > "$TOOLS/ps4-g++" <<EOF
#!/usr/bin/env bash
exec "$TOOLS/ps4-gcc" -isystem "$OO_PS4_TOOLCHAIN/include/c++/v1" "\$@" -lc++
EOF
chmod +x "$TOOLS/ps4-g++"

ln -sf ps4-gcc "$TOOLS/$HOST-gcc"
ln -sf ps4-g++ "$TOOLS/$HOST-g++"

export PKG_CONFIG="$TOOLS/ps4-pkg-config"
export PKGCONFIG="$TOOLS/ps4-pkg-config"

COMMON_CFLAGS="-isysroot $OO_PS4_TOOLCHAIN -isystem $OO_PS4_TOOLCHAIN/include -I$PREFIX/include -I$ROOT/ports/compat -fPIC -DORBIS -D_GNU_SOURCE"
COMMON_LDFLAGS="-L$PREFIX/lib -L$OO_PS4_TOOLCHAIN/lib"

build_zlib() {
    echo
    echo "================ zlib ================"
    cd "$SRC/zlib"
    mkdir -p "$WORK/zlib-obj" "$PREFIX/include" "$PREFIX/lib"
    local sources=(
        adler32.c compress.c crc32.c deflate.c
        gzclose.c gzlib.c gzread.c gzwrite.c
        infback.c inffast.c inflate.c inftrees.c
        trees.c uncompr.c zutil.c
    )
    local objects=()
    for src in "${sources[@]}"; do
        obj="$WORK/zlib-obj/${src%.c}.o"
        "$TOOLS/ps4-gcc" -O2 -DZLIB_CONST -Dlseek=sceKernelLseek -Dread=sceKernelRead -Dwrite=sceKernelWrite -Dclose=sceKernelClose -include orbis/libkernel.h -I"$SRC/zlib" -c "$src" -o "$obj"
        objects+=("$obj")
    done
    llvm-ar-18 rcs "$PREFIX/lib/libz.a" "${objects[@]}"
    cp zlib.h zconf.h "$PREFIX/include/"
    test -s "$PREFIX/lib/libz.a"
}

build_lib() {
    local name="$1"
    echo
    echo "================ $name ================"
    cd "$SRC/$name"
    sed -i 's/-Werror//g' Makefile || true

    local makeargs=(
        "PREFIX=$PREFIX"
        "NSSHARED=$NSBUILD"
        "HOST=$HOST"
        "BUILD=$BUILD"
        "CC=$TOOLS/ps4-gcc"
        "CXX=$TOOLS/ps4-g++"
        "AR=llvm-ar-18"
        "BUILD_CC=cc"
        "PKGCONFIG=$TOOLS/ps4-pkg-config"
        "Q="
        "VQ="
    )

    env CFLAGS="$COMMON_CFLAGS" LDFLAGS="$COMMON_LDFLAGS" make -j2 install "${makeargs[@]}"
}

build_zlib

build_lib libwapcaplet
build_lib libparserutils
build_lib libhubbub
build_lib libcss

cat > "$SRC/libdom/Makefile.config.override" <<'EOF'
WITH_LIBXML_BINDING := no
WITH_EXPAT_BINDING := no
WITH_HUBBUB_BINDING := yes
EOF
build_lib libdom
build_lib libnsutils

# Upstream libnsfb has an SDL 1.2 surface. Replace only that display surface
# with the PS4 SDL2 implementation; keep the RAM surface for offscreen bitmaps.
cp "$ROOT/ports/libnsfb/ps4_sdl2.c" "$SRC/libnsfb/src/surface/ps4_sdl2.c"

python3 - "$SRC/libnsfb/src/surface.h" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
s = s.replace(
    "    static void __name##_register_surface(void) __attribute__((constructor)); \\\n"
    "    void __name##_register_surface(void) {                              \\\n",
    "    void __name##_register_surface(void) {                              \\\n"
)
p.write_text(s)
PY
cat > "$SRC/libnsfb/src/surface/Makefile" <<'EOF'
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
inc = '#include "content/fetchers/curl.h"'
if '#include "content/fetchers/ps4.h"' not in s:
    s = s.replace(
        inc,
        '#ifdef WITH_CURL\n#include "content/fetchers/curl.h"\n#endif\n#include "content/fetchers/ps4.h"'
    )

needle = "nserror fetcher_init(void)\n{\n\tnserror ret;\n"
replacement = (
    "nserror fetcher_init(void)\n{\n\tnserror ret;\n\n"
    "\tret = fetch_ps4_register();\n"
    "\tif (ret != NSERROR_OK) {\n"
    "\t\treturn ret;\n"
    "\t}\n"
)
if "fetch_ps4_register();" not in s:
    s = s.replace(needle, replacement)
p.write_text(s)

p = Path("frontends/framebuffer/gui.c")
s = p.read_text()
s = s.replace("static const char *fename;", 'static const char *fename = "ps4";')

# Hardware startup diagnostic.  This deliberately runs before NetSurf core
# initialisation and uses only the same SDL window-surface API that is proven
# by the old PS4 Browser Downloader.  Each checkpoint requires X to continue.
if '#include <SDL2/SDL.h>' not in s:
    s = s.replace(
        '#include <stdbool.h>',
        '#include <stdbool.h>\n#ifdef ORBIS\n#include <SDL2/SDL.h>\n#endif'
    )

diag = r'''
#ifdef ORBIS
extern void ram_register_surface(void);
extern void ps4_register_surface(void);

static SDL_Window *ps4_diag_window;
static SDL_Surface *ps4_diag_surface;
static SDL_Joystick *ps4_diag_pad;

static void ps4_diag_rect(int x, int y, int w, int h, Uint32 colour)
{
    SDL_Rect r = { x, y, w, h };
    SDL_FillRect(ps4_diag_surface, &r, colour);
}

static void ps4_diag_digit(int digit)
{
    static const unsigned char seg[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66,
        0x6d, 0x7d, 0x07, 0x7f, 0x6f
    };
    const int cx = ps4_diag_surface->w / 2;
    const int cy = ps4_diag_surface->h / 2;
    const int t = 36;
    const int len = 220;
    const Uint32 fg = SDL_MapRGB(ps4_diag_surface->format, 255, 255, 255);
    unsigned char m = (digit >= 0 && digit <= 9) ? seg[digit] : 0;

    if (m & 0x01) ps4_diag_rect(cx-len/2, cy-len-t, len, t, fg);
    if (m & 0x02) ps4_diag_rect(cx+len/2-t, cy-len, t, len, fg);
    if (m & 0x04) ps4_diag_rect(cx+len/2-t, cy, t, len, fg);
    if (m & 0x08) ps4_diag_rect(cx-len/2, cy+len, len, t, fg);
    if (m & 0x10) ps4_diag_rect(cx-len/2, cy, t, len, fg);
    if (m & 0x20) ps4_diag_rect(cx-len/2, cy-len, t, len, fg);
    if (m & 0x40) ps4_diag_rect(cx-len/2, cy-t/2, len, t, fg);
}

static bool ps4_diag_checkpoint(int stage)
{
    static const Uint8 colours[][3] = {
        { 20, 20, 20 }, { 30, 80, 170 }, { 20, 140, 90 },
        { 180, 120, 20 }, { 130, 50, 160 }, { 160, 50, 50 }
    };

    if (ps4_diag_window == NULL) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
            return false;

        ps4_diag_window = SDL_CreateWindow(
            "NetSurf PS4 startup diagnostic",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            1920, 1080, 0);
        if (ps4_diag_window == NULL)
            return false;

        ps4_diag_surface = SDL_GetWindowSurface(ps4_diag_window);
        if (ps4_diag_surface == NULL)
            return false;

        if (SDL_NumJoysticks() > 0)
            ps4_diag_pad = SDL_JoystickOpen(0);
    }

    {
        const int ci = (stage >= 1 && stage <= 6) ? stage - 1 : 0;
        Uint32 bg = SDL_MapRGB(ps4_diag_surface->format,
                               colours[ci][0], colours[ci][1], colours[ci][2]);
        SDL_FillRect(ps4_diag_surface, NULL, bg);
    }

    ps4_diag_digit(stage);
    SDL_UpdateWindowSurface(ps4_diag_window);

    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100) == 0)
            continue;

        if (e.type == SDL_JOYBUTTONDOWN && e.jbutton.button == 0)
            return true;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN)
            return true;

        if ((e.type == SDL_JOYBUTTONDOWN && e.jbutton.button == 1) ||
            (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
            return false;
    }
}

static void ps4_diag_close(void)
{
    if (ps4_diag_pad != NULL) {
        SDL_JoystickClose(ps4_diag_pad);
        ps4_diag_pad = NULL;
    }
    if (ps4_diag_window != NULL) {
        SDL_DestroyWindow(ps4_diag_window);
        ps4_diag_window = NULL;
        ps4_diag_surface = NULL;
    }
    SDL_Quit();
}
#endif
'''

if 'ps4_diag_checkpoint(int stage)' not in s:
    main_marker = '/**\n * Entry point from OS.'
    s = s.replace(main_marker, diag + '\n' + main_marker)

# Insert checkpoints at the exact startup boundaries we need to distinguish.
s = s.replace(
    'main(int argc, char** argv)\n{\n\tstruct browser_window *bw;',
    'main(int argc, char** argv)\n{\n#ifdef ORBIS\n'
    '\tif (!ps4_diag_checkpoint(1)) return 101;\n'
    '\tram_register_surface();\n'
    '\tps4_register_surface();\n'
    '#endif\n'
    '\tstruct browser_window *bw;'
)

s = s.replace(
    '\trespaths = fb_init_resource_path(NETSURF_FB_RESPATH":"NETSURF_FB_FONTPATH);',
    '#ifdef ORBIS\n\tif (!ps4_diag_checkpoint(2)) return 102;\n#endif\n\n'
    '\trespaths = fb_init_resource_path(NETSURF_FB_RESPATH":"NETSURF_FB_FONTPATH);'
)

s = s.replace(
    '\t/* common initialisation */\n\tret = netsurf_init(NULL);',
    '#ifdef ORBIS\n\tif (!ps4_diag_checkpoint(3)) return 103;\n#endif\n\n'
    '\t/* common initialisation */\n\tret = netsurf_init(NULL);'
)

s = s.replace(
    '\t/* Override, since we have no support for non-core SELECT menu */',
    '#ifdef ORBIS\n\tif (!ps4_diag_checkpoint(4)) return 104;\n#endif\n\n'
    '\t/* Override, since we have no support for non-core SELECT menu */'
)

s = s.replace(
    '\tif (process_cmdline(argc,argv) != true)\n\t\tdie("unable to process command line.\\n");',
    '\tif (process_cmdline(argc,argv) != true)\n\t\tdie("unable to process command line.\\n");\n\n'
    '#ifdef ORBIS\n\tif (!ps4_diag_checkpoint(5)) return 105;\n'
    '\tps4_diag_close();\n#endif'
)

p.write_text(s)
old = """static void
framebuffer_pick_default_fename(void *ctx, const char *name, enum nsfb_type_e type)
{
\tif (type < fetype) {"""
new = """static void
framebuffer_pick_default_fename(void *ctx, const char *name, enum nsfb_type_e type)
{
\tif (fename != NULL && strcmp(fename, "ps4") == 0)
\t\treturn;
\tif (type < fetype) {"""
s = s.replace(old, new)

# DualShock Circle is translated to NSFB_KEY_ESCAPE by the PS4 SDL2 surface.
# Give that key a browser-level meaning: history back.
if "static void fb_update_back_forward(struct gui_window *gw);" not in s:
    s = s.replace(
        "struct gui_window *window_list = NULL;\n",
        "struct gui_window *window_list = NULL;\n\n"
        "static void fb_update_back_forward(struct gui_window *gw);\n"
    )

needle = """\tcase NSFB_EVENT_KEY_DOWN:
\t\tswitch (cbi->event->value.keycode) {

\t\tcase NSFB_KEY_DELETE:"""
replacement = """\tcase NSFB_EVENT_KEY_DOWN:
\t\tswitch (cbi->event->value.keycode) {

\t\tcase NSFB_KEY_ESCAPE:
\t\t\tif (browser_window_back_available(gw->bw))
\t\t\t\tbrowser_window_history_back(gw->bw, false);
\t\t\tfb_update_back_forward(gw);
\t\t\tbreak;

\t\tcase NSFB_KEY_DELETE:"""
if "case NSFB_KEY_ESCAPE:" not in s[s.find("fb_browser_window_input"):s.find("fb_update_back_forward")]:
    s = s.replace(needle, replacement)

p.write_text(s)
PY

cat > Makefile.config <<'EOF'
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
NETSURF_FB_RESPATH := /app0/assets/misc
NETSURF_FB_FONTPATH := /app0/res
EOF

export GCCSDK_INSTALL_ENV="$PREFIX"
export GCCSDK_INSTALL_CROSSBIN="$TOOLS"

NETSURF_LDFLAGS="$COMMON_LDFLAGS -lc -lkernel -lSDL2 -lSceUserService -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lSceVideoOut -lSceAudioOut -lScePad"

env CFLAGS="$COMMON_CFLAGS" LDFLAGS="$NETSURF_LDFLAGS" make -j2 TARGET=framebuffer "CC=$TOOLS/ps4-gcc" "CXX=$TOOLS/ps4-g++" "PKG_CONFIG=$TOOLS/ps4-pkg-config" Q= VQ=

test -s nsfb
mkdir -p "$ROOT/build/independent"
cp nsfb "$ROOT/build/independent/netsurf-ps4.elf"
mkdir -p "$ROOT/build/independent/res"
cp -aL frontends/framebuffer/res/. "$ROOT/build/independent/res/"

echo "SUCCESS: $ROOT/build/independent/netsurf-ps4.elf"
file "$ROOT/build/independent/netsurf-ps4.elf" || true

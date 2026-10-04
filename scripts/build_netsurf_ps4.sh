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
exec clang-18 "\${COMMON[@]}" -fuse-ld=lld -nostdlib -Wl,-pie -Wl,--no-dynamic-linker -Wl,--script="$OO_PS4_TOOLCHAIN/link.x" -Wl,--eh-frame-hdr -L"$PREFIX/lib" -L"$OO_PS4_TOOLCHAIN/lib" "\$@" "$OO_PS4_TOOLCHAIN/lib/crt1.o" -Wl,--start-group -lSDL2 -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceCommonDialog -lSceImeDialog -lSceIme -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lc -lkernel -Wl,--end-group
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
        '#include <stdbool.h>\n#ifdef ORBIS\n#include <SDL2/SDL.h>\n#include <orbis/ImeDialog.h>\n#include <orbis/Sysmodule.h>\n#include <orbis/Http.h>\n#include <orbis/Net.h>\n#include <orbis/Ssl.h>\n#endif'
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
    '\tram_register_surface();\n'
    '\tps4_register_surface();\n'
    '#endif\n'
    '\tstruct browser_window *bw;'
)

s = s.replace(
    '\trespaths = fb_init_resource_path(NETSURF_FB_RESPATH":"NETSURF_FB_FONTPATH);',
    '\trespaths = fb_init_resource_path(NETSURF_FB_RESPATH":"NETSURF_FB_FONTPATH);'
)

s = s.replace(
    '\t/* common initialisation */\n\tret = netsurf_init(NULL);',
    '\t/* common initialisation */\n\tret = netsurf_init(NULL);'
)

s = s.replace(
    '\t/* Override, since we have no support for non-core SELECT menu */',
    '\t/* Override, since we have no support for non-core SELECT menu */'
)

s = s.replace(
    '\tif (process_cmdline(argc,argv) != true)\n\t\tdie("unable to process command line.\\n");',
    '\tif (process_cmdline(argc,argv) != true)\n\t\tdie("unable to process command line.\\n");'
)

p.write_text(s)
# Native PS4 HTTP/HTTPS preflight. This runs before the NetSurf fetcher so
# we can distinguish PS4 networking failures from fetch-bridge failures.
netdiag_code = r'''
#ifdef ORBIS
struct ps4_probe_result {
    int stage;
    int code;
    int http_status;
    int bytes;
};

static const char *ps4_probe_stage_name(int stage)
{
    switch (stage) {
    case 0: return "OK";
    case 1: return "net-pool";
    case 2: return "ssl-init";
    case 3: return "http-init";
    case 4: return "template";
    case 5: return "connection";
    case 6: return "request";
    case 7: return "send";
    case 8: return "status";
    case 9: return "read";
    default: return "unknown";
    }
}

static struct ps4_probe_result
ps4_native_probe(int http, const char *url)
{
    struct ps4_probe_result r;
    int tpl = -1, conn = -1, req = -1;
    int ret;
    unsigned char buf[2048];

    memset(&r, 0, sizeof(r));

    tpl = sceHttpCreateTemplate(http, "PS4BrowserPreflight/1.0",
                                ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        r.stage = 4; r.code = tpl; goto done;
    }

    (void)sceHttpSetResolveTimeOut(tpl, 5000000);
    (void)sceHttpSetConnectTimeOut(tpl, 5000000);
    (void)sceHttpSetSendTimeOut(tpl, 5000000);
    sceHttpSetRecvTimeOut(tpl, 5000000);

    conn = sceHttpCreateConnectionWithURL(tpl, url, false);
    if (conn < 0) {
        r.stage = 5; r.code = conn; goto done;
    }

    req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url, 0);
    if (req < 0) {
        r.stage = 6; r.code = req; goto done;
    }

    (void)sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", 1);
    (void)sceHttpAddRequestHeader(req, "Connection", "close", 1);

    ret = sceHttpSendRequest(req, NULL, 0);
    if (ret < 0) {
        r.stage = 7; r.code = ret; goto done;
    }

    ret = sceHttpGetStatusCode(req, &r.http_status);
    if (ret < 0) {
        r.stage = 8; r.code = ret; goto done;
    }

    ret = sceHttpReadData(req, buf, sizeof(buf));
    if (ret < 0) {
        r.stage = 9; r.code = ret; goto done;
    }

    r.bytes = ret;
    r.stage = 0;
    r.code = 0;

done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return r;
}

static void ps4_percent_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (cap == 0) return;

    for (size_t i = 0; src[i] != '\0' && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        bool safe = (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';

        if (safe) {
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= cap) break;
            dst[o++] = '%';
            dst[o++] = hex[(c >> 4) & 0xF];
            dst[o++] = hex[c & 0xF];
        }
    }

    dst[o] = '\0';
}

static const char *ps4_network_test_page(void)
{
    static char data_url[16384];
    char html[4096];
    char encoded[12288];

    struct ps4_probe_result http_r = {0};
    struct ps4_probe_result https_r = {0};

    int net_pool = -1;
    int ssl = -1;
    int http = -1;

    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    (void)sceNetInit();

    net_pool = sceNetPoolCreate("netsurf-preflight", 128 * 1024, 0);
    if (net_pool < 0) {
        http_r.stage = https_r.stage = 1;
        http_r.code = https_r.code = net_pool;
        goto render;
    }

    ssl = sceSslInit(1024 * 1024);
    if (ssl < 0) {
        http_r.stage = https_r.stage = 2;
        http_r.code = https_r.code = ssl;
        goto render;
    }

    http = sceHttpInit(net_pool, ssl, 1024 * 1024);
    if (http < 0) {
        http_r.stage = https_r.stage = 3;
        http_r.code = https_r.code = http;
        goto render;
    }

    http_r = ps4_native_probe(http, "http://example.com/");
    https_r = ps4_native_probe(http, "https://www.google.com/generate_204");

render:
    snprintf(html, sizeof(html),
        "<html><head><title>PS4 Network Test</title>"
        "<style>"
        "body{background:#101820;color:white;font-family:sans-serif;margin:70px;}"
        "h1{font-size:52px;}p{font-size:30px;line-height:1.5;}"
        "a{display:block;background:white;color:black;padding:30px;margin-top:35px;"
        "font-size:36px;text-decoration:none;border:3px solid #777;}"
        "</style></head><body>"
        "<h1>PS4 Browser - teste nativo de rede</h1>"
        "<p>HTTP example.com: stage=%s code=0x%08x status=%d bytes=%d</p>"
        "<p>HTTPS Google: stage=%s code=0x%08x status=%d bytes=%d</p>"
        "<p>Se os dois mostrarem stage=OK, a internet do PS4 esta funcionando "
        "e o defeito esta somente na ponte NetSurf - sceHttp.</p>"
        "<a href='http://example.com/'>TESTAR EXAMPLE PELO NETSURF</a>"
        "<a href='https://www.google.com/'>ABRIR GOOGLE PELO NETSURF</a>"
        "</body></html>",
        ps4_probe_stage_name(http_r.stage), (unsigned)http_r.code,
        http_r.http_status, http_r.bytes,
        ps4_probe_stage_name(https_r.stage), (unsigned)https_r.code,
        https_r.http_status, https_r.bytes);

    if (http >= 0) sceHttpTerm(http);
    if (ssl >= 0) sceSslTerm(ssl);
    if (net_pool >= 0) sceNetPoolDestroy(net_pool);

    ps4_percent_encode(html, encoded, sizeof(encoded));
    snprintf(data_url, sizeof(data_url), "data:text/html,%s", encoded);
    return data_url;
}
#endif

'''
netdiag_anchor = """static int
fb_url_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{"""
if "ps4_network_test_page" not in s:
    s = s.replace(netdiag_anchor, netdiag_code + "\n" + netdiag_anchor)


# PS4-native URL keyboard. The framebuffer OSK is unsuitable for a TV/controller.
ime_anchor = """static int
fb_url_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{"""
ime_code = r'''
#ifdef ORBIS
static void ps4_ascii_to_utf16(const char *src, uint16_t *dst, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    while (src != NULL && src[i] != '\0' && i + 1 < cap) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (uint16_t)c;
        i++;
    }
    dst[i] = 0;
}

static void ps4_utf16_to_ascii(const uint16_t *src, char *dst, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    while (src[i] != 0 && i + 1 < cap) {
        uint16_t c = src[i];
        dst[i] = (c < 0x80) ? (char)c : '?';
        i++;
    }
    dst[i] = '\0';
}

static bool ps4_open_url_keyboard(struct gui_window *gw)
{
    static uint16_t input[512];
    static uint16_t title[64];
    char output[512];
    const char *initial = "";
    nsurl *current = browser_window_access_url(gw->bw);

    if (current != NULL)
        initial = nsurl_access(current);

    memset(input, 0, sizeof(input));
    memset(title, 0, sizeof(title));
    ps4_ascii_to_utf16(initial, input, 512);
    ps4_ascii_to_utf16("Digite um endereco", title, 64);

    (void)sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);

    OrbisImeDialogSetting param;
    memset(&param, 0, sizeof(param));
    param.userId = 0xFE;
    param.type = ORBIS_TYPE_TYPE_URL;
    param.enterLabel = ORBIS_BUTTON_LABEL_GO;
    param.maxTextLength = 510;
    param.inputTextBuffer = (wchar_t *)input;
    param.title = (const wchar_t *)title;
    /*
     * Keep the native IME inside the 1920x1080 framebuffer.  Centering with
     * the default (0,0) anchor placed most of the panel outside the screen.
     */
    param.posx = 0.0f;
    param.posy = 0.0f;
    param.horizontalAlignment = ORBIS_H_LEFT;
    param.verticalAlignment = ORBIS_V_TOP;

    int ret = sceImeDialogInit(&param, NULL);
    if (ret < 0)
        return false;

    bool accepted = false;
    for (;;) {
        OrbisDialogStatus status = sceImeDialogGetStatus();
        if (status == ORBIS_DIALOG_STATUS_STOPPED) {
            OrbisDialogResult result;
            memset(&result, 0, sizeof(result));
            if (sceImeDialogGetResult(&result) >= 0 &&
                result.endstatus == ORBIS_DIALOG_OK) {
                accepted = true;
            }
            break;
        }
        if (status == ORBIS_DIALOG_STATUS_NONE)
            break;
        SDL_Delay(16);
    }

    sceImeDialogTerm();

    if (!accepted)
        return false;

    ps4_utf16_to_ascii(input, output, sizeof(output));
    if (output[0] == '\0')
        return false;

    fbtk_set_text(gw->url, output);
    fb_url_enter(gw->bw, output);
    return true;
}

static int ps4_url_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
    struct gui_window *gw = cbi->context;
    (void)widget;

    if (cbi->event->type != NSFB_EVENT_KEY_UP)
        return 0;

    (void)ps4_open_url_keyboard(gw);
    return 0;
}

static int ps4_url_ignore_input(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
    (void)widget;
    (void)cbi;
    return 0;
}
#endif

'''
if "ps4_open_url_keyboard" not in s:
    s = s.replace(ime_anchor, ime_code + ime_anchor)

# Hook X-click on URL bar to native IME and suppress the framebuffer text input.
url_hook = """				fbtk_set_handler(widget, 
						 FBTK_CBT_POINTERENTER, 
						 fb_url_move, gw->bw);

				gw->url = widget; /* keep reference */"""
url_hook_new = """				fbtk_set_handler(widget,
						 FBTK_CBT_POINTERENTER,
						 fb_url_move, gw->bw);
#ifdef ORBIS
				fbtk_set_handler(widget, FBTK_CBT_CLICK, ps4_url_click, gw);
				fbtk_set_handler(widget, FBTK_CBT_INPUT, ps4_url_ignore_input, gw);
#endif

				gw->url = widget; /* keep reference */"""
s = s.replace(url_hook, url_hook_new)

# PS4 is always a 1080p TV framebuffer for this frontend and starts on a
# lightweight plain-HTTP page. Once native text entry works the user can
# navigate anywhere from the address bar.
dim_anchor = """	if (optind < argc) {
		feurl = argv[optind];
	}

	if (nsfb_type_from_name(fename) == NSFB_SURFACE_NONE) {"""
dim_new = """	if (optind < argc) {
		feurl = argv[optind];
	}

#ifdef ORBIS
	fewidth = 1920;
	feheight = 1080;
	feurl = ps4_network_test_page();
#endif

	if (nsfb_type_from_name(fename) == NSFB_SURFACE_NONE) {"""
s = s.replace(dim_anchor, dim_new)

# Disable framebuffer on-screen keyboard; PS4 native IME replaces it.
s = s.replace("\tfbtk_enable_oskb(fbtk);", "#ifndef ORBIS\n\tfbtk_enable_oskb(fbtk);\n#endif")

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

cat > frontends/framebuffer/res/welcome.html <<'EOF'
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>NetSurf PS4 - Teste de Rede</title>
<style>
html,body { margin:0; padding:0; background:#101820; color:#fff; font-family:sans-serif; }
main { width:88%; margin:40px auto; }
h1 { font-size:52px; margin:0 0 20px; }
p { font-size:30px; line-height:1.4; }
a {
  display:block;
  margin:30px 0;
  padding:38px;
  background:#f2f2f2;
  color:#111;
  text-decoration:none;
  font-size:40px;
  border:4px solid #777;
}
small { font-size:24px; }
</style>
</head>
<body>
<main>
<h1>NetSurf PS4</h1>
<p>O motor abriu. Agora teste a rede sem precisar digitar.</p>
<a href="http://example.com/">TESTAR HTTP</a>
<a href="https://example.com/">TESTAR HTTPS</a>
<p><small>Use o analogico para mover a seta e X para clicar.</small></p>
</main>
</body>
</html>
EOF

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
override NETSURF_HOMEPAGE := "about:welcome"
NETSURF_FB_RESPATH := /app0/assets/misc
NETSURF_FB_FONTPATH := /app0/res
EOF

export GCCSDK_INSTALL_ENV="$PREFIX"
export GCCSDK_INSTALL_CROSSBIN="$TOOLS"

NETSURF_LDFLAGS="$COMMON_LDFLAGS -lc -lkernel -lSDL2 -lSceUserService -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lSceVideoOut -lSceAudioOut -lScePad -lSceCommonDialog -lSceImeDialog -lSceIme"

env CFLAGS="$COMMON_CFLAGS" LDFLAGS="$NETSURF_LDFLAGS" make -j2 TARGET=framebuffer "CC=$TOOLS/ps4-gcc" "CXX=$TOOLS/ps4-g++" "PKG_CONFIG=$TOOLS/ps4-pkg-config" Q= VQ=

test -s nsfb

# PS4 FSELF executables must not request the host FreeBSD runtime loader.
# A PT_INTERP here makes the console fail before main() (CE-34878-0).
if readelf -lW nsfb | grep -q 'INTERP'; then
    echo "ERROR: PS4 NetSurf ELF unexpectedly contains PT_INTERP"
    readelf -lW nsfb
    exit 1
fi

mkdir -p "$ROOT/build/independent"
cp nsfb "$ROOT/build/independent/netsurf-ps4.elf"
mkdir -p "$ROOT/build/independent/res"
cp -aL frontends/framebuffer/res/. "$ROOT/build/independent/res/"

echo "SUCCESS: $ROOT/build/independent/netsurf-ps4.elf"
file "$ROOT/build/independent/netsurf-ps4.elf" || true

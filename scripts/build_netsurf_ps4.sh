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
exec clang-18 "\${COMMON[@]}" -fuse-ld=lld -nostdlib -Wl,-pie -Wl,--no-dynamic-linker -Wl,--script="$OO_PS4_TOOLCHAIN/link.x" -Wl,--eh-frame-hdr -L"$PREFIX/lib" -L"$OO_PS4_TOOLCHAIN/lib" "\$@" "$OO_PS4_TOOLCHAIN/lib/crt1.o" -Wl,--start-group -lSDL2 -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceCommonDialog -lSceMsgDialog -lSceImeDialog -lSceIme -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lc -lkernel -Wl,--end-group
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
s = s.replace(
    "static const char *fename;",
    'static const char *fename = "ps4";'
)
s = s.replace(
    "static const char *feurl;",
    "static const char *feurl;\n#ifdef ORBIS\nstatic void ps4_show_network_probe_dialog(void);\n#endif"
)

# Hardware startup diagnostic.  This deliberately runs before NetSurf core
# initialisation and uses only the same SDL window-surface API that is proven
# by the old PS4 Browser Downloader.  Each checkpoint requires X to continue.
if '#include <SDL2/SDL.h>' not in s:
    s = s.replace(
        '#include <stdbool.h>',
        '#include <stdbool.h>\n#ifdef ORBIS\n#include <SDL2/SDL.h>\n#include <orbis/ImeDialog.h>\n#include <orbis/Sysmodule.h>\n#include <orbis/Http.h>\n#include <orbis/Net.h>\n#include <orbis/Ssl.h>\n#include <orbis/CommonDialog.h>\n#include <orbis/MsgDialog.h>\n#endif'
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

# PS4 bootstrap mode: render the document before optional remote assets.
# This keeps the first independent-browser milestone deterministic.
s = s.replace(
    "\t/* Override, since we have no support for non-core SELECT menu */",
    "#ifdef ORBIS\n"
    "\t/* Re-enable real site CSS and common raster images. */\n"
    "\tnsoption_set_bool(author_level_css, true);\n"
    "\tnsoption_set_bool(foreground_images, true);\n"
    "\tnsoption_set_bool(background_images, true);\n"
    "\tnsoption_set_bool(animate_images, false);\n"
    "#endif\n\n"
    "\t/* Override, since we have no support for non-core SELECT menu */"
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

static inline void ps4_common_dialog_base_init(OrbisCommonDialogBaseParam *param)
{
    memset(param, 0, sizeof(*param));
    param->size = sizeof(*param);
    param->magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)param);
}

static inline void ps4_msg_dialog_param_init(OrbisMsgDialogParam *param)
{
    memset(param, 0, sizeof(*param));
    ps4_common_dialog_base_init(&param->baseParam);
    param->size = sizeof(*param);
}

static void ps4_show_network_probe_dialog(void)
{
    char message[1400];

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
    snprintf(message, sizeof(message),
        "PS4 Browser - teste nativo de rede\n\n"
        "HTTP example.com\n"
        "stage=%s\ncode=0x%08x\nstatus=%d\nbytes=%d\n\n"
        "HTTPS Google\n"
        "stage=%s\ncode=0x%08x\nstatus=%d\nbytes=%d\n\n"
        "Feche com X. Depois o NetSurf tentara abrir o Google.",
        ps4_probe_stage_name(http_r.stage), (unsigned)http_r.code,
        http_r.http_status, http_r.bytes,
        ps4_probe_stage_name(https_r.stage), (unsigned)https_r.code,
        https_r.http_status, https_r.bytes);

    if (http >= 0) sceHttpTerm(http);
    if (ssl >= 0) sceSslTerm(ssl);
    if (net_pool >= 0) sceNetPoolDestroy(net_pool);

    (void)sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
    (void)sceCommonDialogInitialize();

    {
        OrbisMsgDialogParam param;
        OrbisMsgDialogUserMessageParam user_msg;
        OrbisMsgDialogResult result;

        ps4_msg_dialog_param_init(&param);
        memset(&user_msg, 0, sizeof(user_msg));
        memset(&result, 0, sizeof(result));

        param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
        user_msg.msg = message;
        user_msg.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
        param.userMsgParam = &user_msg;

        if (sceMsgDialogInitialize() >= 0 &&
            sceMsgDialogOpen(&param) >= 0) {
            while (sceMsgDialogUpdateStatus() !=
                   ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
                SDL_Delay(16);
            }
            sceMsgDialogClose();
            (void)sceMsgDialogGetResult(&result);
            sceMsgDialogTerminate();
        }
    }
}
#endif

'''
netdiag_anchor = """static int
fb_url_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{"""
if "struct ps4_probe_result" not in s:
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

/*
 * Native IME for editable HTML controls.
 * NetSurf calls gui_window_place_caret() whenever a text input/textarea
 * receives focus.  The accepted text is sent back through the normal
 * browser_window_key_press() path.
 */
static bool ps4_form_ime_active;

static bool ps4_open_form_keyboard(struct gui_window *gw)
{
    static uint16_t input[512];
    static uint16_t title[64];
    bool accepted = false;

    memset(input, 0, sizeof(input));
    memset(title, 0, sizeof(title));
    ps4_ascii_to_utf16("Digite o texto", title, 64);

    (void)sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);

    OrbisImeDialogSetting param;
    memset(&param, 0, sizeof(param));
    param.userId = 0xFE;
    param.type = ORBIS_TYPE_DEFAULT;
    param.enterLabel = ORBIS_BUTTON_LABEL_SEARCH;
    param.maxTextLength = 510;
    param.inputTextBuffer = (wchar_t *)input;
    param.title = (const wchar_t *)title;
    param.posx = 0.0f;
    param.posy = 0.0f;
    param.horizontalAlignment = ORBIS_H_LEFT;
    param.verticalAlignment = ORBIS_V_TOP;

    if (sceImeDialogInit(&param, NULL) < 0)
        return false;

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

    (void)browser_window_key_press(gw->bw, NS_KEY_SELECT_ALL);
    (void)browser_window_key_press(gw->bw, NS_KEY_DELETE_RIGHT);

    for (size_t i = 0; input[i] != 0; i++) {
        uint32_t cp = input[i];

        if (cp >= 0xD800 && cp <= 0xDBFF &&
            input[i + 1] >= 0xDC00 && input[i + 1] <= 0xDFFF) {
            uint32_t hi = cp - 0xD800;
            uint32_t lo = input[++i] - 0xDC00;
            cp = 0x10000 + ((hi << 10) | lo);
        }

        (void)browser_window_key_press(gw->bw, cp);
    }

    /* Search/Go on the PS4 keyboard maps to Enter in the focused control. */
    (void)browser_window_key_press(gw->bw, NS_KEY_CR);

    return true;
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
	feurl = "about:welcome";
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

# When an HTML editable control places a caret, open the native PS4 IME.
caret_old = """static void
gui_window_place_caret(struct gui_window *g, int x, int y, int height,
		const struct rect *clip)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

	/* set new pos */
	fbtk_set_caret(g->browser, true, x, y, height,
			gui_window_remove_caret_cb);

	/* redraw new caret pos */
	fb_queue_redraw(g->browser,
			x - bwidget->scrollx,
			y - bwidget->scrolly,
			x + 1 - bwidget->scrollx,
			y + height - bwidget->scrolly);
}"""

caret_new = """static void
gui_window_place_caret(struct gui_window *g, int x, int y, int height,
		const struct rect *clip)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

	/* set new pos */
	fbtk_set_caret(g->browser, true, x, y, height,
			gui_window_remove_caret_cb);

	/* redraw new caret pos */
	fb_queue_redraw(g->browser,
			x - bwidget->scrollx,
			y - bwidget->scrolly,
			x + 1 - bwidget->scrollx,
			y + height - bwidget->scrolly);

#ifdef ORBIS
	if (!ps4_form_ime_active) {
		ps4_form_ime_active = true;
		(void)ps4_open_form_keyboard(g);
		ps4_form_ime_active = false;
	}
#endif
}"""

if caret_old not in s:
    raise SystemExit("framebuffer caret implementation changed")
s = s.replace(caret_old, caret_new)

p.write_text(s)
PY

cat > frontends/framebuffer/res/welcome.html <<'EOF'
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>PS4 Browser</title>
<style>
html,body {
  margin:0;
  padding:0;
  width:100%;
  height:100%;
  background:#ffffff;
  color:#202124;
  font-family:sans-serif;
}
main {
  width:82%;
  margin:0 auto;
  padding-top:120px;
  text-align:center;
}
h1 {
  font-size:72px;
  font-weight:600;
  margin:0 0 20px;
}
.subtitle {
  font-size:28px;
  color:#5f6368;
  margin-bottom:48px;
}
form {
  width:100%;
}
input[type=text] {
  width:86%;
  height:74px;
  padding:0 28px;
  font-size:32px;
  border:2px solid #dfe1e5;
  border-radius:36px;
  background:#fff;
  color:#202124;
}
input[type=submit] {
  margin-top:30px;
  padding:20px 42px;
  font-size:28px;
  border:1px solid #dadce0;
  border-radius:10px;
  background:#f8f9fa;
  color:#202124;
}
.help {
  margin-top:60px;
  font-size:22px;
  color:#5f6368;
  line-height:1.5;
}
.quick {
  margin-top:42px;
}
.quick a {
  display:inline-block;
  margin:10px 18px;
  padding:14px 22px;
  border:1px solid #dadce0;
  border-radius:10px;
  text-decoration:none;
  color:#1a73e8;
  font-size:24px;
}
</style>
</head>
<body>
<main>
  <h1>PS4 Browser</h1>
  <div class="subtitle">Pesquise e abra sites sem depender de JavaScript</div>

  <form action="https://html.duckduckgo.com/html/" method="get">
    <input type="text" name="q" placeholder="Pesquisar na web" autofocus>
    <br>
    <input type="submit" value="Pesquisar">
  </form>

  <div class="quick">
    <a href="https://www.wikipedia.org/">Wikipedia</a>
    <a href="https://github.com/">GitHub</a>
    <a href="https://www.google.com/">Google</a>
  </div>

  <div class="help">
    X: clicar / abrir teclado &nbsp;&nbsp;•&nbsp;&nbsp; O: voltar<br>
    A busca usa uma página HTML compatível com navegadores sem JavaScript.
  </div>
</main>
</body>
</html>
EOF


# PS4 image handler: use stb_image from OpenOrbis for the common web formats
# without pulling libpng/libjpeg into the first visual milestone.
cat > content/handlers/image/ps4_stb.c <<'EOF'
#ifdef ORBIS

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "utils/utils.h"
#include "netsurf/bitmap.h"
#include "content/llcache.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"
#include "image/image_cache.h"

static nserror
ps4stb_create(const content_handler *handler,
              lwc_string *imime_type,
              const struct http_parameter *params,
              llcache_handle *llcache,
              const char *fallback_charset,
              bool quirks,
              struct content **c)
{
    struct content *img = calloc(1, sizeof(*img));
    nserror err;

    if (img == NULL)
        return NSERROR_NOMEM;

    err = content__init(img, handler, imime_type, params, llcache,
                        fallback_charset, quirks);
    if (err != NSERROR_OK) {
        free(img);
        return err;
    }

    *c = img;
    return NSERROR_OK;
}

static struct bitmap *
ps4stb_cache_convert(struct content *c)
{
    const uint8_t *src;
    size_t src_len;
    int w = 0, h = 0, channels = 0;
    unsigned char *rgba;
    struct bitmap *bitmap;
    uint8_t *dst;
    size_t stride;

    src = content__get_source_data(c, &src_len);
    if (src == NULL || src_len == 0 || src_len > INT32_MAX)
        return NULL;

    rgba = stbi_load_from_memory(src, (int)src_len,
                                 &w, &h, &channels, 4);
    if (rgba == NULL || w <= 0 || h <= 0) {
        if (rgba != NULL)
            stbi_image_free(rgba);
        return NULL;
    }

    bitmap = guit->bitmap->create(w, h, BITMAP_NONE);
    if (bitmap == NULL) {
        stbi_image_free(rgba);
        return NULL;
    }

    dst = guit->bitmap->get_buffer(bitmap);
    stride = guit->bitmap->get_rowstride(bitmap);
    if (dst == NULL || stride < (size_t)w * 4) {
        guit->bitmap->destroy(bitmap);
        stbi_image_free(rgba);
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        memcpy(dst + ((size_t)y * stride),
               rgba + ((size_t)y * (size_t)w * 4),
               (size_t)w * 4);
    }

    stbi_image_free(rgba);

    /*
     * stb_image emits ordinary RGBA. Convert once into whatever client
     * bitmap layout the framebuffer selected.
     */
    bitmap_format_to_client(bitmap, &(bitmap_fmt_t) {
        .layout = BITMAP_LAYOUT_R8G8B8A8,
        .pma = false,
    });

    {
        bool opaque = bitmap_test_opaque(bitmap);
        guit->bitmap->set_opaque(bitmap, opaque);
    }
    guit->bitmap->modified(bitmap);

    return bitmap;
}

static bool
ps4stb_convert(struct content *c)
{
    const uint8_t *src;
    size_t src_len;
    int w = 0, h = 0, channels = 0;

    src = content__get_source_data(c, &src_len);
    if (src == NULL || src_len == 0 || src_len > INT32_MAX)
        return false;

    if (stbi_info_from_memory(src, (int)src_len, &w, &h, &channels) == 0 ||
        w <= 0 || h <= 0) {
        NSLOG(netsurf, INFO, "PS4 stb_image could not identify image");
        return false;
    }

    c->width = w;
    c->height = h;
    c->size = (size_t)w * (size_t)h * 4;

    image_cache_add(c, NULL, ps4stb_cache_convert);
    content_set_ready(c);
    content_set_done(c);
    content_set_status(c, "");

    return true;
}

static nserror
ps4stb_clone(const struct content *old, struct content **new_c)
{
    struct content *img = calloc(1, sizeof(*img));
    nserror err;

    if (img == NULL)
        return NSERROR_NOMEM;

    err = content__clone(old, img);
    if (err != NSERROR_OK) {
        content_destroy(img);
        return err;
    }

    if (old->status == CONTENT_STATUS_READY ||
        old->status == CONTENT_STATUS_DONE) {
        if (!ps4stb_convert(img)) {
            content_destroy(img);
            return NSERROR_CLONE_FAILED;
        }
    }

    *new_c = img;
    return NSERROR_OK;
}

static const content_handler ps4stb_content_handler = {
    .create = ps4stb_create,
    .data_complete = ps4stb_convert,
    .destroy = image_cache_destroy,
    .redraw = image_cache_redraw,
    .clone = ps4stb_clone,
    .get_internal = image_cache_get_internal,
    .type = image_cache_content_type,
    .is_opaque = image_cache_is_opaque,
    .no_share = false,
};

static const char *ps4stb_types[] = {
    "image/png",
    "image/x-png",
    "image/jpeg",
    "image/jpg",
    "image/pjpeg",
    "image/gif",
    "image/bmp",
    "image/x-bmp"
};

CONTENT_FACTORY_REGISTER_TYPES(ps4stb, ps4stb_types, ps4stb_content_handler);

#endif
EOF

python3 - <<'PY'
from pathlib import Path
p = Path("content/handlers/image/Makefile")
s = p.read_text()
if "ps4_stb.c" not in s:
    s = s.replace(
        "S_IMAGE_YES := image.c image_cache.c",
        "S_IMAGE_YES := image.c image_cache.c ps4_stb.c"
    )
p.write_text(s)

p = Path("content/handlers/image/image.c")
s = p.read_text()
if "ps4stb_init" not in s:
    s = s.replace(
        '#include "image/image.h"',
        '#include "image/image.h"\n#ifdef ORBIS\nnserror ps4stb_init(void);\n#endif'
    )
    s = s.replace(
        'nserror error = NSERROR_OK;',
        'nserror error = NSERROR_OK;\n\n#ifdef ORBIS\n'
        '\terror = ps4stb_init();\n'
        '\tif (error != NSERROR_OK)\n'
        '\t\treturn error;\n'
        '#endif'
    )
p.write_text(s)
PY

# On PS4 do not make NetSurf's mandatory UA stylesheets depend on
# POSIX realpath/stat/access. Embed them into the executable and expose them
# through gui_fetch_table.get_resource_data().
python3 - <<'PY'
from pathlib import Path

root = Path(".")
resource_names = [
    "default.css",
    "adblock.css",
    "quirks.css",
    "internal.css",
]

def c_array(name: str, data: bytes) -> str:
    ident = "ps4_res_" + name.replace(".", "_").replace("-", "_")
    vals = ",".join(str(b) for b in data)
    return (
        f"static const unsigned char {ident}[] = {{{vals}}};\n"
        f"static const size_t {ident}_len = sizeof({ident});\n"
    )

parts = ["/* generated PS4 embedded NetSurf resources */\n"]
for name in resource_names:
    data = (root / "resources" / name).read_bytes()
    parts.append(c_array(name, data))

# NetSurf always asks for resource:user.css.  An empty stylesheet is valid.
parts.append("static const unsigned char ps4_res_user_css[] = {0};\n")
parts.append("static const size_t ps4_res_user_css_len = 0;\n")

(root / "frontends/framebuffer/ps4_resource_data.h").write_text(
    "".join(parts), encoding="utf-8"
)

p = root / "frontends/framebuffer/fetch.c"
src = p.read_text()

inc = '#include "framebuffer/fetch.h"\n'
if "ps4_resource_data.h" not in src:
    src = src.replace(
        inc,
        inc + '#ifdef ORBIS\n#include "framebuffer/ps4_resource_data.h"\n#endif\n'
    )

anchor = "/* table for fetch operations */"
impl = r'''
#ifdef ORBIS
static nserror
ps4_get_resource_data(const char *path,
                      const uint8_t **data,
                      size_t *data_len)
{
    if (strcmp(path, "default.css") == 0) {
        *data = ps4_res_default_css;
        *data_len = ps4_res_default_css_len;
        return NSERROR_OK;
    }
    if (strcmp(path, "adblock.css") == 0) {
        *data = ps4_res_adblock_css;
        *data_len = ps4_res_adblock_css_len;
        return NSERROR_OK;
    }
    if (strcmp(path, "quirks.css") == 0) {
        *data = ps4_res_quirks_css;
        *data_len = ps4_res_quirks_css_len;
        return NSERROR_OK;
    }
    if (strcmp(path, "internal.css") == 0) {
        *data = ps4_res_internal_css;
        *data_len = ps4_res_internal_css_len;
        return NSERROR_OK;
    }
    if (strcmp(path, "user.css") == 0) {
        *data = ps4_res_user_css;
        *data_len = ps4_res_user_css_len;
        return NSERROR_OK;
    }

    return NSERROR_NOT_FOUND;
}

static nserror
ps4_release_resource_data(const uint8_t *data)
{
    (void)data;
    return NSERROR_OK;
}
#endif

'''
if "ps4_get_resource_data" not in src:
    src = src.replace(anchor, impl + anchor)

old = '''static struct gui_fetch_table fetch_table = {
\t.filetype = fetch_filetype,

\t.get_resource_url = get_resource_url,
};'''
new = '''static struct gui_fetch_table fetch_table = {
\t.filetype = fetch_filetype,

\t.get_resource_url = get_resource_url,
#ifdef ORBIS
\t.get_resource_data = ps4_get_resource_data,
\t.release_resource_data = ps4_release_resource_data,
#endif
};'''
if old not in src:
    raise SystemExit("framebuffer fetch table shape changed")
src = src.replace(old, new)

p.write_text(src)
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
override NETSURF_HOMEPAGE := "about:welcome"
NETSURF_FB_RESPATH := /app0/assets/misc
NETSURF_FB_FONTPATH := /app0/res
EOF

export GCCSDK_INSTALL_ENV="$PREFIX"
export GCCSDK_INSTALL_CROSSBIN="$TOOLS"

NETSURF_LDFLAGS="$COMMON_LDFLAGS -lc -lkernel -lSDL2 -lSceUserService -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lSceVideoOut -lSceAudioOut -lScePad -lSceCommonDialog -lSceMsgDialog -lSceImeDialog -lSceIme"

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

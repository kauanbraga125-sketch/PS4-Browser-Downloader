#include <cstddef>
#include <cstdint>
#include <orbis/Bgft.h>
#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

static const int SCREEN_W = 1920;
static const int SCREEN_H = 1080;
static const uint32_t DIALOG_MAGIC = 0xC0D1A109;
static const int DIALOG_STATUS_NONE = 0;
static const int DIALOG_STATUS_INITIALIZED = 1;
static const int DIALOG_STATUS_RUNNING = 2;
static const int DIALOG_STATUS_FINISHED = 3;
static const int DIALOG_RESULT_CALLBACK = 2;
static const int CALLBACK_TYPE_REGEXP = 2;
static const int BROWSER_MODE_DEFAULT = 1;
static const int BROWSER_MODE_CUSTOM = 2;

struct CommonDialogBaseParam {
    size_t size;
    uint8_t reserved[36];
    uint32_t magic;
} __attribute__((aligned(8)));

struct BrowserCallbackInitParam {
    size_t size;
    int32_t type;
    int32_t pad;
    const char* data;
    char reserved[32];
};

struct BrowserImeParam {
    size_t size;
    uint32_t option;
    char reserved[256];
    int32_t pad;
};

struct BrowserWebViewParam {
    size_t size;
    uint32_t option;
    char reserved[256];
    int32_t pad;
};

struct BrowserParam {
    CommonDialogBaseParam baseParam;
    size_t size;
    int32_t mode;
    int32_t userId;
    const char* url;
    BrowserCallbackInitParam* callbackInitParam;
    uint16_t width;
    uint16_t height;
    uint16_t positionX;
    uint16_t positionY;
    uint32_t parts;
    uint16_t headerWidth;
    uint16_t headerPositionX;
    uint16_t headerPositionY;
    uint16_t pad0;
    uint32_t control;
    BrowserImeParam* imeParam;
    BrowserWebViewParam* webviewParam;
    uint32_t animation;
    char reserved[202];
    uint16_t tailPad;
};

struct BrowserCallbackResultParam {
    size_t size;
    int32_t type;
    int32_t pad;
    const char* data;
    char* buffer;
    size_t bufferSize;
    char reserved[32];
};

struct BrowserResult {
    int32_t result;
    int32_t pad;
    BrowserCallbackResultParam* callbackResultParam;
    char reserved[240];
};

using BrowserInitializeFn = int32_t (*)();
using BrowserTerminateFn = int32_t (*)();
using BrowserOpenFn = int32_t (*)(const BrowserParam*);
using BrowserUpdateStatusFn = int32_t (*)();
using BrowserGetStatusFn = int32_t (*)();
using BrowserGetResultFn = int32_t (*)(BrowserResult*);
using BrowserCloseFn = int32_t (*)();

struct BrowserApi {
    int32_t module = -1;
    BrowserInitializeFn initialize = nullptr;
    BrowserTerminateFn terminate = nullptr;
    BrowserOpenFn open = nullptr;
    BrowserUpdateStatusFn updateStatus = nullptr;
    BrowserGetStatusFn getStatus = nullptr;
    BrowserGetResultFn getResult = nullptr;
    BrowserCloseFn close = nullptr;
};

struct PkgInfo {
    char contentId[37];
    uint32_t contentType;
    uint32_t flags;
    uint64_t packageSize;
    bool valid;
    PkgInfo() : contentType(0), flags(0), packageSize(0), valid(false) { std::memset(contentId, 0, sizeof(contentId)); }
};

static BrowserApi g_browser;
static void* g_bgftHeap = nullptr;
static bool g_bgftInit = false;
static bool g_httpInit = false;
static int g_netPool = -1;
static int g_ssl = -1;
static int g_http = -1;
static std::string g_status = "Pronto";
static std::string g_lastUrl;

static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint64_t be64(const uint8_t* p) {
    return (uint64_t(be32(p)) << 32) | uint64_t(be32(p + 4));
}

static std::string hex32(int32_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%08X", static_cast<uint32_t>(v));
    return b;
}

static bool dlsym_required(int32_t h, const char* name, void** out) {
    const int32_t r = sceKernelDlsym(h, name, out);
    if (r < 0 || !*out) {
        g_status = std::string("Falha ao localizar ") + name + " (" + hex32(r) + ")";
        return false;
    }
    return true;
}

static int32_t load_system_module(const char* name) {
    char path[256];
    std::snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    int32_t h = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
    if (h >= 0) return h;
    const char* word = sceKernelGetFsSandboxRandomWord();
    if (word && *word) {
        std::snprintf(path, sizeof(path), "/%s/common/lib/%s", word, name);
        h = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
    }
    return h;
}

static bool init_browser() {
    if (g_browser.module >= 0) return true;

    // Match Sony SDK sample order: load the WebBrowserDialog sysmodule first.
    (void)sceSysmoduleLoadModule(ORBIS_SYSMODULE_WEB_BROWSER_DIALOG);

    /*
     * The browser dialog is backed by the PS4 system WebKit.  Initialising
     * CommonDialog first mirrors Sony's own sample flow and recent hardware-
     * tested homebrew.  "Already initialised" is harmless, so this is best-
     * effort and we still let WebBrowserDialog report the authoritative error.
     */
    const int32_t common = load_system_module("libSceCommonDialog.sprx");
    if (common >= 0) {
        using CommonDialogInitializeFn = int32_t (*)();
        CommonDialogInitializeFn commonInit = nullptr;
        if (sceKernelDlsym(common, "sceCommonDialogInitialize", (void**)&commonInit) >= 0 && commonInit)
            (void)commonInit();
    }

    g_browser.module = load_system_module("libSceWebBrowserDialog.sprx");
    if (g_browser.module < 0) {
        g_status = "Nao foi possivel carregar WebBrowserDialog: " + hex32(g_browser.module);
        return false;
    }
    if (!dlsym_required(g_browser.module, "sceWebBrowserDialogInitialize", (void**)&g_browser.initialize) ||
        !dlsym_required(g_browser.module, "sceWebBrowserDialogTerminate", (void**)&g_browser.terminate) ||
        !dlsym_required(g_browser.module, "sceWebBrowserDialogOpen", (void**)&g_browser.open) ||
        !dlsym_required(g_browser.module, "sceWebBrowserDialogUpdateStatus", (void**)&g_browser.updateStatus) ||
        !dlsym_required(g_browser.module, "sceWebBrowserDialogGetStatus", (void**)&g_browser.getStatus) ||
        !dlsym_required(g_browser.module, "sceWebBrowserDialogGetResult", (void**)&g_browser.getResult) ||
        !dlsym_required(g_browser.module, "sceWebBrowserDialogClose", (void**)&g_browser.close)) return false;

    const int32_t r = g_browser.initialize();
    if (r != 0) {
        g_status = "WebBrowserDialog init falhou: " + hex32(r);
        return false;
    }
    return true;
}

static bool init_network() {
    if (g_httpInit) return true;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0) {
        g_status = "Falha ao carregar modulos de rede";
        return false;
    }
    sceNetInit();
    g_netPool = sceNetPoolCreate("PBDLNet", 128 * 1024, 0);
    if (g_netPool < 0) { g_status = "sceNetPoolCreate: " + hex32(g_netPool); return false; }
    g_ssl = sceSslInit(1024 * 1024);
    if (g_ssl < 0) { g_status = "sceSslInit: " + hex32(g_ssl); return false; }
    g_http = sceHttpInit(g_netPool, g_ssl, 1024 * 1024);
    if (g_http < 0) { g_status = "sceHttpInit: " + hex32(g_http); return false; }
    g_httpInit = true;
    return true;
}

static bool fetch_pkg_header(const std::string& url, PkgInfo& info) {
    if (!init_network()) return false;
    const int tmpl = sceHttpCreateTemplate(g_http, "PS4BrowserDownloader/1.0", ORBIS_HTTP_VERSION_1_1, 1);
    if (tmpl < 0) { g_status = "HTTP template: " + hex32(tmpl); return false; }
    const int conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), true);
    if (conn < 0) { sceHttpDeleteTemplate(tmpl); g_status = "HTTP connection: " + hex32(conn); return false; }
    const int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (req < 0) { sceHttpDeleteConnection(conn); sceHttpDeleteTemplate(tmpl); g_status = "HTTP request: " + hex32(req); return false; }
    sceHttpAddRequestHeader(req, "Range", "bytes=0-8191", 1);
    int32_t r = sceHttpSendRequest(req, nullptr, 0);
    if (r < 0) {
        g_status = "Falha HTTP: " + hex32(r);
        sceHttpDeleteRequest(req); sceHttpDeleteConnection(conn); sceHttpDeleteTemplate(tmpl); return false;
    }
    int32_t code = 0;
    sceHttpGetStatusCode(req, &code);
    if (code != 200 && code != 206) {
        char b[64]; std::snprintf(b, sizeof(b), "Servidor respondeu HTTP %d", code); g_status = b;
        sceHttpDeleteRequest(req); sceHttpDeleteConnection(conn); sceHttpDeleteTemplate(tmpl); return false;
    }
    std::vector<uint8_t> data(8192);
    size_t got = 0;
    while (got < data.size()) {
        int n = sceHttpReadData(req, data.data() + got, static_cast<uint32_t>(data.size() - got));
        if (n < 0) { g_status = "Erro lendo cabecalho PKG: " + hex32(n); break; }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    sceHttpDeleteRequest(req); sceHttpDeleteConnection(conn); sceHttpDeleteTemplate(tmpl);
    if (got < 0x438) return false;
    if (!(data[0] == 0x7F && data[1] == 'C' && data[2] == 'N' && data[3] == 'T')) {
        g_status = "O link nao aponta para um PKG PS4 valido";
        return false;
    }
    std::memcpy(info.contentId, data.data() + 0x40, 36);
    info.contentId[36] = 0;
    info.contentType = be32(data.data() + 0x74);
    info.flags = be32(data.data() + 0x78);
    info.packageSize = be64(data.data() + 0x430);
    info.valid = info.packageSize > 0 && std::strlen(info.contentId) >= 19;
    if (!info.valid) g_status = "Cabecalho PKG incompleto";
    return info.valid;
}

struct BgftDownloadParam64 {
    int32_t userId;
    int32_t entitlementType;
    const char* id;
    const char* contentUrl;
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    uint32_t option;
    uint32_t pad;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    unsigned long packageSize;
};

static bool init_bgft() {
    if (g_bgftInit) return true;
    const char* modules[] = {"libSceSystemService.sprx", "libSceAppInstUtil.sprx", "libSceSysUtil.sprx", "libSceBgft.sprx"};
    for (const char* m : modules) {
        int32_t h = load_system_module(m);
        if (h < 0) { g_status = std::string("Falha carregando ") + m + ": " + hex32(h); return false; }
    }
    sceUserServiceInitialize(nullptr);
    g_bgftHeap = std::malloc(1024 * 1024);
    if (!g_bgftHeap) { g_status = "Sem memoria para BGFT"; return false; }
    std::memset(g_bgftHeap, 0, 1024 * 1024);
    OrbisBgftInitParams p{};
    p.heap = g_bgftHeap;
    p.heapSize = 1024 * 1024;
    int32_t r = sceBgftServiceIntInit(&p);
    if (r != 0) { g_status = "BGFT init: " + hex32(r); return false; }
    g_bgftInit = true;
    return true;
}

static bool is_patch(uint32_t flags) {
    return (flags & 0x00100000) || (flags & 0x40000000) || (flags & 0x41000000) || (flags & 0x60000000);
}

static const char* package_type(uint32_t type) {
    switch (type) {
        case 0x1A: return "PS4GD";
        case 0x1B: return "PS4AC";
        case 0x1C: return "PS4AL";
        case 0x1E: return "PS4DP";
        default: return nullptr;
    }
}

static bool queue_bgft(const std::string& url) {
    g_status = "Validando PKG...";
    PkgInfo info;
    if (!fetch_pkg_header(url, info)) return false;
    const char* ptype = package_type(info.contentType);
    if (!ptype) { g_status = "Tipo de PKG nao suportado"; return false; }
    if (!init_bgft()) return false;
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);
    std::string title = info.contentId;
    BgftDownloadParam64 p{};
    p.userId = user;
    p.entitlementType = 5;
    p.id = info.contentId;
    p.contentUrl = url.c_str();
    p.contentExUrl = "";
    p.contentName = title.c_str();
    p.iconPath = "/app0/sce_sys/icon0.png";
    p.skuId = "";
    p.option = 0x10000; // disable CDN query parameter
    p.playgoScenarioId = "0";
    p.releaseDate = "";
    p.packageType = ptype;
    p.packageSubType = "";
    p.packageSize = static_cast<unsigned long>(info.packageSize);

    int task = -1;
    int32_t r;
    if (is_patch(info.flags))
        r = sceBgftServiceIntDebugDownloadRegisterPkg(reinterpret_cast<OrbisBgftDownloadParam*>(&p), &task);
    else
        r = sceBgftServiceIntDownloadRegisterTask(reinterpret_cast<OrbisBgftDownloadParam*>(&p), &task);
    if (r != 0) { g_status = "BGFT register: " + hex32(r); return false; }
    r = sceBgftServiceDownloadStartTask(task);
    if (r != 0) {
        sceBgftServiceIntDownloadUnregisterTask(task);
        g_status = "BGFT start: " + hex32(r);
        return false;
    }
    char msg[160];
    std::snprintf(msg, sizeof(msg), "Download enviado ao PS4 (task %d). Pode fechar o app.", task);
    g_status = msg;
    g_lastUrl = url;
    return true;
}

static bool looks_like_pkg(std::string u) {
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    const size_t q = u.find_first_of("?#");
    if (q != std::string::npos) u.resize(q);
    return u.size() >= 4 && u.substr(u.size() - 4) == ".pkg";
}

static bool open_browser_and_wait() {
    if (!init_browser()) return false;

    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);

    static const char* startUrl = "https://www.google.com/?hl=pt-BR";
    static const char* callbackRegex = "https?://.*\\.pkg([?#].*)?$";

    BrowserCallbackInitParam cb{};
    cb.size = sizeof(cb);
    cb.type = CALLBACK_TYPE_REGEXP;
    cb.data = callbackRegex;

    BrowserImeParam ime{};
    ime.size = sizeof(ime);
    ime.option = 0;

    BrowserWebViewParam webview{};
    webview.size = sizeof(webview);
    webview.option = 0;

    /*
     * Use CUSTOM mode directly. This keeps the same PS4 WebKit engine that
     * already rendered Google and YouTube correctly in v4.1, but exposes the
     * browser header/address bar plus the native Back/Forward/Reload controls.
     *
     * Sony's sample uses:
     *   parts   = TITLE | ADDRESS
     *   control = EXIT | RELOAD | BACK | FORWARD | ZOOM | OPTION_MENU
     * Values are bit flags; 0x3 and 0x7F cover that family on this ABI.
     */
    BrowserParam p{};
    p.baseParam.size = sizeof(CommonDialogBaseParam);
    p.baseParam.magic = DIALOG_MAGIC + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&p.baseParam));
    p.size = sizeof(p);
    p.mode = BROWSER_MODE_CUSTOM;
    p.userId = user;
    p.url = startUrl;
    p.callbackInitParam = &cb;
    p.width = 1920;
    p.height = 963;
    p.positionX = 0;
    p.positionY = 117;
    p.parts = 0x3;
    p.headerWidth = 1920;
    p.headerPositionX = 0;
    p.headerPositionY = 0;
    p.control = 0x7F;
    p.imeParam = &ime;
    p.webviewParam = &webview;
    p.animation = 0;

    g_status = "Abrindo WebKit PS4 com barra de endereco...";
    int32_t r = g_browser.open(&p);
    if (r != 0) {
        g_status = "Browser WebKit open: " + hex32(r);
        return false;
    }

    bool finished = false;
    int32_t terminalStatus = DIALOG_STATUS_RUNNING;

    /*
     * Correct lifecycle:
     * - GetStatus only observes.
     * - UpdateStatus advances one frame.
     * - GetResult is legal only after FINISHED.
     * - Do NOT call Close after a naturally-finished dialog.
     *
     * v4.1 called GetResult/Close even after other terminal states; on real
     * hardware that was a plausible source of CE-34878-0 when using Back/Exit.
     */
    for (;;) {
        const int32_t observed = g_browser.getStatus ? g_browser.getStatus() : DIALOG_STATUS_RUNNING;
        if (observed == DIALOG_STATUS_FINISHED) {
            finished = true;
            terminalStatus = observed;
            break;
        }

        const int32_t st = g_browser.updateStatus();
        terminalStatus = st;
        if (st == DIALOG_STATUS_FINISHED) {
            finished = true;
            break;
        }
        if (st < 0) {
            g_status = "Browser update status: " + hex32(st);
            return false;
        }

        usleep(16 * 1000);
    }

    if (!finished) {
        g_status = "Browser terminou em estado inesperado: " + hex32(terminalStatus);
        return false;
    }

    BrowserCallbackResultParam cbout{};
    cbout.size = sizeof(cbout);
    BrowserResult result{};
    result.callbackResultParam = &cbout;

    r = g_browser.getResult(&result);
    if (r != 0) {
        g_status = "Browser get result: " + hex32(r);
        return false;
    }

    std::string captured;
    if (result.result == DIALOG_RESULT_CALLBACK && cbout.data)
        captured = cbout.data;
    if (captured.empty() && cbout.buffer && cbout.bufferSize)
        captured.assign(cbout.buffer, cbout.bufferSize);

    // Important: no sceWebBrowserDialogClose() here. FINISHED already owns teardown.

    if (!captured.empty() && looks_like_pkg(captured)) {
        g_status = "Link PKG capturado. Preparando download em segundo plano...";
        return queue_bgft(captured);
    }

    g_status = "Navegador fechado normalmente";
    return true;
}


} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // No SDL menu, no external controller loop: go straight to the PS4 WebKit.
    sceUserServiceInitialize(nullptr);
    const bool ok = open_browser_and_wait();

    // Tear down only after the browser has naturally reached FINISHED.
    if (g_browser.terminate) g_browser.terminate();
    if (g_bgftInit) sceBgftServiceIntTerm();
    if (g_bgftHeap) std::free(g_bgftHeap);
    if (g_httpInit) {
        sceHttpTerm(g_http);
        sceSslTerm();
        sceNetPoolDestroy(g_netPool);
    }
    return ok ? 0 : 1;
}

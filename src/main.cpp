#include <cstddef>
#include <cstdint>
#include <orbis/Bgft.h>
#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/ImeDialog.h>
#include <orbis/CommonDialog.h>
#include <orbis/Pigletv2VSH.h>
#include <orbis/libkernel.h>
#include <SDL2/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <cwchar>
#include <cmath>
#include <pthread.h>


struct PbdlShaderBlob {
    char* ident;
    unsigned char hash[16];
    uint64_t len;
    unsigned char* code;
};
extern "C" PbdlShaderBlob scePrecompiledShaderEntries[];

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
using BrowserGetResultFn = int32_t (*)(BrowserResult*);
using BrowserCloseFn = int32_t (*)();

struct BrowserApi {
    int32_t module = -1;
    BrowserInitializeFn initialize = nullptr;
    BrowserTerminateFn terminate = nullptr;
    BrowserOpenFn open = nullptr;
    BrowserUpdateStatusFn updateStatus = nullptr;
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


static void notify_user(const std::string& message) {
    const int32_t h = load_system_module("libSceSysUtil.sprx");
    if (h < 0) return;
    using NotifyFn = void (*)(int32_t, const char*);
    NotifyFn fn = nullptr;
    if (sceKernelDlsym(h, "sceSysUtilSendSystemNotificationWithText",
                       reinterpret_cast<void**>(&fn)) >= 0 && fn) {
        fn(222, message.c_str());
    }
}


static void clean_exit_to_shell() {
    const int32_t h = load_system_module("libSceSystemService.sprx");
    if (h >= 0) {
        using LoadExecFn = int32_t (*)(const char*, const char**);
        LoadExecFn loadExec = nullptr;
        if (sceKernelDlsym(h, "sceSystemServiceLoadExec", (void**)&loadExec) >= 0 && loadExec) {
            loadExec("exit", nullptr);
            for (;;) usleep(1000 * 1000);
        }
    }
    _Exit(0);
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
    const int tmpl = sceHttpCreateTemplate(g_http, "PS4HybridBrowser/7.6", ORBIS_HTTP_VERSION_1_1, 1);
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


struct UrlProbe {
    bool ok = false;
    int32_t status = 0;
    uint64_t contentLength = 0;
    std::string finalUrl;
    std::string contentType;
    std::string contentDisposition;
};

static std::string lower_copy(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return v;
}

static std::string header_value(const std::string& headers, const char* wanted) {
    const std::string target = lower_copy(std::string(wanted));
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t end = headers.find('\n', pos);
        if (end == std::string::npos) end = headers.size();
        std::string line = headers.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = lower_copy(line.substr(0, colon));
            if (name == target) {
                size_t b = colon + 1;
                while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
                return line.substr(b);
            }
        }
        pos = end + 1;
    }
    return "";
}

static bool split_http_url(const std::string& url, std::string& origin, std::string& path) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    size_t slash = url.find('/', scheme + 3);
    if (slash == std::string::npos) {
        origin = url;
        path = "/";
    } else {
        origin = url.substr(0, slash);
        path = url.substr(slash);
    }
    return origin.rfind("http://", 0) == 0 || origin.rfind("https://", 0) == 0;
}

static std::string resolve_location(const std::string& base, const std::string& loc) {
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return loc;
    if (loc.rfind("//", 0) == 0) return base.substr(0, scheme) + ":" + loc;

    std::string origin, path;
    if (!split_http_url(base, origin, path)) return loc;
    if (!loc.empty() && loc[0] == '/') return origin + loc;

    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path.resize(q);
    size_t slash = path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "/" : path.substr(0, slash + 1);
    return origin + dir + loc;
}

static uint64_t parse_content_range_total(const std::string& v) {
    size_t slash = v.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= v.size() || v[slash + 1] == '*') return 0;
    return strtoull(v.c_str() + slash + 1, nullptr, 10);
}

static UrlProbe probe_url(const std::string& input, const std::string& referer = "") {
    UrlProbe out;
    if (!init_network()) return out;

    std::string current = input;
    for (int redirect = 0; redirect < 8; ++redirect) {
        const int tmpl = sceHttpCreateTemplate(
            g_http,
            "Mozilla/5.0 (PlayStation 4; WebKit) AppleWebKit/605.1.15 Safari/605.1.15",
            ORBIS_HTTP_VERSION_1_1, 1);
        if (tmpl < 0) return out;

        const int conn = sceHttpCreateConnectionWithURL(tmpl, current.c_str(), true);
        if (conn < 0) {
            sceHttpDeleteTemplate(tmpl);
            return out;
        }

        const int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, current.c_str(), 0);
        if (req < 0) {
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            return out;
        }

        sceHttpAddRequestHeader(req, "Range", "bytes=0-0", 1);
        sceHttpAddRequestHeader(req, "Accept", "*/*", 1);
        sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", 1);
        if (!referer.empty()) sceHttpAddRequestHeader(req, "Referer", referer.c_str(), 1);
        sceHttpSetConnectTimeOut(req, 15 * 1000 * 1000);
        sceHttpSetResolveTimeOut(req, 15 * 1000 * 1000);

        int32_t r = sceHttpSendRequest(req, nullptr, 0);
        if (r < 0) {
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            return out;
        }

        int32_t code = 0;
        sceHttpGetStatusCode(req, &code);

        char* raw = nullptr;
        size_t rawSize = 0;
        std::string headers;
        if (sceHttpGetAllResponseHeaders(req, &raw, &rawSize) >= 0 && raw && rawSize)
            headers.assign(raw, rawSize);

        const std::string location = header_value(headers, "location");
        const std::string ctype = header_value(headers, "content-type");
        const std::string cdisp = header_value(headers, "content-disposition");
        const std::string clen = header_value(headers, "content-length");
        const std::string crange = header_value(headers, "content-range");

        uint64_t length = 0;
        if (!crange.empty()) length = parse_content_range_total(crange);
        if (!length && !clen.empty()) length = strtoull(clen.c_str(), nullptr, 10);

        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);

        if (code >= 300 && code < 400 && !location.empty()) {
            current = resolve_location(current, location);
            continue;
        }

        out.ok = (code >= 200 && code < 400);
        out.status = code;
        out.finalUrl = current;
        out.contentType = ctype;
        out.contentDisposition = cdisp;
        out.contentLength = length;
        return out;
    }
    return out;
}

static bool is_file_extension(std::string url) {
    size_t cut = url.find_first_of("?#");
    if (cut != std::string::npos) url.resize(cut);
    url = lower_copy(url);
    static const char* exts[] = {
        ".pkg",".zip",".7z",".rar",".iso",".bin",".chd",".cso",".pbp",".rom",
        ".nes",".sfc",".smc",".gba",".gbc",".gb",".n64",".z64",".nds",".3ds",".cia",
        ".jpg",".jpeg",".png",".gif",".webp",".bmp",".mp4",".mkv",".avi",".mov",
        ".mp3",".flac",".wav",".pdf"
    };
    for (const char* e : exts) {
        const size_t n = std::strlen(e);
        if (url.size() >= n && url.compare(url.size() - n, n, e) == 0) return true;
    }
    return false;
}

static bool probe_is_download(const UrlProbe& p) {
    const std::string ct = lower_copy(p.contentType);
    const std::string cd = lower_copy(p.contentDisposition);
    if (cd.find("attachment") != std::string::npos || cd.find("filename=") != std::string::npos ||
        cd.find("filename*=") != std::string::npos) return true;
    if (is_file_extension(p.finalUrl)) return true;
    if (ct.rfind("image/", 0) == 0 || ct.rfind("video/", 0) == 0 || ct.rfind("audio/", 0) == 0)
        return true;
    if (ct.find("application/octet-stream") != std::string::npos ||
        ct.find("application/zip") != std::string::npos ||
        ct.find("application/x-7z") != std::string::npos ||
        ct.find("application/x-rar") != std::string::npos ||
        ct.find("application/pdf") != std::string::npos ||
        ct.find("application/x-iso9660") != std::string::npos)
        return true;
    return false;
}

static std::string safe_filename(std::string name) {
    if (name.empty()) name = "download.bin";
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    while (!name.empty() && (name.front() == ' ' || name.front() == '.')) name.erase(name.begin());
    if (name.empty()) name = "download.bin";
    if (name.size() > 180) name.resize(180);
    return name;
}

static std::string filename_for_probe(const UrlProbe& p) {
    const std::string cd = p.contentDisposition;
    std::string lower = lower_copy(cd);
    size_t at = lower.find("filename=");
    if (at != std::string::npos) {
        std::string v = cd.substr(at + 9);
        size_t semi = v.find(';');
        if (semi != std::string::npos) v.resize(semi);
        while (!v.empty() && (v.front() == ' ' || v.front() == '"' || v.front() == '\'')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '"' || v.back() == '\'' || v.back() == '\r')) v.pop_back();
        if (!v.empty()) return safe_filename(v);
    }

    std::string u = p.finalUrl;
    size_t cut = u.find_first_of("?#");
    if (cut != std::string::npos) u.resize(cut);
    size_t slash = u.find_last_of('/');
    std::string name = (slash == std::string::npos) ? u : u.substr(slash + 1);
    return safe_filename(name);
}

static bool port_open(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
    close(fd);
    return ok;
}

static bool start_download_daemon() {
    if (port_open(6701)) return true;

    int filefd = open("/app0/daemon.elf", O_RDONLY);
    if (filefd < 0) {
        g_status = "daemon.elf nao encontrado";
        return false;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        close(filefd);
        return false;
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9090);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sockfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        close(sockfd);
        close(filefd);
        g_status = "GoldHEN BinLoader 9090 indisponivel";
        return false;
    }

    char buf[8192];
    for (;;) {
        ssize_t n = read(filefd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            close(sockfd);
            close(filefd);
            return false;
        }
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = write(sockfd, buf + sent, static_cast<size_t>(n - sent));
            if (w <= 0) {
                close(sockfd);
                close(filefd);
                return false;
            }
            sent += w;
        }
    }
    close(sockfd);
    close(filefd);

    for (int i = 0; i < 50; ++i) {
        usleep(100 * 1000);
        if (port_open(6701)) return true;
    }
    g_status = "Daemon nao iniciou na porta 6701";
    return false;
}

static std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 0x20) out += static_cast<char>(c);
                break;
        }
    }
    return out;
}

static bool post_local_json(const char* path, const std::string& body) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(6701);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        close(fd);
        return false;
    }

    std::string req =
        std::string("POST ") + path + " HTTP/1.1\r\n"
        "Host: 127.0.0.1:6701\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

    size_t off = 0;
    while (off < req.size()) {
        ssize_t n = write(fd, req.data() + off, req.size() - off);
        if (n <= 0) {
            close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }

    char response[512]{};
    ssize_t got = read(fd, response, sizeof(response) - 1);
    close(fd);
    if (got <= 0) return false;
    return std::strstr(response, " 200 ") != nullptr;
}

static bool queue_generic_background(const UrlProbe& p) {
    if (!start_download_daemon()) return false;

    std::string origin, path;
    if (!split_http_url(p.finalUrl, origin, path)) {
        g_status = "URL de download invalida";
        return false;
    }

    mkdir("/data/Downloads", 0777);
    const std::string name = filename_for_probe(p);
    const std::string dest = "/data/Downloads/" + name;
    const uint64_t id = (static_cast<uint64_t>(time(nullptr)) << 16) ^
                        static_cast<uint64_t>(getpid() & 0xFFFF);

    std::string body =
        "{\"type\":4,\"url\":\"" + json_escape(origin) +
        "\",\"username\":\"\",\"password\":\"\",\"http_server_type\":\"" +
        "\",\"src_path\":\"" + json_escape(path) +
        "\",\"dest_path\":\"" + json_escape(dest) +
        "\",\"size\":" + std::to_string(p.contentLength) +
        ",\"id\":" + std::to_string(id) + "}";

    if (!post_local_json("/download_url", body)) {
        g_status = "Falha enviando download ao daemon";
        return false;
    }

    g_status = "Download em segundo plano: " + name;
    g_lastUrl = p.finalUrl;
    return true;
}


static void append_diag(const char* tag, const std::string& value) {
    mkdir("/data/PBDL", 0777);
    int fd = open("/data/PBDL/navigation.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    char tbuf[64];
    std::snprintf(tbuf, sizeof(tbuf), "%lld", static_cast<long long>(time(nullptr)));
    std::string line = std::string(tbuf) + " [" + tag + "] " + value + "\n";
    (void)write(fd, line.data(), line.size());
    close(fd);
}

static std::string regex_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 2);
    const char* met = R"(\.^$|()[]{}*+?)";
    for (char c : in) {
        if (std::strchr(met, c)) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static std::string make_navigation_regex(const std::string&) {
    // Hybrid v7 only intercepts our own explicit handoff route.
    // Normal browsing inside the backend UI never finishes WebBrowserDialog.
    return "^https?://[^/]+/handoff/.*$";
}

struct TextResponse {
    bool ok = false;
    int32_t status = 0;
    std::string finalUrl;
    std::string headers;
    std::string body;
};

static TextResponse fetch_text(const std::string& input,
                               const std::string& referer = "",
                               const std::string& postData = "",
                               const std::string& cookie = "") {
    TextResponse out;
    if (!init_network()) return out;

    std::string current = input;
    for (int redirect = 0; redirect < 8; ++redirect) {
        const int tmpl = sceHttpCreateTemplate(
            g_http,
            "Mozilla/5.0 (PlayStation 4; WebKit) AppleWebKit/605.1.15 Safari/605.1.15",
            ORBIS_HTTP_VERSION_1_1, 1);
        if (tmpl < 0) return out;

        const int conn = sceHttpCreateConnectionWithURL(tmpl, current.c_str(), true);
        if (conn < 0) {
            sceHttpDeleteTemplate(tmpl);
            return out;
        }

        const bool doPost = !postData.empty();
        const int req = sceHttpCreateRequestWithURL(
            conn, doPost ? ORBIS_METHOD_POST : ORBIS_METHOD_GET, current.c_str(),
            doPost ? postData.size() : 0);
        if (req < 0) {
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            return out;
        }

        sceHttpAddRequestHeader(req, "Accept", "text/html,application/xhtml+xml,*/*;q=0.8", 1);
        sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", 1);
        if (!referer.empty()) sceHttpAddRequestHeader(req, "Referer", referer.c_str(), 1);
        if (!cookie.empty()) sceHttpAddRequestHeader(req, "Cookie", cookie.c_str(), 1);
        if (doPost)
            sceHttpAddRequestHeader(req, "Content-Type", "application/x-www-form-urlencoded", 1);
        sceHttpSetConnectTimeOut(req, 15 * 1000 * 1000);
        sceHttpSetResolveTimeOut(req, 15 * 1000 * 1000);

        int32_t r = sceHttpSendRequest(req, doPost ? postData.data() : nullptr,
                                      doPost ? postData.size() : 0);
        if (r < 0) {
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            return out;
        }

        int32_t code = 0;
        sceHttpGetStatusCode(req, &code);

        char* raw = nullptr;
        size_t rawSize = 0;
        std::string headers;
        if (sceHttpGetAllResponseHeaders(req, &raw, &rawSize) >= 0 && raw && rawSize)
            headers.assign(raw, rawSize);

        const std::string location = header_value(headers, "location");
        if (code >= 300 && code < 400 && !location.empty()) {
            current = resolve_location(current, location);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            continue;
        }

        std::string body;
        body.reserve(64 * 1024);
        char buf[16 * 1024];
        while (body.size() < 2 * 1024 * 1024) {
            int n = sceHttpReadData(req, buf, sizeof(buf));
            if (n <= 0) break;
            body.append(buf, static_cast<size_t>(n));
        }

        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);

        out.ok = (code >= 200 && code < 400);
        out.status = code;
        out.finalUrl = current;
        out.headers = headers;
        out.body = body;
        return out;
    }
    return out;
}

static std::string html_attr_near(const std::string& html,
                                  const std::string& marker,
                                  const std::string& attr) {
    size_t p = html.find(marker);
    if (p == std::string::npos) return "";
    size_t begin = (p > 2048) ? p - 2048 : 0;
    size_t end = std::min(html.size(), p + 4096);
    std::string area = html.substr(begin, end - begin);

    std::string needle1 = attr + "=\"";
    size_t a = area.find(needle1);
    if (a != std::string::npos) {
        a += needle1.size();
        size_t b = area.find('"', a);
        if (b != std::string::npos) return area.substr(a, b - a);
    }
    std::string needle2 = attr + "='";
    a = area.find(needle2);
    if (a != std::string::npos) {
        a += needle2.size();
        size_t b = area.find('\'', a);
        if (b != std::string::npos) return area.substr(a, b - a);
    }
    return "";
}

static std::string resolve_known_filehost(const std::string& input,
                                          const std::string& referer) {
    const std::string low = lower_copy(input);

    // PixelDrain public file page -> direct API endpoint.
    const std::string px = "https://pixeldrain.com/u/";
    size_t p = low.find(px);
    if (p == 0) {
        std::string id = input.substr(px.size());
        size_t cut = id.find_first_of("?#/");
        if (cut != std::string::npos) id.resize(cut);
        if (!id.empty()) {
            std::string direct = "https://pixeldrain.com/api/file/" + id + "?download=";
            append_diag("RESOLVER_PIXELDRAIN", direct);
            return direct;
        }
    }

    // MediaFire exposes the direct URL on #downloadButton.
    if (low.find("mediafire.com/") != std::string::npos) {
        TextResponse r = fetch_text(input, referer);
        if (r.ok) {
            std::string href = html_attr_near(r.body, "downloadButton", "href");
            if (!href.empty()) {
                href = resolve_location(r.finalUrl, href);
                append_diag("RESOLVER_MEDIAFIRE", href);
                return href;
            }
        }
    }

    // 1fichier free download flow: GET -> adz hidden value -> POST -> orange download href.
    if (low.find("1fichier.com/") != std::string::npos) {
        TextResponse first = fetch_text(input, referer);
        if (first.ok) {
            std::string adz = html_attr_near(first.body, "name=\"adz\"", "value");
            if (adz.empty()) adz = html_attr_near(first.body, "name='adz'", "value");
            if (!adz.empty()) {
                std::string cookie;
                size_t sc = lower_copy(first.headers).find("set-cookie:");
                if (sc != std::string::npos) {
                    size_t b = sc + 11;
                    while (b < first.headers.size() && (first.headers[b] == ' ' || first.headers[b] == '\t')) ++b;
                    size_t e = first.headers.find_first_of(";\r\n", b);
                    if (e != std::string::npos) cookie = first.headers.substr(b, e - b);
                }
                std::string post = "adz=" + adz + "&did=0&dl_no_ssl=off&dlinline=on";
                TextResponse second = fetch_text(first.finalUrl, first.finalUrl, post, cookie);
                if (second.ok) {
                    std::string href = html_attr_near(second.body, "btn-orange", "href");
                    if (!href.empty()) {
                        href = resolve_location(second.finalUrl, href);
                        append_diag("RESOLVER_1FICHIER", href);
                        return href;
                    }
                }
            }
        }
    }

    return input;
}

static bool handle_captured_url(const std::string& captured, const std::string& referer, std::string& reopenUrl) {
    append_diag("CAPTURE", captured);

    if (captured.rfind("blob:", 0) == 0 || captured.rfind("data:", 0) == 0) {
        // BrowserDialog cannot expose the bytes behind a blob/data navigation.
        // Keep the originating page alive and record the exact unsupported scheme.
        append_diag("BROWSERDIALOG_SCHEME_LIMIT", captured);
        g_status = "Link blob/data capturado; requer WebKit2 direto.";
        reopenUrl = referer;
        return false;
    }

    const std::string resolved = resolve_known_filehost(captured, referer);
    if (resolved != captured) append_diag("RESOLVED", resolved);

    UrlProbe p = probe_url(resolved, referer);
    if (!p.ok) {
        // Warm DNS/TLS with a second short attempt; the user's SuperPSX tests
        // consistently succeed on the second navigation.
        usleep(250 * 1000);
        p = probe_url(resolved, referer);
    }

    if (!p.ok) {
        append_diag("PROBE_FAIL", resolved);
        // Protected pages may refuse our native probe while still rendering in WebKit.
        reopenUrl = resolved;
        return false;
    }

    append_diag("PROBE",
        std::to_string(p.status) + " " + p.contentType + " " +
        std::to_string(p.contentLength) + " " + p.finalUrl);

    // Detect PKG by file magic, not only by the extension.
    PkgInfo info;
    if (fetch_pkg_header(p.finalUrl, info)) {
        append_diag("PKG", p.finalUrl);
        g_status = "PKG detectado. Enviando ao BGFT...";
        return queue_bgft(p.finalUrl);
    }

    if (probe_is_download(p)) {
        append_diag("FILE", p.finalUrl);
        if (queue_generic_background(p)) return true;
        reopenUrl = referer;
        return false;
    }

    // It is a normal page/navigation. Reopen it ourselves in the same browser
    // instead of letting WebBrowserDialog attempt popup/new-window handling.
    append_diag("PAGE", p.finalUrl);
    reopenUrl = p.finalUrl.empty() ? resolved : p.finalUrl;
    return false;
}


static std::string trim_copy(std::string v) {
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ' || v.back() == '\t'))
        v.pop_back();
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '\r' || v[i] == '\n'))
        ++i;
    if (i) v.erase(0, i);
    return v;
}

static std::string read_small_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return "";
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "";
    buf[n] = 0;
    return trim_copy(std::string(buf));
}

static void write_small_file(const char* path, const std::string& value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    (void)write(fd, value.data(), value.size());
    close(fd);
}


static bool backend_healthy(std::string base) {
    if (base.empty()) return false;
    while (!base.empty() && base.back() == '/') base.pop_back();
    UrlProbe p = probe_url(base + "/health");
    return p.ok && p.status >= 200 && p.status < 300;
}

static bool backend_ui_healthy(std::string base) {
    if (base.empty()) return false;
    while (!base.empty() && base.back() == '/') base.pop_back();
    UrlProbe p = probe_url(base + "/ps4");
    return p.ok && p.status >= 200 && p.status < 300;
}


static std::string discover_hybrid_backend() {
    if (!init_network()) {
        const std::string cached = read_small_file("/data/PBDL/backend.txt");
        return backend_healthy(cached) && backend_ui_healthy(cached) ? cached : "";
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        const std::string cached = read_small_file("/data/PBDL/backend.txt");
        return backend_healthy(cached) && backend_ui_healthy(cached) ? cached : "";
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(32123);
    dst.sin_addr.s_addr = inet_addr("255.255.255.255");

    const char* hello = "PBDL_DISCOVER_V1";
    std::string found;

    for (int tick = 0; tick < 50 && found.empty(); ++tick) {
        if (tick == 0 || tick == 10 || tick == 20 || tick == 35)
            (void)sendto(fd, hello, std::strlen(hello), 0,
                         reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

        char buf[512];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n > 0) {
            buf[n] = 0;
            const std::string msg = trim_copy(std::string(buf));

            // v7.1: backend returns only its TCP port. We deliberately build
            // the URL from the UDP packet's SOURCE address, avoiding VPN/
            // VirtualBox/Tailscale interfaces on the PC.
            const std::string portPrefix = "PBDL_BACKEND_PORT ";
            if (msg.compare(0, portPrefix.size(), portPrefix) == 0) {
                const int port = std::atoi(msg.substr(portPrefix.size()).c_str());
                const char* ip = inet_ntoa(from.sin_addr);
                if (ip && port > 0 && port <= 65535) {
                    found = std::string("http://") + ip + ":" + std::to_string(port);
                }
            } else {
                // Backward compatibility with v7 backend.
                const std::string prefix = "PBDL_BACKEND ";
                if (msg.compare(0, prefix.size(), prefix) == 0)
                    found = trim_copy(msg.substr(prefix.size()));
            }

            if (!found.empty() &&
                !(backend_healthy(found) && backend_ui_healthy(found))) {
                append_diag("HYBRID_HEALTH_FAIL", found);
                found.clear();
            }
        }
        usleep(100 * 1000);
    }
    close(fd);

    if (!found.empty()) {
        mkdir("/data/PBDL", 0777);
        write_small_file("/data/PBDL/backend.txt", found);
        append_diag("HYBRID_BACKEND", found);
        return found;
    }

    const std::string cached = read_small_file("/data/PBDL/backend.txt");
    if (!cached.empty() && backend_healthy(cached) && backend_ui_healthy(cached)) {
        append_diag("HYBRID_BACKEND_CACHE", cached);
        return cached;
    }

    append_diag("HYBRID_BACKEND", "nao encontrado");
    return "";
}

static bool handle_hybrid_handoff(const std::string& captured,
                                  const std::string& browserHome) {
    const size_t pos = captured.find("/handoff/");
    if (pos == std::string::npos) return false;

    append_diag("HYBRID_HANDOFF", captured);

    std::string fileUrl = captured;
    fileUrl.replace(pos, 9, "/file/");
    append_diag("HYBRID_FILE_URL", fileUrl);

    // Never trust just the extension/kind sent by the backend. Read the real
    // PS4 package magic so extensionless PKGs still go through BGFT.
    PkgInfo pkg;
    if (fetch_pkg_header(fileUrl, pkg)) {
        g_status = "Hybrid: PKG real detectado; enviando ao BGFT...";
        const bool ok = queue_bgft(fileUrl);
        notify_user(ok ? "Hybrid: PKG enviado ao BGFT." :
                         ("Hybrid: falha no BGFT - " + g_status));
        return ok;
    }

    UrlProbe probe = probe_url(fileUrl, browserHome);
    if (!probe.ok) {
        usleep(250 * 1000);
        probe = probe_url(fileUrl, browserHome);
    }
    if (!probe.ok) {
        g_status = "Hybrid: backend nao respondeu ao arquivo";
        notify_user(g_status);
        return false;
    }

    g_status = "Hybrid: enviando arquivo ao daemon...";
    const bool ok = queue_generic_background(probe);
    notify_user(ok ? ("Hybrid: download iniciado - " + filename_for_probe(probe))
                   : ("Hybrid: falha no download - " + g_status));
    return ok;
}

static bool open_browser_and_wait(const char* requestedUrl = nullptr) {
    if (!init_browser()) return false;

    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);

    static const char* fallbackHome = "https://www.google.com/?hl=pt-BR";
    const std::string startUrl =
        (requestedUrl && *requestedUrl) ? requestedUrl : fallbackHome;
    const std::string callbackRegex = make_navigation_regex(startUrl);

    for (;;) {
        BrowserCallbackInitParam cb{};
        cb.size = sizeof(cb);
        cb.type = CALLBACK_TYPE_REGEXP;
        cb.data = callbackRegex.c_str();

        BrowserParam p{};
        p.baseParam.size = sizeof(CommonDialogBaseParam);
        p.baseParam.magic = DIALOG_MAGIC +
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&p.baseParam));
        p.size = sizeof(p);
        p.mode = BROWSER_MODE_DEFAULT;
        p.userId = user;
        p.url = startUrl.c_str();
        p.callbackInitParam = &cb;

        g_status = "Hybrid v7.2: abrindo interface simples...";
        int32_t r = g_browser.open(&p);
        if (r != 0) {
            g_status = "Hybrid browser open: " + hex32(r);
            notify_user(g_status);
            return false;
        }

        for (;;) {
            const int32_t st = g_browser.updateStatus();
            if (st == DIALOG_STATUS_FINISHED) break;
            if (st < 0) {
                g_status = "Hybrid update status: " + hex32(st);
                notify_user(g_status);
                return false;
            }
            usleep(16 * 1000);
        }

        BrowserCallbackResultParam cbout{};
        cbout.size = sizeof(cbout);
        BrowserResult result{};
        result.callbackResultParam = &cbout;

        r = g_browser.getResult(&result);
        if (r != 0) {
            g_status = "Hybrid get result: " + hex32(r);
            notify_user(g_status);
            return false;
        }

        std::string captured;
        if (result.result == DIALOG_RESULT_CALLBACK && cbout.data)
            captured = cbout.data;
        if (captured.empty() && cbout.buffer && cbout.bufferSize)
            captured.assign(cbout.buffer, cbout.bufferSize);

        if (!captured.empty() && captured.find("/handoff/") != std::string::npos) {
            (void)handle_hybrid_handoff(captured, startUrl);
            // Intentional callback closes the dialog. Reopen in this loop,
            // not recursively, so many downloads cannot grow the stack.
            continue;
        }

        g_status = "Hybrid v7.1 fechado normalmente";
        return true;
    }
}


static bool http_get_bytes_native(const std::string& url,
                                  std::vector<uint8_t>& out,
                                  int32_t* statusOut = nullptr) {
    out.clear();
    if (!init_network()) return false;

    const int tmpl = sceHttpCreateTemplate(
        g_http, "PS4HybridBrowser/7.6", ORBIS_HTTP_VERSION_1_1, 1);
    if (tmpl < 0) return false;

    const int conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), true);
    if (conn < 0) {
        sceHttpDeleteTemplate(tmpl);
        return false;
    }

    const int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return false;
    }

    sceHttpAddRequestHeader(req, "Accept", "*/*", 1);
    sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", 1);
    sceHttpSetConnectTimeOut(req, 5 * 1000 * 1000);
    sceHttpSetResolveTimeOut(req, 5 * 1000 * 1000);

    const int32_t send = sceHttpSendRequest(req, nullptr, 0);
    if (send < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return false;
    }

    int32_t status = 0;
    sceHttpGetStatusCode(req, &status);
    if (statusOut) *statusOut = status;
    if (status < 200 || status >= 300) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return false;
    }

    uint8_t buf[32 * 1024];
    for (;;) {
        const int n = sceHttpReadData(req, buf, sizeof(buf));
        if (n < 0) {
            out.clear();
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            return false;
        }
        if (n == 0) break;
        out.insert(out.end(), buf, buf + n);
        if (out.size() > 12 * 1024 * 1024) {
            out.clear();
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            return false;
        }
    }

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tmpl);
    return !out.empty();
}

static bool http_get_text_native(const std::string& url,
                                 std::string& out,
                                 int32_t* statusOut = nullptr) {
    std::vector<uint8_t> bytes;
    int32_t status = 0;
    const bool ok = http_get_bytes_native(url, bytes, &status);
    if (statusOut) *statusOut = status;
    if (!ok) {
        out.clear();
        return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out = trim_copy(out);
    return true;
}

static bool http_post_json_native(const std::string& url,
                                  const std::string& body) {
    if (!init_network()) return false;

    const int tmpl = sceHttpCreateTemplate(
        g_http, "PS4HybridBrowser/7.6", ORBIS_HTTP_VERSION_1_1, 1);
    if (tmpl < 0) return false;

    const int conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), true);
    if (conn < 0) {
        sceHttpDeleteTemplate(tmpl);
        return false;
    }

    const int req = sceHttpCreateRequestWithURL(
        conn, ORBIS_METHOD_POST, url.c_str(), body.size());
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return false;
    }

    sceHttpAddRequestHeader(req, "Content-Type", "application/json", 1);
    sceHttpAddRequestHeader(req, "Accept", "application/json,text/plain,*/*", 1);
    sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", 1);
    sceHttpSetConnectTimeOut(req, 5 * 1000 * 1000);
    sceHttpSetResolveTimeOut(req, 5 * 1000 * 1000);

    const int32_t send = sceHttpSendRequest(
        req, body.empty() ? nullptr : body.data(), body.size());
    int32_t status = 0;
    if (send >= 0) sceHttpGetStatusCode(req, &status);

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tmpl);
    return send >= 0 && status >= 200 && status < 300;
}

static bool http_post_empty_native(const std::string& url) {
    return http_post_json_native(url, "{}");
}

static std::string wide_to_utf8(const wchar_t* ws) {
    std::string out;
    if (!ws) return out;
    for (size_t i = 0; ws[i]; ++i) {
        uint32_t cp = static_cast<uint32_t>(ws[i]);
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

static bool open_url_ime(std::string& value) {
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
    (void)sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    (void)sceCommonDialogInitialize();

    // PS4 IME ABI is UTF-16 even though wchar_t is 32-bit on this target.
    uint16_t buffer[1024]{};
    static const uint16_t title[] = {
        'U','R','L',' ','o','u',' ','p','e','s','q','u','i','s','a',0
    };
    static const uint16_t placeholder[] = {
        'D','i','g','i','t','e',' ','u','m',' ','e','n','d','e','r','e','c','o',
        ' ','o','u',' ','p','e','s','q','u','i','s','a',0
    };

    OrbisImeDialogSetting setting{};
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);
    setting.userId = static_cast<uint32_t>(user);
    setting.type = ORBIS_TYPE_DEFAULT;
    setting.supportedLanguages = 0;
    setting.enterLabel = ORBIS_BUTTON_LABEL_GO;
    setting.inputMethod = ORBIS__DEFAULT;
    setting.maxTextLength = 1023;
    setting.inputTextBuffer = reinterpret_cast<wchar_t*>(buffer);
    setting.posx = 960.0f;
    setting.posy = 540.0f;
    setting.horizontalAlignment = ORBIS_H_CENTER;
    setting.verticalAlignment = ORBIS_V_CENTER;
    setting.placeholder = reinterpret_cast<const wchar_t*>(placeholder);
    setting.title = reinterpret_cast<const wchar_t*>(title);

    const int32_t r = sceImeDialogInit(&setting, nullptr);
    if (r < 0) {
        notify_user("Hybrid: falha abrindo teclado - " + hex32(r));
        return false;
    }

    OrbisDialogStatus status = sceImeDialogGetStatus();
    while (status == ORBIS_DIALOG_STATUS_RUNNING) {
        usleep(16 * 1000);
        status = sceImeDialogGetStatus();
    }

    OrbisDialogResult result{};
    const bool accepted =
        status == ORBIS_DIALOG_STATUS_STOPPED &&
        sceImeDialogGetResult(&result) >= 0 &&
        result.endstatus == ORBIS_DIALOG_OK;

    (void)sceImeDialogTerm();
    if (!accepted) return false;

    value.clear();
    for (size_t i = 0; i < 1023 && buffer[i] != 0; ++i) {
        const uint32_t cp = buffer[i];
        if (cp < 0x80) {
            value.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            value.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            value.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            value.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            value.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            value.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return !value.empty();
}

struct FrameWorker {
    std::string backend;
    pthread_t thread{};
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    volatile bool stop = false;
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    uint64_t generation = 0;
    bool everLoaded = false;
};

static bool parse_http_backend(const std::string& backend,
                               std::string& host,
                               int& port) {
    const std::string prefix = "http://";
    if (backend.compare(0, prefix.size(), prefix) != 0) return false;
    std::string rest = backend.substr(prefix.size());
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest.resize(slash);
    const size_t colon = rest.rfind(':');
    if (colon == std::string::npos) {
        host = rest;
        port = 80;
    } else {
        host = rest.substr(0, colon);
        port = std::atoi(rest.substr(colon + 1).c_str());
    }
    return !host.empty() && port > 0 && port <= 65535;
}

static bool raw_http_get_local(const std::string& backend,
                               const std::string& path,
                               std::vector<uint8_t>& body,
                               uint64_t* frameSeq = nullptr) {
    body.clear();
    std::string host;
    int port = 0;
    if (!parse_http_backend(backend, host, port)) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    sa.sin_addr.s_addr = inet_addr(host.c_str());
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        close(fd);
        return false;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        close(fd);
        return false;
    }

    const std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Accept: image/jpeg,image/png\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n\r\n";

    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = write(fd, req.data() + sent, req.size() - sent);
        if (n <= 0) {
            close(fd);
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    std::vector<uint8_t> response;
    response.reserve(2 * 1024 * 1024);
    uint8_t buf[32 * 1024];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            return false;
        }
        if (n == 0) break;
        response.insert(response.end(), buf, buf + n);
        if (response.size() > 12 * 1024 * 1024) {
            close(fd);
            return false;
        }
    }
    close(fd);

    static const uint8_t sep[] = {'\r','\n','\r','\n'};
    auto it = std::search(response.begin(), response.end(),
                          sep, sep + sizeof(sep));
    if (it == response.end()) return false;

    const size_t headerLen =
        static_cast<size_t>(it - response.begin()) + sizeof(sep);
    std::string header(reinterpret_cast<const char*>(response.data()),
                       headerLen);
    if (header.find(" 200 ") == std::string::npos) return false;

    if (frameSeq) {
        std::string lowerHeader = header;
        std::transform(lowerHeader.begin(), lowerHeader.end(),
                       lowerHeader.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        const std::string key = "x-frame-seq:";
        const size_t p = lowerHeader.find(key);
        if (p != std::string::npos) {
            const char* start = header.c_str() + p + key.size();
            *frameSeq = static_cast<uint64_t>(std::strtoull(start, nullptr, 10));
        } else {
            *frameSeq = 0;
        }
    }

    body.assign(response.begin() + headerLen, response.end());
    return !body.empty();
}

static void* frame_worker_main(void* arg) {
    FrameWorker* worker = static_cast<FrameWorker*>(arg);
    uint64_t counter = 0;

    uint64_t lastFrameSeq = 0;
    while (!worker->stop) {
        std::vector<uint8_t> imageBytes;
        const std::string path =
            "/shot-fast?t=" + std::to_string(++counter);
        uint64_t frameSeq = 0;

        if (raw_http_get_local(worker->backend, path, imageBytes, &frameSeq) &&
            (frameSeq == 0 || frameSeq != lastFrameSeq)) {
            int w = 0, h = 0, channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(
                imageBytes.data(), static_cast<int>(imageBytes.size()),
                &w, &h, &channels, 4);

            if (pixels && w > 0 && h > 0) {
                const size_t size =
                    static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
                pthread_mutex_lock(&worker->mutex);
                worker->rgba.assign(pixels, pixels + size);
                worker->width = w;
                worker->height = h;
                worker->generation++;
                worker->everLoaded = true;
                pthread_mutex_unlock(&worker->mutex);
                if (frameSeq) lastFrameSeq = frameSeq;
            }
            if (pixels) stbi_image_free(pixels);
        }

        // CDP screencast is push-driven on the PC; poll the latest frame often.
        for (int i = 0; i < 2 && !worker->stop; ++i)
            usleep(10 * 1000);
    }
    return nullptr;
}

static bool start_frame_worker(FrameWorker& worker,
                               const std::string& backend) {
    worker.backend = backend;
    worker.stop = false;
    return pthread_create(&worker.thread, nullptr,
                          frame_worker_main, &worker) == 0;
}

static void stop_frame_worker(FrameWorker& worker) {
    worker.stop = true;
    pthread_join(worker.thread, nullptr);
    pthread_mutex_destroy(&worker.mutex);
}

static bool consume_latest_frame(FrameWorker& worker,
                                 uint64_t& seenGeneration,
                                 SDL_Surface* scaledSurface) {
    std::vector<uint8_t> copy;
    int w = 0, h = 0;
    uint64_t generation = 0;

    pthread_mutex_lock(&worker.mutex);
    generation = worker.generation;
    if (generation != seenGeneration && !worker.rgba.empty()) {
        copy = worker.rgba;
        w = worker.width;
        h = worker.height;
    }
    pthread_mutex_unlock(&worker.mutex);

    if (copy.empty() || w <= 0 || h <= 0) return false;

    SDL_Surface* source = SDL_CreateRGBSurfaceFrom(
        copy.data(), w, h, 32, w * 4,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!source) return false;

    SDL_FillRect(scaledSurface, nullptr,
                 SDL_MapRGB(scaledSurface->format, 0, 0, 0));
    SDL_Rect dst{0, 0, SCREEN_W, SCREEN_H};
    const int rc = SDL_BlitScaled(source, nullptr, scaledSurface, &dst);
    SDL_FreeSurface(source);

    if (rc == 0) {
        seenGeneration = generation;
        return true;
    }
    return false;
}

static void draw_cursor(SDL_Surface* surface, int x, int y) {
    const Uint32 outer = SDL_MapRGB(surface->format, 0, 0, 0);
    const Uint32 inner = SDL_MapRGB(surface->format, 255, 255, 255);
    const Uint32 center = SDL_MapRGB(surface->format, 255, 45, 45);

    SDL_Rect ho{x - 17, y - 3, 35, 7};
    SDL_Rect vo{x - 3, y - 17, 7, 35};
    SDL_FillRect(surface, &ho, outer);
    SDL_FillRect(surface, &vo, outer);

    SDL_Rect hi{x - 15, y - 1, 31, 3};
    SDL_Rect vi{x - 1, y - 15, 3, 31};
    SDL_FillRect(surface, &hi, inner);
    SDL_FillRect(surface, &vi, inner);

    SDL_Rect dot{x - 2, y - 2, 5, 5};
    SDL_FillRect(surface, &dot, center);
}

static bool backend_api(const std::string& backend,
                        const char* path,
                        const std::string& json = "{}") {
    return http_post_json_native(backend + path, json);
}

static bool backend_focus_is_editable(const std::string& backend) {
    std::string text;
    int32_t status = 0;
    return http_get_text_native(
               backend + "/api/focus-info", text, &status) &&
           text == "1";
}

static bool process_pending_download(const std::string& backend) {
    std::string handoff;
    int32_t status = 0;
    if (!http_get_text_native(backend + "/api/pending-download", handoff, &status))
        return false;
    if (handoff.empty()) return false;

    const size_t pos = handoff.find("/handoff/");
    if (pos == std::string::npos) {
        (void)backend_api(backend, "/api/ack-download");
        return false;
    }

    std::string fileUrl = backend + handoff;
    const size_t fullPos = fileUrl.find("/handoff/");
    fileUrl.replace(fullPos, 9, "/file/");
    append_diag("NATIVE_FILE_URL", fileUrl);

    bool ok = false;
    PkgInfo pkg;
    if (fetch_pkg_header(fileUrl, pkg)) {
        g_status = "Native: PKG detectado; BGFT...";
        ok = queue_bgft(fileUrl);
        notify_user(ok ? "Hybrid: PKG enviado ao BGFT."
                       : ("Hybrid: falha no BGFT - " + g_status));
    } else {
        UrlProbe probe = probe_url(fileUrl, backend);
        if (!probe.ok) {
            usleep(250 * 1000);
            probe = probe_url(fileUrl, backend);
        }
        if (probe.ok) {
            ok = queue_generic_background(probe);
            notify_user(ok ? ("Hybrid: download iniciado - " +
                              filename_for_probe(probe))
                           : ("Hybrid: falha no download - " + g_status));
        } else {
            notify_user("Hybrid: backend nao entregou o arquivo.");
        }
    }

    (void)backend_api(backend, "/api/ack-download");
    return ok;
}

static float cursor_axis_velocity(uint8_t raw) {
    const float centered = static_cast<float>(static_cast<int>(raw) - 128);
    const float magnitude = std::fabs(centered);
    const float deadzone = 8.0f;
    if (magnitude <= deadzone) return 0.0f;

    float t = (magnitude - deadzone) / (127.0f - deadzone);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Gentle precision near center, rapid travel at larger deflection.
    const float speed = 120.0f * t + 2800.0f * t * t;
    return centered < 0.0f ? -speed : speed;
}

struct DownloadWorker {
    std::string backend;
    pthread_t thread{};
    volatile bool stop = false;
};

static void* download_worker_main(void* arg) {
    DownloadWorker* worker = static_cast<DownloadWorker*>(arg);
    while (!worker->stop) {
        (void)process_pending_download(worker->backend);
        for (int i = 0; i < 150 && !worker->stop; ++i)
            usleep(10 * 1000);
    }
    return nullptr;
}

static bool start_download_worker(DownloadWorker& worker,
                                  const std::string& backend) {
    worker.backend = backend;
    worker.stop = false;
    return pthread_create(&worker.thread, nullptr,
                          download_worker_main, &worker) == 0;
}

static void stop_download_worker(DownloadWorker& worker) {
    worker.stop = true;
    pthread_join(worker.thread, nullptr);
}

static SDL_Rect cursor_dirty_rect(int x, int y) {
    SDL_Rect r{x - 22, y - 22, 45, 45};
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > SCREEN_W) r.w = SCREEN_W - r.x;
    if (r.y + r.h > SCREEN_H) r.h = SCREEN_H - r.y;
    return r;
}

static void restore_region_from_frame(SDL_Surface* frame,
                                      SDL_Surface* screen,
                                      const SDL_Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) return;
    SDL_Rect srcRect = rect;
    SDL_Rect dstRect = rect;
    SDL_BlitSurface(frame, &srcRect, screen, &dstRect);
}

static void present_cursor_only(SDL_Window* window,
                                SDL_Surface* frame,
                                SDL_Surface* screen,
                                int oldX, int oldY,
                                int newX, int newY) {
    SDL_Rect rects[2] = {
        cursor_dirty_rect(oldX, oldY),
        cursor_dirty_rect(newX, newY)
    };

    restore_region_from_frame(frame, screen, rects[0]);
    restore_region_from_frame(frame, screen, rects[1]);
    draw_cursor(screen, newX, newY);

    if (SDL_UpdateWindowSurfaceRects(window, rects, 2) != 0) {
        // Conservative fallback if the PS4 SDL backend rejects partial update.
        SDL_UpdateWindowSurface(window);
    }
}


struct GpuRenderer {
    int32_t piglet = -1;
    int32_t precompiled = -1;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0;
    GLuint texture = 0;
    GLint aVertex = -1;
    GLint uModel = -1;
    GLint uProjection = -1;
    GLint uTexSpace = -1;
    GLint uOpacity = -1;
    GLint uSampler = -1;
    int texW = 0;
    int texH = 0;
};

static PbdlShaderBlob* find_precompiled_shader(const char* name) {
    for (PbdlShaderBlob* e = scePrecompiledShaderEntries;
         e && e->ident; ++e) {
        if (std::strcmp(e->ident, name) == 0) return e;
    }
    return nullptr;
}

static bool gpu_load_modules(GpuRenderer& g) {
    const char* word = sceKernelGetFsSandboxRandomWord();
    if (!word) return false;
    const std::string prefix = std::string("/") + word + "/common/lib/";

    int mstart = 0;
    g.piglet = static_cast<int32_t>(sceKernelLoadStartModule(
        (prefix + "libScePigletv2VSH.sprx").c_str(),
        0, nullptr, 0, nullptr, &mstart));
    if (g.piglet < 0) return false;

    g.precompiled = static_cast<int32_t>(sceKernelLoadStartModule(
        (prefix + "libScePrecompiledShaders.sprx").c_str(),
        0, nullptr, 0, nullptr, &mstart));
    return g.precompiled >= 0;
}

static bool gpu_init_context(GpuRenderer& g) {
    OrbisPglConfig cfg{};
    cfg.size = sizeof(cfg);
    cfg.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT |
                ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    cfg.processOrder = 1;
    cfg.systemSharedMemorySize = 250ULL * 1024ULL * 1024ULL;
    cfg.videoSharedMemorySize = 512ULL * 1024ULL * 1024ULL;
    cfg.maxMappedFlexibleMemory = 170ULL * 1024ULL * 1024ULL;
    cfg.drawCommandBufferSize = 1ULL * 1024ULL * 1024ULL;
    cfg.lcueResourceBufferSize = 1ULL * 1024ULL * 1024ULL;
    cfg.dbgPosCmd_0x40 = SCREEN_W;
    cfg.dbgPosCmd_0x44 = SCREEN_H;
    cfg.unk_0x5C = 2;

    if (!scePigletSetConfigurationVSH(&cfg)) return false;

    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g.display == EGL_NO_DISPLAY) return false;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(g.display, &major, &minor)) return false;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;

    const EGLint attribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_SAMPLES, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig config{};
    EGLint numConfigs = 0;
    if (!eglChooseConfig(g.display, attribs, &config, 1, &numConfigs) ||
        numConfigs < 1) return false;

    OrbisPglWindow window{0,
        static_cast<khronos_uint32_t>(SCREEN_W),
        static_cast<khronos_uint32_t>(SCREEN_H), 0};
    const EGLint windowAttribs[] = {
        EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE
    };
    g.surface = eglCreateWindowSurface(
        g.display, config, &window, windowAttribs);
    if (g.surface == EGL_NO_SURFACE) return false;

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    g.context = eglCreateContext(
        g.display, config, EGL_NO_CONTEXT, contextAttribs);
    if (g.context == EGL_NO_CONTEXT) return false;

    if (!eglMakeCurrent(g.display, g.surface, g.surface, g.context))
        return false;

    // Prefer display-synchronised 60 Hz. If unsupported Piglet simply
    // keeps the current swap interval.
    (void)eglSwapInterval(g.display, 1);
    return true;
}

static bool gpu_init_program(GpuRenderer& g) {
    PbdlShaderBlob* vert = find_precompiled_shader("texmap/v_2.vert");
    PbdlShaderBlob* frag = find_precompiled_shader("texmap/f_2.frag");
    if (!vert || !frag || !vert->len || !frag->len) return false;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (!vs || !fs) return false;

    glShaderBinary(1, &vs, 0, vert->code,
                   static_cast<GLsizei>(vert->len));
    glShaderBinary(1, &fs, 0, frag->code,
                   static_cast<GLsizei>(frag->len));

    g.program = glCreateProgram();
    if (!g.program) return false;
    glAttachShader(g.program, vs);
    glAttachShader(g.program, fs);
    glLinkProgram(g.program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(g.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return false;

    g.aVertex = glGetAttribLocation(g.program, "a_vertex");
    g.uModel = glGetUniformLocation(g.program, "u_modelViewMatrix");
    g.uProjection = glGetUniformLocation(g.program, "u_projectionMatrix");
    g.uTexSpace = glGetUniformLocation(g.program, "u_textureSpaceMatrix");
    g.uOpacity = glGetUniformLocation(g.program, "u_opacity");
    g.uSampler = glGetUniformLocation(g.program, "s_sampler");
    if (g.aVertex < 0 || g.uModel < 0 || g.uProjection < 0 ||
        g.uTexSpace < 0 || g.uOpacity < 0 || g.uSampler < 0)
        return false;

    glGenTextures(1, &g.texture);
    if (!g.texture) return false;
    glBindTexture(GL_TEXTURE_2D, g.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const uint32_t black[4] = {
        0xFF000000u,0xFF000000u,0xFF000000u,0xFF000000u
    };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    g.texW = 2;
    g.texH = 2;
    return glGetError() == GL_NO_ERROR;
}

static bool gpu_init(GpuRenderer& g) {
    if (!gpu_load_modules(g)) {
        notify_user("Hybrid GPU: falha carregando Piglet.");
        return false;
    }
    if (!gpu_init_context(g)) {
        notify_user("Hybrid GPU: falha criando contexto EGL.");
        return false;
    }
    if (!gpu_init_program(g)) {
        notify_user("Hybrid GPU: falha preparando shader/textura.");
        return false;
    }
    return true;
}

static void gpu_upload_frame(GpuRenderer& g,
                             const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    glBindTexture(GL_TEXTURE_2D, g.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (w != g.texW || h != g.texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        g.texW = w;
        g.texH = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}

static void gpu_scissor_clear(int x, int y, int w, int h,
                              float r, float g, float b, float a) {
    if (w <= 0 || h <= 0) return;
    int sx = std::max(0, x);
    int syTop = std::max(0, y);
    int ex = std::min(SCREEN_W, x + w);
    int eyTop = std::min(SCREEN_H, y + h);
    if (ex <= sx || eyTop <= syTop) return;

    const int sw = ex - sx;
    const int sh = eyTop - syTop;
    const int sy = SCREEN_H - eyTop;
    glScissor(sx, sy, sw, sh);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void gpu_render(GpuRenderer& g, int cursorX, int cursorY) {
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Clip-space positions + texture coordinates. V is flipped because
    // stb_image's first decoded row is the top row.
    const GLfloat vertices[] = {
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,

        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 1.0f
    };
    const GLfloat identity[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };

    glUseProgram(g.program);
    glUniform1f(g.uOpacity, 1.0f);
    glUniformMatrix4fv(g.uModel, 1, GL_FALSE, identity);
    glUniformMatrix4fv(g.uProjection, 1, GL_FALSE, identity);
    glUniformMatrix4fv(g.uTexSpace, 1, GL_FALSE, identity);
    glVertexAttribPointer(g.aVertex, 4, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), vertices);
    glEnableVertexAttribArray(g.aVertex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.texture);
    glUniform1i(g.uSampler, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(g.aVertex);

    // Hardware cursor: only a few scissored clears, no 1920x1080 CPU copy.
    glEnable(GL_SCISSOR_TEST);
    gpu_scissor_clear(cursorX - 18, cursorY - 4, 37, 9,
                      0.0f, 0.0f, 0.0f, 1.0f);
    gpu_scissor_clear(cursorX - 4, cursorY - 18, 9, 37,
                      0.0f, 0.0f, 0.0f, 1.0f);
    gpu_scissor_clear(cursorX - 15, cursorY - 1, 31, 3,
                      1.0f, 1.0f, 1.0f, 1.0f);
    gpu_scissor_clear(cursorX - 1, cursorY - 15, 3, 31,
                      1.0f, 1.0f, 1.0f, 1.0f);
    gpu_scissor_clear(cursorX - 2, cursorY - 2, 5, 5,
                      1.0f, 0.15f, 0.15f, 1.0f);
    glDisable(GL_SCISSOR_TEST);

    (void)eglSwapBuffers(g.display, g.surface);
}

static bool take_latest_frame(FrameWorker& worker,
                              uint64_t& seenGeneration,
                              std::vector<uint8_t>& pixels,
                              int& w, int& h) {
    bool got = false;
    pthread_mutex_lock(&worker.mutex);
    if (worker.generation != seenGeneration &&
        !worker.rgba.empty()) {
        pixels.swap(worker.rgba);
        w = worker.width;
        h = worker.height;
        seenGeneration = worker.generation;
        got = true;
    }
    pthread_mutex_unlock(&worker.mutex);
    return got;
}

static void gpu_shutdown(GpuRenderer& g) {
    if (g.texture) glDeleteTextures(1, &g.texture);
    if (g.program) glDeleteProgram(g.program);

    if (g.display != EGL_NO_DISPLAY) {
        (void)eglMakeCurrent(
            g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g.context != EGL_NO_CONTEXT)
            (void)eglDestroyContext(g.display, g.context);
        if (g.surface != EGL_NO_SURFACE)
            (void)eglDestroySurface(g.display, g.surface);
        (void)eglTerminate(g.display);
    }

    int mstop = 0;
    if (g.precompiled >= 0)
        (void)sceKernelStopUnloadModule(
            g.precompiled, 0, nullptr, 0, nullptr, &mstop);
    if (g.piglet >= 0)
        (void)sceKernelStopUnloadModule(
            g.piglet, 0, nullptr, 0, nullptr, &mstop);
}

static int open_native_pad() {
    scePadInit();
    sceUserServiceInitialize(nullptr);
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);
    return scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    sceUserServiceInitialize(nullptr);
    mkdir("/data/Downloads", 0777);
    mkdir("/data/PBDL", 0777);
    append_diag("BOOT", "PS4 Hybrid Browser v7.6 GPU screencast");

    const bool daemonReady = start_download_daemon();
    if (!daemonReady) {
        append_diag("DAEMON_WARN", g_status);
        notify_user("Hybrid: daemon indisponivel. Navegacao continua.");
    }

    std::string backend = discover_hybrid_backend();
    if (backend.empty()) {
        notify_user("Hybrid: backend Chromium nao encontrado.");
        sleep(4);
        clean_exit_to_shell();
    }
    while (!backend.empty() && backend.back() == '/') backend.pop_back();

    GpuRenderer gpu;
    if (!gpu_init(gpu)) {
        sleep(4);
        clean_exit_to_shell();
    }

    FrameWorker frameWorker;
    if (!start_frame_worker(frameWorker, backend)) {
        notify_user("Hybrid: falha iniciando stream de video.");
        gpu_shutdown(gpu);
        sleep(4);
        clean_exit_to_shell();
    }

    DownloadWorker downloadWorker;
    const bool downloadWorkerStarted =
        start_download_worker(downloadWorker, backend);

    const int pad = open_native_pad();
    if (pad < 0) {
        frameWorker.stop = true;
        stop_frame_worker(frameWorker);
        if (downloadWorkerStarted) stop_download_worker(downloadWorker);
        gpu_shutdown(gpu);
        notify_user("Hybrid: controle PS4 nao abriu.");
        sleep(4);
        clean_exit_to_shell();
    }

    notify_user("Hybrid v7.6 GPU: X clicar | O voltar | R2 URL | L2 Google | Options sair");

    float cursorX = SCREEN_W * 0.5f;
    float cursorY = SCREEN_H * 0.5f;
    uint32_t oldButtons = 0;
    uint64_t seenGeneration = 0;
    uint64_t lastUs = sceKernelGetProcessTime();

    std::vector<uint8_t> pendingPixels;
    int pendingW = 0;
    int pendingH = 0;

    for (;;) {
        const uint64_t nowUs = sceKernelGetProcessTime();
        float dt = static_cast<float>(nowUs - lastUs) / 1000000.0f;
        lastUs = nowUs;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.025f) dt = 0.025f;

        if (take_latest_frame(frameWorker, seenGeneration,
                              pendingPixels, pendingW, pendingH)) {
            gpu_upload_frame(gpu, pendingPixels.data(),
                             pendingW, pendingH);
            pendingPixels.clear();
        }

        OrbisPadData pd{};
        if (scePadReadState(pad, &pd) >= 0) {
            const uint32_t pressed = pd.buttons & ~oldButtons;
            oldButtons = pd.buttons;

            const float vx = cursor_axis_velocity(pd.leftStick.x);
            const float vy = cursor_axis_velocity(pd.leftStick.y);
            cursorX += vx * dt;
            cursorY += vy * dt;
            if (cursorX < 0.0f) cursorX = 0.0f;
            if (cursorY < 0.0f) cursorY = 0.0f;
            if (cursorX > SCREEN_W - 1) cursorX = SCREEN_W - 1;
            if (cursorY > SCREEN_H - 1) cursorY = SCREEN_H - 1;

            const int cx = static_cast<int>(cursorX + 0.5f);
            const int cy = static_cast<int>(cursorY + 0.5f);
            const int bx = cx * 1280 / SCREEN_W;
            const int by = cy * 720 / SCREEN_H;

            if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                if (backend_api(backend, "/api/click",
                    "{\"x\":" + std::to_string(bx) +
                    ",\"y\":" + std::to_string(by) + "}")) {
                    usleep(35 * 1000);
                    if (backend_focus_is_editable(backend)) {
                        std::string text;
                        if (open_url_ime(text)) {
                            backend_api(backend, "/api/type-submit",
                                "{\"text\":\"" + json_escape(text) + "\"}");
                        }
                    }
                }
            }

            if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) ||
                (pressed & ORBIS_PAD_BUTTON_L1))
                backend_api(backend, "/api/back");

            if (pressed & ORBIS_PAD_BUTTON_R1)
                backend_api(backend, "/api/forward");

            if (pressed & ORBIS_PAD_BUTTON_SQUARE)
                backend_api(backend, "/api/reload");

            if (pressed & ORBIS_PAD_BUTTON_UP)
                backend_api(backend, "/api/scroll", "{\"y\":-500}");

            if (pressed & ORBIS_PAD_BUTTON_DOWN)
                backend_api(backend, "/api/scroll", "{\"y\":500}");

            if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                backend_api(backend, "/api/image-at",
                    "{\"x\":" + std::to_string(bx) +
                    ",\"y\":" + std::to_string(by) + "}");
                notify_user("Hybrid: procurando imagem sob o cursor...");
            }

            if (pressed & ORBIS_PAD_BUTTON_R2) {
                std::string text;
                if (open_url_ime(text)) {
                    backend_api(backend, "/api/nav",
                        "{\"url\":\"" + json_escape(text) + "\"}");
                }
            }

            if (pressed & ORBIS_PAD_BUTTON_R3) {
                std::string text;
                if (open_url_ime(text)) {
                    backend_api(backend, "/api/type",
                        "{\"text\":\"" + json_escape(text) + "\"}");
                }
            }

            if (pressed & ORBIS_PAD_BUTTON_L2) {
                backend_api(backend, "/api/home");
                notify_user("Hybrid: Google/Home.");
            }

            if (pressed & ORBIS_PAD_BUTTON_OPTIONS)
                break;

            gpu_render(gpu, cx, cy);
        } else {
            gpu_render(gpu,
                static_cast<int>(cursorX),
                static_cast<int>(cursorY));
        }

        // eglSwapInterval(1) should pace at display refresh. This tiny sleep
        // prevents a runaway loop on systems where swap interval is ignored.
        usleep(1000);
    }

    if (downloadWorkerStarted) stop_download_worker(downloadWorker);
    stop_frame_worker(frameWorker);
    scePadClose(pad);
    gpu_shutdown(gpu);
    clean_exit_to_shell();
    return 0;
}

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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>

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
    const int tmpl = sceHttpCreateTemplate(g_http, "PS4HybridBrowser/7.2", ORBIS_HTTP_VERSION_1_1, 1);
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

            if (!found.empty() && !backend_healthy(found) && backend_ui_healthy(found)) {
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

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    sceUserServiceInitialize(nullptr);
    mkdir("/data/Downloads", 0777);
    mkdir("/data/PBDL", 0777);
    append_diag("BOOT", "PS4 Hybrid Browser v7.2 legacy UI");

    const bool daemonReady = start_download_daemon();
    if (!daemonReady) {
        append_diag("DAEMON_WARN", g_status);
        notify_user("Hybrid: BinLoader/daemon indisponivel. Arquivos comuns nao terao background.");
    }

    std::string backend = discover_hybrid_backend();
    if (backend.empty()) {
        append_diag("HYBRID_FATAL", "backend nao encontrado");
        notify_user("Hybrid: backend Chromium nao encontrado. Inicie o start_windows.bat e libere a rede privada.");
        sleep(4);
        clean_exit_to_shell();
    }

    while (!backend.empty() && backend.back() == '/') backend.pop_back();
    const std::string uiUrl = backend + "/ps4";
    append_diag("HYBRID_OPEN", uiUrl);
    notify_user("Hybrid v7.2: backend encontrado. Abrindo interface simples.");
    (void)open_browser_and_wait(uiUrl.c_str());

    clean_exit_to_shell();
    return 0;
}

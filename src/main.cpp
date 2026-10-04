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
    const int tmpl = sceHttpCreateTemplate(g_http, "PS4BrowserDownloader/5.0", ORBIS_HTTP_VERSION_1_1, 1);
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

    int filefd = open("/app0/daemon/daemon.elf", O_RDONLY);
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

static bool handle_captured_url(const std::string& captured, const std::string& referer, std::string& reopenUrl) {
    UrlProbe p = probe_url(captured, referer);
    if (!p.ok) {
        // Some protected pages refuse a probe but still render correctly in WebKit.
        reopenUrl = captured;
        return false;
    }

    // Detect PKG by file magic, not only by the extension.
    PkgInfo info;
    if (fetch_pkg_header(p.finalUrl, info)) {
        g_status = "PKG detectado. Enviando ao BGFT...";
        return queue_bgft(p.finalUrl);
    }

    if (probe_is_download(p)) {
        if (queue_generic_background(p)) return true;
        // If the persistent daemon cannot start, give the WebKit a chance rather than crash.
        reopenUrl = p.finalUrl;
        return false;
    }

    reopenUrl = p.finalUrl.empty() ? captured : p.finalUrl;
    return false;
}

static bool open_browser_and_wait(const char* requestedUrl = nullptr) {
    if (!init_browser()) return false;

    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);

    /*
     * Use the PS4's real WebKit/browser service.  This is intentionally NOT
     * NetSurf: modern JavaScript, cookies and the DOM are handled by the
     * console's WebKit stack.  Default mode is tried first because it gives
     * the browser the least restrictive system presentation (new-window,
     * media and site UI behaviour are less constrained than our old custom
     * rectangle).  Custom mode is only a fallback for firmwares where default
     * presentation is unavailable to homebrew.
     */
    static const char* homeUrl = "https://www.google.com/?hl=pt-BR";
    const char* startUrl = (requestedUrl && *requestedUrl) ? requestedUrl : homeUrl;
    static const char* callbackRegexDefault =
        "^(https?://www\\.superpsx\\.com/(ps4-fake-pkgs-game-list/?.*|.*-ps4-fpkg/?.*)|"
        "https?://.*\\.(pkg|zip|7z|rar|iso|bin|chd|cso|pbp|rom|nes|sfc|smc|gba|gbc|gb|n64|z64|nds|3ds|cia|jpg|jpeg|png|gif|webp|bmp|mp4|mkv|avi|mov|mp3|flac|wav|pdf)([?#].*)?|"
        "https?://([^/]+\\.)?(1fichier\\.com|mediafire\\.com|pixeldrain\\.com|vikingfile\\.com|akirabox\\.com)/.*)$";
    static const char* callbackRegexDownloads =
        "^(https?://.*\\.(pkg|zip|7z|rar|iso|bin|chd|cso|pbp|rom|nes|sfc|smc|gba|gbc|gb|n64|z64|nds|3ds|cia|jpg|jpeg|png|gif|webp|bmp|mp4|mkv|avi|mov|mp3|flac|wav|pdf)([?#].*)?|"
        "https?://([^/]+\\.)?(1fichier\\.com|mediafire\\.com|pixeldrain\\.com|vikingfile\\.com|akirabox\\.com)/.*)$";

    const bool openingSuperPsxInternal =
        requestedUrl && std::strstr(requestedUrl, "superpsx.com/") != nullptr;

    BrowserCallbackInitParam cb{};
    cb.size = sizeof(cb);
    cb.type = CALLBACK_TYPE_REGEXP;
    cb.data = openingSuperPsxInternal ? callbackRegexDownloads : callbackRegexDefault;

    BrowserParam p{};
    p.baseParam.size = sizeof(CommonDialogBaseParam);
    p.baseParam.magic = DIALOG_MAGIC + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&p.baseParam));
    p.size = sizeof(p);
    p.mode = BROWSER_MODE_DEFAULT;
    p.userId = user;
    p.url = startUrl;
    p.callbackInitParam = &cb;

    g_status = "Abrindo WebKit do PS4 (modo completo)...";
    int32_t r = g_browser.open(&p);


    if (r != 0) {
        g_status = "Browser WebKit open: " + hex32(r);
        return false;
    }

    g_status = "WebKit v5: downloads interceptados; PKG=BGFT, arquivos=daemon.";

    bool finished = false;
    for (;;) {
        const int32_t st = g_browser.updateStatus();
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
        g_status = "Browser nao chegou a FINISHED";
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

    // IMPORTANT: do not call sceWebBrowserDialogClose() after FINISHED/GetResult.

    if (!captured.empty()) {
        if (!openingSuperPsxInternal &&
            captured.find("https://www.superpsx.com/") == 0) {
            g_status = "Abrindo pagina SuperPSX na mesma janela...";
            return open_browser_and_wait(captured.c_str());
        }

        std::string reopen;
        if (handle_captured_url(captured, startUrl, reopen)) {
            // Download is now owned by BGFT or the persistent daemon.
            // Reopen the page that originated the download so browsing can continue.
            return open_browser_and_wait(startUrl);
        }
        if (!reopen.empty() && reopen != startUrl)
            return open_browser_and_wait(reopen.c_str());
    }

    g_status = "Navegador fechado - v5";
    return true;
}


} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    sceUserServiceInitialize(nullptr);
    mkdir("/data/Downloads", 0777);
    (void)start_download_daemon();

    // Keep the same DEFAULT WebKit path proven on hardware.
    (void)open_browser_and_wait();

    // Do not manually tear down browser/network modules here.
    // Stable PS4 homebrew hands control back through SystemService.
    clean_exit_to_shell();
    return 0;
}

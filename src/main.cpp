#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include <orbis/Pigletv2VSH.h>
#include <orbis/Pad.h>
#include <orbis/SysUtil.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

// Opaque WebKit2 C API types.
using WKTypeRef = const void*;
using WKStringRef = const void*;
using WKURLRef = const void*;
using WKContextRef = void*;
using WKPageGroupRef = void*;
using WKViewRef = void*;
using WKPageRef = void*;
using WKPreferencesRef = void*;

struct WKSize {
    double width;
    double height;
};

// WebKit2 ABI matching the older CoordinatedGraphics API exported by PS4.
using WKContextCreateFn = WKContextRef (*)();
using WKContextSetCacheModelFn = void (*)(WKContextRef, uint32_t);
using WKStringCreateWithUTF8CStringFn = WKStringRef (*)(const char*);
using WKPageGroupCreateWithIdentifierFn = WKPageGroupRef (*)(WKStringRef);
using WKPageGroupGetPreferencesFn = WKPreferencesRef (*)(WKPageGroupRef);
using WKPreferencesSetBoolFn = void (*)(WKPreferencesRef, bool);
using WKViewCreateFn = WKViewRef (*)(WKContextRef, WKPageGroupRef);
using WKViewGetPageFn = WKPageRef (*)(WKViewRef);
using WKViewSetSizeFn = void (*)(WKViewRef, WKSize);
using WKViewSetBoolFn = void (*)(WKViewRef, bool);
using WKViewSyncFn = void (*)(WKViewRef);
using WKViewPaintGLFn = void (*)(WKViewRef);
using WKURLCreateWithUTF8CStringFn = WKURLRef (*)(const char*);
using WKPageLoadURLFn = void (*)(WKPageRef, WKURLRef);
using WKReleaseFn = void (*)(WKTypeRef);

// PS4 Manx run loop exported by libSceOrbisCompat.
using ManxRunLoopCurrentFn = void* (*)();
using ManxRunLoopPollFn = void (*)(void*);

struct WebKitApi {
    int32_t module = -1;
    int32_t compatModule = -1;

    WKContextCreateFn contextCreate = nullptr;
    WKContextSetCacheModelFn contextSetCacheModel = nullptr;
    WKStringCreateWithUTF8CStringFn stringCreate = nullptr;
    WKPageGroupCreateWithIdentifierFn pageGroupCreate = nullptr;
    WKPageGroupGetPreferencesFn pageGroupGetPreferences = nullptr;

    WKPreferencesSetBoolFn prefJs = nullptr;
    WKPreferencesSetBoolFn prefJsWindows = nullptr;
    WKPreferencesSetBoolFn prefLocalStorage = nullptr;
    WKPreferencesSetBoolFn prefCookies = nullptr;
    WKPreferencesSetBoolFn prefImages = nullptr;
    WKPreferencesSetBoolFn prefFullScreen = nullptr;
    WKPreferencesSetBoolFn prefWebGL = nullptr;
    WKPreferencesSetBoolFn prefWebAudio = nullptr;

    WKViewCreateFn viewCreate = nullptr;
    WKViewGetPageFn viewGetPage = nullptr;
    WKViewSetSizeFn viewSetSize = nullptr;
    WKViewSetBoolFn viewSetActive = nullptr;
    WKViewSetBoolFn viewSetFocused = nullptr;
    WKViewSetBoolFn viewSetVisible = nullptr;
    WKViewSetBoolFn viewSetDrawsBackground = nullptr;
    WKViewSyncFn viewSync = nullptr;
    WKViewPaintGLFn viewPaintGL = nullptr;

    WKURLCreateWithUTF8CStringFn urlCreate = nullptr;
    WKPageLoadURLFn pageLoadURL = nullptr;
    WKReleaseFn release = nullptr;

    ManxRunLoopCurrentFn runLoopCurrent = nullptr;
    ManxRunLoopPollFn runLoopPoll = nullptr;
};

WebKitApi g_wk;

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
int32_t g_pigletModule = -1;
int32_t g_precompiledModule = -1;
int32_t g_pad = -1;

static void notify(const char* text) {
    sceSysUtilSendSystemNotificationWithText(222, text);
}

static std::string hex32(int32_t value) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%08X", static_cast<uint32_t>(value));
    return b;
}

static int32_t load_system_module(const char* name) {
    char path[256];
    std::snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    int32_t h = static_cast<int32_t>(
        sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
    if (h >= 0)
        return h;

    const char* word = sceKernelGetFsSandboxRandomWord();
    if (word && *word) {
        std::snprintf(path, sizeof(path), "/%s/common/lib/%s", word, name);
        h = static_cast<int32_t>(
            sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
    }
    return h;
}

template <typename T>
static bool resolve_required(int32_t module, const char* symbol, T& out) {
    void* p = nullptr;
    const int32_t r = sceKernelDlsym(module, symbol, &p);
    if (r < 0 || !p) {
        std::string msg = std::string("v6: faltou simbolo ") + symbol +
                          " (" + hex32(r) + ")";
        notify(msg.c_str());
        return false;
    }
    out = reinterpret_cast<T>(p);
    return true;
}

template <typename T>
static void resolve_optional(int32_t module, const char* symbol, T& out) {
    void* p = nullptr;
    if (sceKernelDlsym(module, symbol, &p) >= 0 && p)
        out = reinterpret_cast<T>(p);
}

static bool init_piglet() {
    const char* word = sceKernelGetFsSandboxRandomWord();
    if (!word || !*word) {
        notify("v6: sandbox word indisponivel");
        return false;
    }

    char path[256];
    std::snprintf(path, sizeof(path), "/%s/common/lib/libScePigletv2VSH.sprx", word);
    g_pigletModule = static_cast<int32_t>(
        sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
    if (g_pigletModule < 0) {
        notify("v6: falha carregando Piglet");
        return false;
    }

    std::snprintf(path, sizeof(path), "/%s/common/lib/libScePrecompiledShaders.sprx", word);
    g_precompiledModule = static_cast<int32_t>(
        sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
    if (g_precompiledModule < 0) {
        notify("v6: falha carregando shaders");
        return false;
    }

    OrbisPglConfig cfg{};
    cfg.size = sizeof(cfg);
    cfg.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT |
                ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    cfg.processOrder = 1;
    cfg.systemSharedMemorySize = 250 * 1024 * 1024;
    cfg.videoSharedMemorySize = 512 * 1024 * 1024;
    cfg.maxMappedFlexibleMemory = 170 * 1024 * 1024;
    cfg.drawCommandBufferSize = 1 * 1024 * 1024;
    cfg.lcueResourceBufferSize = 1 * 1024 * 1024;
    cfg.dbgPosCmd_0x40 = kWidth;
    cfg.dbgPosCmd_0x44 = kHeight;
    cfg.dbgPosCmd_0x48 = 0;
    cfg.dbgPosCmd_0x4C = 0;
    cfg.unk_0x5C = 2;

    if (!scePigletSetConfigurationVSH(&cfg)) {
        notify("v6: scePigletSetConfigurationVSH falhou");
        return false;
    }

    OrbisPglWindow window{0, static_cast<khronos_uint32_t>(kWidth),
                          static_cast<khronos_uint32_t>(kHeight)};

    const EGLint attributes[] = {
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

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    const EGLint windowAttributes[] = {
        EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
        EGL_NONE
    };

    EGLConfig config{};
    EGLint configCount = 0;
    EGLint major = 0;
    EGLint minor = 0;

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        notify("v6: eglGetDisplay falhou");
        return false;
    }
    if (!eglInitialize(g_display, &major, &minor)) {
        notify("v6: eglInitialize falhou");
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        notify("v6: eglBindAPI falhou");
        return false;
    }
    (void)eglSwapInterval(g_display, 0);

    if (!eglChooseConfig(g_display, attributes, &config, 1, &configCount) ||
        configCount < 1) {
        notify("v6: eglChooseConfig falhou");
        return false;
    }

    g_surface = eglCreateWindowSurface(
        g_display, config, &window, windowAttributes);
    if (g_surface == EGL_NO_SURFACE) {
        notify("v6: eglCreateWindowSurface falhou");
        return false;
    }

    g_context = eglCreateContext(
        g_display, config, EGL_NO_CONTEXT, contextAttributes);
    if (g_context == EGL_NO_CONTEXT) {
        notify("v6: eglCreateContext falhou");
        return false;
    }

    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        notify("v6: eglMakeCurrent falhou");
        return false;
    }

    glViewport(0, 0, kWidth, kHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(g_display, g_surface);

    notify("v6: Piglet/EGL OK");
    return true;
}

static bool init_webkit() {
    // Load compatibility/runtime before WebKit2.
    g_wk.compatModule = load_system_module("libSceOrbisCompat.sprx");
    if (g_wk.compatModule < 0) {
        notify("v6: libSceOrbisCompat falhou");
        return false;
    }

    g_wk.module = load_system_module("libSceWebKit2.sprx");
    if (g_wk.module < 0) {
        notify("v6: libSceWebKit2 falhou");
        return false;
    }

    if (!resolve_required(g_wk.module, "WKContextCreate", g_wk.contextCreate) ||
        !resolve_required(g_wk.module, "WKStringCreateWithUTF8CString", g_wk.stringCreate) ||
        !resolve_required(g_wk.module, "WKPageGroupCreateWithIdentifier", g_wk.pageGroupCreate) ||
        !resolve_required(g_wk.module, "WKViewCreate", g_wk.viewCreate) ||
        !resolve_required(g_wk.module, "WKViewGetPage", g_wk.viewGetPage) ||
        !resolve_required(g_wk.module, "WKViewSetSize", g_wk.viewSetSize) ||
        !resolve_required(g_wk.module, "WKViewSetActive", g_wk.viewSetActive) ||
        !resolve_required(g_wk.module, "WKViewSetFocused", g_wk.viewSetFocused) ||
        !resolve_required(g_wk.module, "WKViewSetIsVisible", g_wk.viewSetVisible) ||
        !resolve_required(g_wk.module, "WKViewSyncCoordinatedGraphicsState", g_wk.viewSync) ||
        !resolve_required(g_wk.module, "WKViewPaintToCurrentGLContext", g_wk.viewPaintGL) ||
        !resolve_required(g_wk.module, "WKURLCreateWithUTF8CString", g_wk.urlCreate) ||
        !resolve_required(g_wk.module, "WKPageLoadURL", g_wk.pageLoadURL) ||
        !resolve_required(g_wk.module, "WKRelease", g_wk.release)) {
        return false;
    }

    resolve_optional(g_wk.module, "WKContextSetCacheModel", g_wk.contextSetCacheModel);
    resolve_optional(g_wk.module, "WKPageGroupGetPreferences", g_wk.pageGroupGetPreferences);
    resolve_optional(g_wk.module, "WKPreferencesSetJavaScriptEnabled", g_wk.prefJs);
    resolve_optional(g_wk.module, "WKPreferencesSetJavaScriptCanOpenWindowsAutomatically", g_wk.prefJsWindows);
    resolve_optional(g_wk.module, "WKPreferencesSetLocalStorageEnabled", g_wk.prefLocalStorage);
    resolve_optional(g_wk.module, "WKPreferencesSetCookieEnabled", g_wk.prefCookies);
    resolve_optional(g_wk.module, "WKPreferencesSetLoadsImagesAutomatically", g_wk.prefImages);
    resolve_optional(g_wk.module, "WKPreferencesSetFullScreenEnabled", g_wk.prefFullScreen);
    resolve_optional(g_wk.module, "WKPreferencesSetWebGLEnabled", g_wk.prefWebGL);
    resolve_optional(g_wk.module, "WKPreferencesSetWebAudioEnabled", g_wk.prefWebAudio);
    resolve_optional(g_wk.module, "WKViewSetDrawsBackground", g_wk.viewSetDrawsBackground);

    // Manx run loop lives in the compatibility layer.
    resolve_optional(g_wk.compatModule, "_ZN4Manx7RunLoop7currentEv", g_wk.runLoopCurrent);
    resolve_optional(g_wk.compatModule, "_ZN4Manx7RunLoop4pollEv", g_wk.runLoopPoll);
    if (!g_wk.runLoopCurrent || !g_wk.runLoopPoll) {
        // Some firmware builds re-export these from WebKit2.
        resolve_optional(g_wk.module, "_ZN4Manx7RunLoop7currentEv", g_wk.runLoopCurrent);
        resolve_optional(g_wk.module, "_ZN4Manx7RunLoop4pollEv", g_wk.runLoopPoll);
    }

    notify("v6: WebKit2 simbolos OK");
    return true;
}

static void clean_exit() {
    const int32_t h = load_system_module("libSceSystemService.sprx");
    if (h >= 0) {
        using LoadExecFn = int32_t (*)(const char*, const char**);
        LoadExecFn loadExec = nullptr;
        if (sceKernelDlsym(h, "sceSystemServiceLoadExec",
                           reinterpret_cast<void**>(&loadExec)) >= 0 && loadExec) {
            loadExec("exit", nullptr);
            for (;;)
                usleep(1000 * 1000);
        }
    }
    _Exit(0);
}

static int open_pad() {
    scePadInit();
    sceUserServiceInitialize(nullptr);
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);
    return scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Piglet sample requires internal system service to be present.
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    (void)sceSystemServiceHideSplashScreen();

    notify("v6.0: iniciando WebKit2 nativo");

    if (!init_piglet()) {
        sleep(8);
        clean_exit();
    }

    if (!init_webkit()) {
        sleep(8);
        clean_exit();
    }

    WKContextRef context = g_wk.contextCreate();
    if (!context) {
        notify("v6: WKContextCreate retornou null");
        sleep(8);
        clean_exit();
    }
    notify("v6: WKContext OK");

    if (g_wk.contextSetCacheModel)
        g_wk.contextSetCacheModel(context, 2); // primary web browser

    WKStringRef identifier = g_wk.stringCreate("PBDL.WebKit2.V6");
    if (!identifier) {
        notify("v6: WKString identifier falhou");
        sleep(8);
        clean_exit();
    }

    WKPageGroupRef group = g_wk.pageGroupCreate(identifier);
    g_wk.release(identifier);
    if (!group) {
        notify("v6: WKPageGroup falhou");
        sleep(8);
        clean_exit();
    }

    if (g_wk.pageGroupGetPreferences) {
        WKPreferencesRef prefs = g_wk.pageGroupGetPreferences(group);
        if (prefs) {
            if (g_wk.prefJs) g_wk.prefJs(prefs, true);
            if (g_wk.prefJsWindows) g_wk.prefJsWindows(prefs, true);
            if (g_wk.prefLocalStorage) g_wk.prefLocalStorage(prefs, true);
            if (g_wk.prefCookies) g_wk.prefCookies(prefs, true);
            if (g_wk.prefImages) g_wk.prefImages(prefs, true);
            if (g_wk.prefFullScreen) g_wk.prefFullScreen(prefs, true);
            if (g_wk.prefWebGL) g_wk.prefWebGL(prefs, true);
            if (g_wk.prefWebAudio) g_wk.prefWebAudio(prefs, true);
        }
    }

    WKViewRef view = g_wk.viewCreate(context, group);
    if (!view) {
        notify("v6: WKViewCreate retornou null");
        sleep(8);
        clean_exit();
    }
    notify("v6: WKView OK");

    g_wk.viewSetSize(view, WKSize{static_cast<double>(kWidth),
                                  static_cast<double>(kHeight)});
    g_wk.viewSetActive(view, true);
    g_wk.viewSetFocused(view, true);
    g_wk.viewSetVisible(view, true);
    if (g_wk.viewSetDrawsBackground)
        g_wk.viewSetDrawsBackground(view, true);

    WKPageRef page = g_wk.viewGetPage(view);
    if (!page) {
        notify("v6: WKViewGetPage retornou null");
        sleep(8);
        clean_exit();
    }
    notify("v6: WKPage OK");

    WKURLRef google = g_wk.urlCreate("https://www.google.com/?hl=pt-BR");
    if (!google) {
        notify("v6: WKURLCreate falhou");
        sleep(8);
        clean_exit();
    }

    g_wk.pageLoadURL(page, google);
    g_wk.release(google);
    notify("v6: Google solicitado");

    g_pad = open_pad();
    uint32_t oldButtons = 0;

    for (;;) {
        if (g_wk.runLoopCurrent && g_wk.runLoopPoll) {
            void* loop = g_wk.runLoopCurrent();
            if (loop)
                g_wk.runLoopPoll(loop);
        }

        if (g_pad >= 0) {
            OrbisPadData pad{};
            if (scePadReadState(g_pad, &pad) >= 0) {
                const uint32_t pressed = pad.buttons & ~oldButtons;
                oldButtons = pad.buttons;

                // Circle is only an emergency clean exit in v6.0.
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
                    clean_exit();
            }
        }

        glViewport(0, 0, kWidth, kHeight);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        g_wk.viewSync(view);
        g_wk.viewPaintGL(view);

        if (!eglSwapBuffers(g_display, g_surface)) {
            notify("v6: eglSwapBuffers falhou");
            sleep(5);
            clean_exit();
        }

        usleep(16 * 1000);
    }

    return 0;
}

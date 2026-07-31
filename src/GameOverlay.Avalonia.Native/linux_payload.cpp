// linux_payload.cpp - the Linux injected payload entry.
//
// Injection on Linux is LD_PRELOAD, so there is no DllMain and no MinHook: we
// simply export glXSwapBuffers with default visibility, which the dynamic
// loader binds ahead of the real libGL symbol. Each call composites the overlay
// into the back buffer and then forwards to the real glXSwapBuffers via
// dlsym(RTLD_NEXT, ...). This is the Linux counterpart of dllmain.cpp +
// opengl_hooks.cpp, but far smaller because interposition needs no trampolines.
//
// Display-only for this first slice: it publishes the swapchain and composites
// host frames, but does not yet capture input (that is the X11 input milestone).

#include "host_liveness.h"
#include "linux_input.h"
#include "log.h"
#include "opengl_glx_renderer.h"
#include "platform/platform.h"
#include "shared_state.h"

#include <cstring>
#include <dlfcn.h>

#include <GL/glx.h>
#include <X11/Xlib.h>
#include <EGL/egl.h>   // types only; entry points are resolved via dlsym

namespace overlay {
namespace {

platform::SharedMapping g_mapping;
SharedBlock*            g_block = nullptr;
OpenGLGlxRenderer       g_renderer;

uint32_t g_generation = 0;
unsigned g_lastWidth = 0;
unsigned g_lastHeight = 0;
bool         g_initTried = false;
bool         g_capturing = false;
HostLiveness g_hostLive;
uint64_t     g_eglWindow = 0;   // X11 Window from eglCreateWindowSurface

void EnsureInit() {
    if (g_initTried) return;
    g_initTried = true;

    const uint32_t pid = platform::CurrentProcessId();
    g_mapping = platform::MapSharedBlock(pid, sizeof(SharedBlock));
    if (!g_mapping.base) {
        OVERLAY_LOG("linux payload: shared block map failed for pid %u", pid);
        return;
    }
    g_block = static_cast<SharedBlock*>(g_mapping.base);

    g_block->state.gamePid = pid;
    g_block->state.graphicsApi = kGraphicsApiOpenGL;
    // abiVersion + dllAttached last: the host treats them as the "payload live"
    // signal, so everything else must already be in place.
    g_block->state.abiVersion = kAbiVersion;
    g_block->state.dllAttached = 1;

    OVERLAY_LOG("linux payload attached to pid %u (GLX)", pid);
}

void OnSwap(Display* dpy, GLXDrawable drawable) {
    EnsureInit();
    if (!g_block) return;

    SharedState* state = &g_block->state;

    // The drawable is an X window/pixmap; XGetGeometry gives its pixel size
    // without needing a GLX config. Bump the generation on a size change so the
    // host resizes its overlay surface, exactly like a swapchain resize.
    Window root;
    int x = 0, y = 0;
    unsigned width = 0, height = 0, border = 0, depth = 0;
    if (XGetGeometry(dpy, drawable, &root, &x, &y, &width, &height, &border, &depth)
        && width > 0 && height > 0) {
        if (width != g_lastWidth || height != g_lastHeight) {
            g_lastWidth = width;
            g_lastHeight = height;
            state->gameWidth = width;
            state->gameHeight = height;
            state->gameHwnd = static_cast<uint64_t>(drawable);
            state->swapchainGeneration = ++g_generation;
            OVERLAY_LOG("swapchain published: %ux%u drawable=0x%lx gen=%u",
                        width, height, static_cast<unsigned long>(drawable), g_generation);
        }
    }

    state->presentCount++;

    // A dead host must not leave the overlay frozen or the game deaf.
    if (!g_hostLive.Alive(state)) {
        if (g_capturing) { LeaveLinuxCapture(); g_capturing = false; }
        OVERLAY_LOG_ONCE("host is not responding; overlay disabled");
        return;
    }

    // Follow the host's capture toggle: grab input away from the game when the
    // overlay becomes interactive, release it when it stops.
    bool wantCapture = state->inputCapture != 0;
    if (wantCapture && !g_capturing) {
        EnterLinuxCapture(state->gameHwnd);
        g_capturing = true;
    } else if (!wantCapture && g_capturing) {
        LeaveLinuxCapture();
        g_capturing = false;
    }

    g_renderer.Render(state, static_cast<int>(g_lastWidth), static_cast<int>(g_lastHeight));
}

// EGL present path (desktop-GL contexts). The GL renderer is API-agnostic, so it
// composites the same way; only the present entry point and the way the surface
// size and window are discovered differ from GLX.
void OnSwapEgl(EGLDisplay dpy, EGLSurface surface) {
    EnsureInit();
    if (!g_block) return;
    SharedState* state = &g_block->state;
    state->presentCount++;

    if (!g_hostLive.Alive(state)) {
        if (g_capturing) { LeaveLinuxCapture(); g_capturing = false; }
        OVERLAY_LOG_ONCE("host is not responding; overlay disabled");
        return;
    }

    static auto querySurface = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*)>(
        dlsym(RTLD_NEXT, "eglQuerySurface"));
    EGLint w = 0, h = 0;
    if (querySurface) {
        querySurface(dpy, surface, EGL_WIDTH, &w);
        querySurface(dpy, surface, EGL_HEIGHT, &h);
    }
    if (w > 0 && h > 0 && (unsigned(w) != g_lastWidth || unsigned(h) != g_lastHeight)) {
        g_lastWidth = w;
        g_lastHeight = h;
        state->gameWidth = w;
        state->gameHeight = h;
        state->gameHwnd = g_eglWindow;
        state->swapchainGeneration = ++g_generation;
        OVERLAY_LOG("egl swapchain published: %ux%u gen=%u", w, h, g_generation);
    }

    if (g_eglWindow != 0) {
        state->gameHwnd = g_eglWindow;
        bool want = state->inputCapture != 0;
        if (want && !g_capturing) { EnterLinuxCapture(g_eglWindow); g_capturing = true; }
        else if (!want && g_capturing) { LeaveLinuxCapture(); g_capturing = false; }
    }

    g_renderer.Render(state, static_cast<int>(g_lastWidth), static_cast<int>(g_lastHeight));
}

}  // namespace

// The input module (linux_input.cpp) reaches the shared block through these,
// mirroring the accessors dllmain.cpp provides on Windows.
SharedState* GetSharedState() { return g_block ? &g_block->state : nullptr; }
InputRing*   GetInputRing()   { return g_block ? &g_block->input : nullptr; }

}  // namespace overlay

// The interposer. Default visibility so the loader binds it ahead of libGL's.
extern "C" __attribute__((visibility("default")))
void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    using Fn = void (*)(Display*, GLXDrawable);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "glXSwapBuffers"));

    overlay::OnSwap(dpy, drawable);   // composite into the back buffer first
    if (real) real(dpy, drawable);    // then present
}

// Some games (GLEW/GLAD-style loaders) resolve glXSwapBuffers through
// glXGetProcAddress rather than linking it directly, which would bypass the
// interposer above. Interpose the resolver too and hand back our own
// glXSwapBuffers for that name; everything else forwards to the real resolver.
namespace {
using GLFuncPtr = void (*)();
using GetProcFn = GLFuncPtr (*)(const GLubyte*);

GLFuncPtr ResolveProc(const char* forwardSymbol, const GLubyte* name) {
    if (name && std::strcmp(reinterpret_cast<const char*>(name), "glXSwapBuffers") == 0)
        return reinterpret_cast<GLFuncPtr>(&glXSwapBuffers);
    auto real = reinterpret_cast<GetProcFn>(dlsym(RTLD_NEXT, forwardSymbol));
    return real ? real(name) : nullptr;
}
}  // namespace

extern "C" __attribute__((visibility("default")))
GLFuncPtr glXGetProcAddressARB(const GLubyte* name) {
    return ResolveProc("glXGetProcAddressARB", name);
}

extern "C" __attribute__((visibility("default")))
GLFuncPtr glXGetProcAddress(const GLubyte* name) {
    return ResolveProc("glXGetProcAddress", name);
}

// --- EGL present path ------------------------------------------------------

extern "C" __attribute__((visibility("default")))
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    using Fn = EGLBoolean (*)(EGLDisplay, EGLSurface);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "eglSwapBuffers"));

    overlay::OnSwapEgl(dpy, surface);
    return real ? real(dpy, surface) : EGL_FALSE;
}

extern "C" __attribute__((visibility("default")))
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win, const EGLint* attribs) {
    using Fn = EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "eglCreateWindowSurface"));

    overlay::g_eglWindow = static_cast<uint64_t>(win);   // X11 Window for input capture
    return real ? real(dpy, config, win, attribs) : EGL_NO_SURFACE;
}

extern "C" __attribute__((visibility("default")))
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    if (name && std::strcmp(name, "eglSwapBuffers") == 0)
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglSwapBuffers);
    using Fn = __eglMustCastToProperFunctionPointerType (*)(const char*);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "eglGetProcAddress"));
    return real ? real(name) : nullptr;
}

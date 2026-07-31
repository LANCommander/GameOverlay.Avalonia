// sample_glx_game.cpp - a minimal GLX/X11 "game" used as a Linux injection
// target, the counterpart of SampleGameOpenGL on Windows. It clears the screen
// and presents in a loop; the payload (LD_PRELOADed) composites the overlay
// over it.
//
// Flags:
//   --core            create a 3.3 core-profile context (exercises the payload's
//                     shader draw path instead of fixed-function)
//   --capture <ppm>   single-buffered render + read back the final framebuffer,
//                     count pixels differing from the clear colour, write a PPM
//   <n>               number of frames
//
// Build: g++ sample_glx_game.cpp -o sample_glx_game -lGL -lX11

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

namespace {
constexpr int kW = 800;
constexpr int kH = 600;
constexpr int kClearR = 26, kClearG = 26, kClearB = 38;   // (0.10, 0.10, 0.15)

// glXCreateContextAttribsARB is declared via glxext.h (included by glx.h); the
// GLX_CONTEXT_* attribute constants come from there too.
using CreateCtxAttribs = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
}

int main(int argc, char** argv) {
    int frames = 600;
    const char* capturePath = nullptr;
    bool core = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capturePath = argv[++i];
        else if (std::strcmp(argv[i], "--core") == 0) core = true;
        else frames = std::atoi(argv[i]);
    }
    const bool capture = capturePath != nullptr;

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::fprintf(stderr, "XOpenDisplay failed\n"); return 2; }

    XVisualInfo* vi = nullptr;
    GLXFBConfig fbc{};
    if (core) {
        int fb[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                     GLX_DOUBLEBUFFER, capture ? False : True,
                     GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
                     GLX_DEPTH_SIZE, 24, None };
        int n = 0;
        GLXFBConfig* cfgs = glXChooseFBConfig(dpy, DefaultScreen(dpy), fb, &n);
        if (!cfgs || n == 0) { std::fprintf(stderr, "glXChooseFBConfig failed\n"); return 2; }
        fbc = cfgs[0];
        vi = glXGetVisualFromFBConfig(dpy, fbc);
        XFree(cfgs);
    } else {
        int dbl[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                      GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 24, None };
        int sgl[] = { GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                      GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 24, None };
        vi = glXChooseVisual(dpy, DefaultScreen(dpy), capture ? sgl : dbl);
    }
    if (!vi) { std::fprintf(stderr, "no visual\n"); return 2; }

    Window root = RootWindow(dpy, vi->screen);
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    Window win = XCreateWindow(dpy, root, 0, 0, kW, kH, 0, vi->depth, InputOutput,
                               vi->visual, CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, "SampleGLXGame");
    XMapWindow(dpy, win);

    GLXContext ctx;
    if (core) {
        auto createCtx = reinterpret_cast<CreateCtxAttribs>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
        if (!createCtx) { std::fprintf(stderr, "no glXCreateContextAttribsARB\n"); return 2; }
        int ca[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                     GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None };
        ctx = createCtx(dpy, fbc, nullptr, True, ca);
    } else {
        ctx = glXCreateContext(dpy, vi, nullptr, GL_TRUE);
    }
    if (!ctx) { std::fprintf(stderr, "context creation failed\n"); return 2; }
    glXMakeCurrent(dpy, win, ctx);

    std::printf("sample glx game pid %d, %d frames%s%s\n", static_cast<int>(getpid()),
                frames, core ? " (core 3.3)" : "", capture ? " (capture)" : "");
    std::fflush(stdout);

    for (int i = 0; i < frames; i++) {
        glViewport(0, 0, kW, kH);
        glClearColor(0.10f, 0.10f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glXSwapBuffers(dpy, win);
        usleep(10000);
    }

    if (capture) {
        glFinish();
        glReadBuffer(GL_FRONT);
        std::vector<unsigned char> px(static_cast<size_t>(kW) * kH * 4);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

        long covered = 0;
        for (size_t i = 0; i < px.size(); i += 4) {
            int d = std::abs(px[i] - kClearR) + std::abs(px[i + 1] - kClearG) + std::abs(px[i + 2] - kClearB);
            if (d > 40) covered++;
        }
        std::printf("CAPTURE: coveredPixels=%ld total=%d\n", covered, kW * kH);

        if (FILE* f = std::fopen(capturePath, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
            for (int y = kH - 1; y >= 0; y--)
                for (int x = 0; x < kW; x++) {
                    size_t p = (static_cast<size_t>(y) * kW + x) * 4;
                    std::fputc(px[p], f); std::fputc(px[p + 1], f); std::fputc(px[p + 2], f);
                }
            std::fclose(f);
            std::printf("CAPTURE: wrote %s\n", capturePath);
        }
    }
    return 0;
}

// sample_egl_game.cpp - a minimal EGL/X11 "game" (desktop GL context) used to
// exercise the payload's eglSwapBuffers interposition. Clears + presents in a
// loop; the payload composites the overlay over it.
//
// Build: g++ sample_egl_game.cpp -o sample_egl_game -lEGL -lGL -lX11

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <EGL/egl.h>
#include <GL/gl.h>
#include <X11/Xlib.h>

int main(int argc, char** argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 600;

    Display* xd = XOpenDisplay(nullptr);
    if (!xd) { std::fprintf(stderr, "XOpenDisplay failed\n"); return 2; }
    Window win = XCreateSimpleWindow(xd, DefaultRootWindow(xd), 0, 0, 800, 600, 0, 0, 0);
    XStoreName(xd, win, "SampleEglGame");
    XMapWindow(xd, win);

    EGLDisplay egl = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(xd));
    if (!eglInitialize(egl, nullptr, nullptr)) { std::fprintf(stderr, "eglInitialize failed\n"); return 2; }
    eglBindAPI(EGL_OPENGL_API);   // desktop GL

    EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint nc = 0;
    if (!eglChooseConfig(egl, cfgAttr, &cfg, 1, &nc) || nc == 0) { std::fprintf(stderr, "eglChooseConfig failed\n"); return 2; }

    EGLSurface surf = eglCreateWindowSurface(egl, cfg, win, nullptr);
    if (surf == EGL_NO_SURFACE) { std::fprintf(stderr, "eglCreateWindowSurface failed\n"); return 2; }

    EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx = eglCreateContext(egl, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) { std::fprintf(stderr, "eglCreateContext failed\n"); return 2; }
    eglMakeCurrent(egl, surf, surf, ctx);

    std::printf("sample egl game pid %d, %d frames\n", static_cast<int>(getpid()), frames);
    std::fflush(stdout);

    for (int i = 0; i < frames; i++) {
        glViewport(0, 0, 800, 600);
        glClearColor(0.10f, 0.10f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(egl, surf);   // payload composites here
        usleep(10000);
    }
    return 0;
}

// glx_overlay_test.cpp - runtime verification for the Linux GLX payload.
//
// Plays both sides of the CPU frame transport in one process so the payload can
// be exercised end-to-end without the full managed host:
//   * as the "host", it maps the shared control block, publishes overlay size
//     and a known premultiplied-BGRA test pattern (opaque red) into the frame
//     shm, exactly as CpuFrameProducer would;
//   * as the "game", it creates a GLX context, clears to blue, and calls
//     glXSwapBuffers - which the LD_PRELOADed payload interposes to composite
//     the red pattern over the blue clear.
//
// It then reads the framebuffer centre back: red means the overlay composited,
// blue means it did not. Build: g++ glx_overlay_test.cpp -o t -lGL -lX11 -lrt

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// Include the ABI header before any X11 header: X11 #defines None/Bool/etc.,
// which would clobber the enum members in shared_state.h.
#include "../../src/GameOverlay.Avalonia.Native/shared_state.h"

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

using namespace overlay;

static constexpr int W = 320;
static constexpr int H = 240;

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::fprintf(stderr, "XOpenDisplay failed (is DISPLAY set / Xvfb up?)\n"); return 2; }

    // Single-buffered: draws land in the front buffer, so glReadPixels reads
    // back reliably under Xvfb/llvmpipe (double-buffered front reads do not).
    int attribs[] = { GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                      GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24, None };
    XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), attribs);
    if (!vi) { std::fprintf(stderr, "glXChooseVisual failed\n"); return 2; }

    Window root = RootWindow(dpy, vi->screen);
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    Window win = XCreateWindow(dpy, root, 0, 0, W, H, 0, vi->depth, InputOutput,
                               vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(dpy, win);

    GLXContext ctx = glXCreateContext(dpy, vi, nullptr, GL_TRUE);
    if (!ctx) { std::fprintf(stderr, "glXCreateContext failed\n"); return 2; }
    glXMakeCurrent(dpy, win, ctx);

    // --- act as the host: publish the control block + a red test pattern -----
    const uint32_t pid = static_cast<uint32_t>(getpid());
    char stateName[64];
    std::snprintf(stateName, sizeof(stateName), "/AvaloniaOverlay.State.%u", pid);
    int sfd = shm_open(stateName, O_CREAT | O_RDWR, 0600);
    if (sfd < 0) { std::perror("shm_open state"); return 2; }
    ftruncate(sfd, sizeof(SharedBlock));
    auto* block = static_cast<SharedBlock*>(
        mmap(nullptr, sizeof(SharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0));
    std::memset(block, 0, sizeof(SharedBlock));

    const uint32_t gen = 1;
    char frameName[80];
    std::snprintf(frameName, sizeof(frameName), "/AvaloniaOverlay.Frame.%u.%u", pid, gen);
    const size_t frameBytes = (size_t)W * H * 4;
    int ffd = shm_open(frameName, O_CREAT | O_RDWR, 0600);
    if (ffd < 0) { std::perror("shm_open frame"); return 2; }
    ftruncate(ffd, frameBytes);
    auto* px = static_cast<uint8_t*>(
        mmap(nullptr, frameBytes, PROT_READ | PROT_WRITE, MAP_SHARED, ffd, 0));
    for (size_t i = 0; i < frameBytes; i += 4) {
        px[i + 0] = 0;    // B
        px[i + 1] = 0;    // G
        px[i + 2] = 255;  // R
        px[i + 3] = 255;  // A (premultiplied: opaque red)
    }

    // Host-written fields the renderer reads.
    block->state.visible = 1;
    block->state.texWidth = W;
    block->state.texHeight = H;
    block->state.cpuFrameGeneration = gen;
    block->state.cpuFrameSeq = 2;   // even = a whole frame is readable

    // --- act as the game: clear blue and present a few times -----------------
    for (int i = 0; i < 5; i++) {
        glViewport(0, 0, W, H);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);   // blue
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glXSwapBuffers(dpy, win);                // payload composites here
        XSync(dpy, False);
    }

    // Read the composited centre from the front buffer.
    glFinish();
    glReadBuffer(GL_FRONT);
    unsigned char rgba[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    std::printf("centre pixel RGBA = %u,%u,%u,%u | payload drawCount=%u presentCount=%u attached=%u\n",
                rgba[0], rgba[1], rgba[2], rgba[3],
                block->state.drawCount, block->state.presentCount, block->state.dllAttached);

    bool overlayComposited = rgba[0] > 128 && rgba[2] < 128;   // red, not blue
    bool payloadRan = block->state.dllAttached == 1 && block->state.drawCount > 0;

    std::printf("%s\n", (overlayComposited && payloadRan) ? "RESULT: PASS" : "RESULT: FAIL");

    shm_unlink(stateName);
    shm_unlink(frameName);
    return (overlayComposited && payloadRan) ? 0 : 1;
}

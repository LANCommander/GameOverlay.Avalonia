// xtest_inject.cpp - fires synthetic keyboard/mouse input via the XTEST
// extension, used to verify the payload's input grab captures events under
// Xvfb. XTEST events follow normal X input routing, so when the payload has
// grabbed the keyboard/pointer they are delivered to it, not the game.
//
// Build: g++ xtest_inject.cpp -o xtest_inject -lXtst -lX11

#include <cstdio>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::fprintf(stderr, "XOpenDisplay failed\n"); return 2; }

    KeyCode a = XKeysymToKeycode(dpy, XK_a);
    KeyCode b = XKeysymToKeycode(dpy, XK_b);

    for (int i = 0; i < 10; i++) {
        XTestFakeKeyEvent(dpy, a, True, CurrentTime);
        XTestFakeKeyEvent(dpy, a, False, CurrentTime);
        XTestFakeKeyEvent(dpy, b, True, CurrentTime);
        XTestFakeKeyEvent(dpy, b, False, CurrentTime);
        XTestFakeMotionEvent(dpy, -1, 100 + i * 5, 100 + i * 3, CurrentTime);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XFlush(dpy);
        usleep(30000);
    }

    std::printf("xtest_inject: fired 10 key/mouse bursts\n");
    XCloseDisplay(dpy);
    return 0;
}

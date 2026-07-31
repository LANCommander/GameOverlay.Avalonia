// xtest_hotkey.cpp - fires Shift+F1 once via XTEST, to verify the payload/host
// global hotkey grab toggles the overlay.
//
// Build: g++ xtest_hotkey.cpp -o xtest_hotkey -lXtst -lX11

#include <cstdio>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

int main(int argc, char** argv) {
    // Key name is an X keysym string (default "F1"), e.g. "F1", "F2", "o".
    const char* keyName = argc > 1 ? argv[1] : "F1";

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::fprintf(stderr, "XOpenDisplay failed\n"); return 2; }

    KeyCode shift = XKeysymToKeycode(dpy, XK_Shift_L);
    KeyCode key = XKeysymToKeycode(dpy, XStringToKeysym(keyName));

    usleep(200000);
    XTestFakeKeyEvent(dpy, shift, True, CurrentTime);  XFlush(dpy); usleep(20000);
    XTestFakeKeyEvent(dpy, key, True, CurrentTime);    XFlush(dpy); usleep(20000);
    XTestFakeKeyEvent(dpy, key, False, CurrentTime);   XFlush(dpy); usleep(20000);
    XTestFakeKeyEvent(dpy, shift, False, CurrentTime); XFlush(dpy);

    std::printf("xtest_hotkey: fired Shift+%s\n", keyName);
    XCloseDisplay(dpy);
    return 0;
}

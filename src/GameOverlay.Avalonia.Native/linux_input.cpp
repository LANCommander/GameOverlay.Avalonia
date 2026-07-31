#include "linux_input.h"

#include "log.h"
#include "shared_state.h"

#include <atomic>
#include <thread>

#include <sys/select.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace overlay {
namespace {

// All Xlib access to g_dpy happens on the input thread. The render thread only
// ever sets the atomics below, so no Xlib locking is needed.
Display*              g_dpy = nullptr;
std::thread           g_thread;
std::atomic<bool>     g_running{false};
std::atomic<bool>     g_wantCapture{false};
std::atomic<uint64_t> g_gameWindow{0};
bool                  g_grabbed = false;   // input-thread only

uint32_t MapModifiers(unsigned int state) {
    uint32_t m = 0;
    if (state & ShiftMask)   m |= kModShift;
    if (state & ControlMask) m |= kModControl;
    if (state & Mod1Mask)    m |= kModAlt;
    return m;
}

void Push(const InputEvent& e) {
    if (SharedState* st = GetSharedState()) st->inputSeenCount++;
    PushInputEvent(GetInputRing(), e);
}

void Translate(XEvent& ev) {
    switch (ev.type) {
    case KeyPress:
    case KeyRelease: {
        InputEvent e{};
        e.type = static_cast<uint32_t>(ev.type == KeyPress ? InputEventType::KeyDown
                                                           : InputEventType::KeyUp);
        // The keysym is the platform key code; the host's LinuxKeyMapper turns
        // it into an Avalonia Key.
        e.data = static_cast<uint32_t>(XLookupKeysym(&ev.xkey, 0));
        e.modifiers = MapModifiers(ev.xkey.state);
        Push(e);

        if (ev.type == KeyPress) {
            char buf[16];
            KeySym ignored;
            int n = XLookupString(&ev.xkey, buf, sizeof(buf), &ignored, nullptr);
            for (int i = 0; i < n; i++) {
                unsigned char c = static_cast<unsigned char>(buf[i]);
                if (c >= 0x20 && c != 0x7f) {
                    InputEvent ce{};
                    ce.type = static_cast<uint32_t>(InputEventType::Char);
                    ce.data = c;   // ASCII/Latin-1 as a UTF-16 code unit
                    ce.modifiers = e.modifiers;
                    Push(ce);
                }
            }
        }
        break;
    }
    case ButtonPress:
    case ButtonRelease: {
        InputEvent e{};
        e.x = ev.xbutton.x;
        e.y = ev.xbutton.y;
        e.modifiers = MapModifiers(ev.xbutton.state);
        unsigned int b = ev.xbutton.button;

        if (b == Button4 || b == Button5) {          // vertical wheel
            if (ev.type == ButtonPress) {
                e.type = static_cast<uint32_t>(InputEventType::MouseWheel);
                e.data = static_cast<uint32_t>(b == Button4 ? 120 : -120);
                Push(e);
            }
            break;
        }
        if (b == 6 || b == 7) {                        // horizontal wheel
            if (ev.type == ButtonPress) {
                e.type = static_cast<uint32_t>(InputEventType::MouseHWheel);
                e.data = static_cast<uint32_t>(b == 6 ? -120 : 120);
                Push(e);
            }
            break;
        }

        MouseButton mb = b == Button2 ? MouseButton::Middle
                       : b == Button3 ? MouseButton::Right
                                      : MouseButton::Left;
        e.type = static_cast<uint32_t>(ev.type == ButtonPress ? InputEventType::MouseDown
                                                             : InputEventType::MouseUp);
        e.data = static_cast<uint32_t>(mb);
        Push(e);
        break;
    }
    case MotionNotify: {
        InputEvent e{};
        e.type = static_cast<uint32_t>(InputEventType::MouseMove);
        e.x = ev.xmotion.x;
        e.y = ev.xmotion.y;
        e.modifiers = MapModifiers(ev.xmotion.state);
        Push(e);
        break;
    }
    default:
        break;
    }
}

void ReconcileGrab() {
    bool want = g_wantCapture.load(std::memory_order_relaxed);
    if (want && !g_grabbed) {
        Window w = static_cast<Window>(g_gameWindow.load(std::memory_order_relaxed));
        if (w == 0) return;
        XGrabKeyboard(g_dpy, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        XGrabPointer(g_dpy, w, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        XFlush(g_dpy);
        g_grabbed = true;
        OVERLAY_LOG("linux input: grabbed keyboard+pointer on window 0x%lx",
                    static_cast<unsigned long>(w));
    } else if (!want && g_grabbed) {
        XUngrabKeyboard(g_dpy, CurrentTime);
        XUngrabPointer(g_dpy, CurrentTime);
        XFlush(g_dpy);
        g_grabbed = false;
        OVERLAY_LOG("linux input: released grab");
    }
}

void ThreadMain() {
    int fd = ConnectionNumber(g_dpy);
    while (g_running.load(std::memory_order_relaxed)) {
        ReconcileGrab();

        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            Translate(ev);
        }

        // Sleep until the connection has data or the timeout elapses, so grab
        // requests and shutdown are noticed promptly without busy-waiting.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 100 * 1000};   // 100 ms
        select(fd + 1, &rfds, nullptr, nullptr, &tv);
    }

    if (g_grabbed) {
        XUngrabKeyboard(g_dpy, CurrentTime);
        XUngrabPointer(g_dpy, CurrentTime);
        g_grabbed = false;
    }
}

void EnsureStarted() {
    if (g_running.load()) return;
    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        OVERLAY_LOG("linux input: XOpenDisplay failed; capture unavailable");
        return;
    }
    g_running.store(true);
    g_thread = std::thread(ThreadMain);
}

}  // namespace

void EnterLinuxCapture(uint64_t gameWindow) {
    EnsureStarted();
    g_gameWindow.store(gameWindow, std::memory_order_relaxed);
    g_wantCapture.store(true, std::memory_order_relaxed);
}

void LeaveLinuxCapture() {
    g_wantCapture.store(false, std::memory_order_relaxed);
}

void ShutdownLinuxInput() {
    g_wantCapture.store(false, std::memory_order_relaxed);
    if (g_running.exchange(false)) {
        if (g_thread.joinable()) g_thread.join();
    }
    if (g_dpy) {
        XCloseDisplay(g_dpy);
        g_dpy = nullptr;
    }
}

}  // namespace overlay

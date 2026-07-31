// input.cpp - intercepts the game's input while the overlay is interactive.
//
// This is a window-procedure subclass rather than a host-side low-level hook
// for one decisive reason: WH_MOUSE_LL cannot reliably suppress WM_INPUT, and
// modern games read mouse-look through Raw Input. Left unsuppressed, opening
// the overlay would still spin the player's camera. WM_INPUT is an ordinary
// window message, so a subclass can swallow it.
//
// Everything here runs on the game's message thread. When capture is off the
// whole path is a single flag test and a tail call to the original procedure.

#include "input.h"

#include <windowsx.h>

#include "log.h"
#include "shared_state.h"

namespace overlay {
namespace {

HWND     g_hwnd = nullptr;
WNDPROC  g_originalProc = nullptr;
bool     g_capturing = false;      // our view of state->inputCapture
RECT     g_savedClip{};
bool     g_hadClip = false;

uint32_t CurrentModifiers() {
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= kModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= kModControl;
    if (GetKeyState(VK_MENU) & 0x8000)    mods |= kModAlt;
    if (GetKeyState(VK_LBUTTON) & 0x8000) mods |= kModLeft;
    if (GetKeyState(VK_RBUTTON) & 0x8000) mods |= kModRight;
    if (GetKeyState(VK_MBUTTON) & 0x8000) mods |= kModMiddle;
    return mods;
}

void Push(InputEventType type, int32_t x, int32_t y, uint32_t data) {
    InputEvent event{};
    event.type = static_cast<uint32_t>(type);
    event.x = x;
    event.y = y;
    event.data = data;
    event.modifiers = CurrentModifiers();

    InputRing* ring = GetInputRing();
    PushInputEvent(ring, event);

    if (SharedState* state = GetSharedState()) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->inputSeenCount));
        if (ring) InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->inputPushCount));
    }
}

// Releasing the cursor clip lets the pointer reach the whole window again.
// Games that re-clip every frame will fight this, which is exactly why the
// host tracks a virtual cursor rather than trusting the OS one.
void EnterCapture() {
    if (g_capturing) return;
    g_capturing = true;

    g_hadClip = GetClipCursor(&g_savedClip) != FALSE;
    ClipCursor(nullptr);
    OVERLAY_LOG("input capture on");
}

void LeaveCapture() {
    if (!g_capturing) return;
    g_capturing = false;

    if (g_hadClip) {
        ClipCursor(&g_savedClip);
        g_hadClip = false;
    }
    OVERLAY_LOG("input capture off");
}

// Reads relative motion out of a WM_INPUT message. Used when the game has the
// cursor captured for mouse-look, where WM_MOUSEMOVE stops carrying useful
// absolute positions.
void HandleRawInput(LPARAM lparam) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                        sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
        return;
    }
    if (raw.header.dwType != RIM_TYPEMOUSE) return;

    // Absolute-mode raw mice (tablets, some VMs) report a normalised position
    // rather than a delta; ignore those here and let WM_MOUSEMOVE drive.
    if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;

    const LONG dx = raw.data.mouse.lLastX;
    const LONG dy = raw.data.mouse.lLastY;
    if (dx != 0 || dy != 0) {
        Push(InputEventType::MouseMoveDelta, dx, dy, 0);
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    SharedState* state = GetSharedState();
    const bool wantCapture = state && state->inputCapture != 0 && state->visible != 0;

    if (wantCapture != g_capturing) {
        if (wantCapture) EnterCapture(); else LeaveCapture();
    }

    if (!g_capturing) {
        return CallWindowProcW(g_originalProc, hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_MOUSEMOVE:
        Push(InputEventType::MouseMove, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), 0);
        return 0;

    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        Push(InputEventType::MouseDown, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             static_cast<uint32_t>(MouseButton::Left));
        return 0;
    case WM_LBUTTONUP:
        Push(InputEventType::MouseUp, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             static_cast<uint32_t>(MouseButton::Left));
        return 0;

    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        Push(InputEventType::MouseDown, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             static_cast<uint32_t>(MouseButton::Right));
        return 0;
    case WM_RBUTTONUP:
        Push(InputEventType::MouseUp, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             static_cast<uint32_t>(MouseButton::Right));
        return 0;

    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        Push(InputEventType::MouseDown, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             static_cast<uint32_t>(MouseButton::Middle));
        return 0;
    case WM_MBUTTONUP:
        Push(InputEventType::MouseUp, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             static_cast<uint32_t>(MouseButton::Middle));
        return 0;

    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        Push(InputEventType::MouseDown, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? static_cast<uint32_t>(MouseButton::X1)
                                                    : static_cast<uint32_t>(MouseButton::X2));
        return TRUE;
    case WM_XBUTTONUP:
        Push(InputEventType::MouseUp, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
             GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? static_cast<uint32_t>(MouseButton::X1)
                                                    : static_cast<uint32_t>(MouseButton::X2));
        return TRUE;

    // Wheel coordinates are in screen space, unlike every other mouse message.
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &pt);
        Push(msg == WM_MOUSEWHEEL ? InputEventType::MouseWheel : InputEventType::MouseHWheel,
             pt.x, pt.y, static_cast<uint32_t>(GET_WHEEL_DELTA_WPARAM(wparam)));
        return 0;
    }

    // lParam carries the scan code and extended-key bit, which Avalonia's
    // KeyInterop needs to distinguish e.g. left and right Ctrl.
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        Push(InputEventType::KeyDown, 0, static_cast<int32_t>(lparam), static_cast<uint32_t>(wparam));
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        Push(InputEventType::KeyUp, 0, static_cast<int32_t>(lparam), static_cast<uint32_t>(wparam));
        return 0;

    case WM_CHAR:
        Push(InputEventType::Char, 0, 0, static_cast<uint32_t>(wparam));
        return 0;

    case WM_INPUT:
        HandleRawInput(lparam);
        // Still let the system clean up the message, but never let the game
        // see the deltas - that is what stops the camera moving.
        return 0;

    // The overlay draws its own cursor, so stop the game re-asserting its
    // (often hidden) one on every mouse move.
    case WM_SETCURSOR:
        return TRUE;

    default:
        // Everything structural - WM_SIZE, WM_CLOSE, WM_ACTIVATE, WM_PAINT -
        // must still reach the game or it breaks in ways unrelated to input.
        return CallWindowProcW(g_originalProc, hwnd, msg, wparam, lparam);
    }
}

} // namespace

void InstallInputHook(HWND hwnd) {
    if (!hwnd || g_originalProc) return;
    if (!IsWindow(hwnd)) {
        OVERLAY_LOG("input hook: 0x%p is not a window", static_cast<void*>(hwnd));
        return;
    }

    g_hwnd = hwnd;
    g_originalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));

    if (!g_originalProc) {
        OVERLAY_LOG("input hook: SetWindowLongPtr failed: %lu", GetLastError());
        g_hwnd = nullptr;
        return;
    }
    OVERLAY_LOG("input hook installed on hwnd 0x%p", static_cast<void*>(hwnd));
}

void RemoveInputHook() {
    if (!g_originalProc || !g_hwnd) return;

    LeaveCapture();

    // Only unsubclass if we are still the outermost procedure. If another
    // overlay subclassed after us, restoring would unhook them too.
    auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC));
    if (current == &OverlayWndProc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalProc));
        OVERLAY_LOG("input hook removed");
    } else {
        // Leaving ours in place is the lesser evil: it forwards everything once
        // capture is off, whereas clobbering the other subclass would break it.
        OVERLAY_LOG("input hook: another subclass is installed on top; leaving ours in place");
    }

    g_originalProc = nullptr;
    g_hwnd = nullptr;
}

void ForceReleaseCapture() {
    if (SharedState* state = GetSharedState()) {
        state->inputCapture = 0;
    }
    LeaveCapture();
}

} // namespace overlay

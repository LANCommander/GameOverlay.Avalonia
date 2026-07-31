// SampleGameOpenGL - the OpenGL counterpart of the D3D and Vulkan sample games.
//
// OpenGL is the odd one out: it has no swapchain object and no cross-API
// sharing of its own. The overlay reaches its framebuffer through
// wglSwapBuffers and shares the host's D3D texture via WGL_NV_DX_interop2.
// This target exercises that path.
//
// Controls:
//   F1  windowed          F2  borderless fullscreen     F3  exclusive fullscreen
//   V   toggle vsync      C   clear frame stats         ESC quit

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "gl_min.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

enum class DisplayMode { Windowed, Borderless, Exclusive };

typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXT)(int);

HWND   g_hwnd = nullptr;
HDC    g_dc = nullptr;
HGLRC  g_rc = nullptr;
GlFunctions gl;
PFNWGLSWAPINTERVALEXT wglSwapIntervalEXT_ = nullptr;

GLuint g_program = 0;
GLuint g_vao = 0;
GLint  g_angleLoc = -1;
GLint  g_aspectLoc = -1;

bool        g_vsync = false;
DisplayMode g_mode = DisplayMode::Windowed;
bool        g_running = true;
bool        g_exclusiveRefused = false;
RECT        g_windowedRect = { 0, 0, 1280, 720 };
unsigned    g_inputReceived = 0;

std::vector<double> g_frameTimes;
LARGE_INTEGER       g_qpcFreq{};
LARGE_INTEGER       g_lastFrameQpc{};
double              g_titleAccumMs = 0.0;

const char* kVertexShader = R"(#version 330 core
uniform float angle;
uniform float aspect;
out vec3 vColor;
void main() {
    float a = angle + float(gl_VertexID) * 2.0943951;   // 120 degrees apart
    vec2 p = vec2(cos(a), sin(a)) * 0.6;
    p.x /= aspect;
    gl_Position = vec4(p, 0.0, 1.0);
    vColor = vec3(gl_VertexID == 0 ? 1.0 : 0.0,
                  gl_VertexID == 1 ? 1.0 : 0.0,
                  gl_VertexID == 2 ? 1.0 : 0.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec3 vColor;
out vec4 outColor;
void main() { outColor = vec4(vColor, 1.0); }
)";

void Fatal(const char* what) {
    MessageBoxA(nullptr, what, "SampleGameOpenGL", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = gl.glCreateShader(type);
    gl.glShaderSource(shader, 1, &source, nullptr);
    gl.glCompileShader(shader);
    GLint ok = 0;
    gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        Fatal(log);
    }
    return shader;
}

void InitGL() {
    g_dc = GetDC(g_hwnd);

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) Fatal("SetPixelFormat failed");

    g_rc = wglCreateContext(g_dc);
    if (!g_rc || !wglMakeCurrent(g_dc, g_rc)) Fatal("wglCreateContext failed");

    if (!gl.Load()) Fatal("failed to load modern OpenGL entry points");
    wglSwapIntervalEXT_ = reinterpret_cast<PFNWGLSWAPINTERVALEXT>(wglGetProcAddress("wglSwapIntervalEXT"));

    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    g_program = gl.glCreateProgram();
    gl.glAttachShader(g_program, vs);
    gl.glAttachShader(g_program, fs);
    gl.glLinkProgram(g_program);
    GLint linked = 0;
    gl.glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked) Fatal("shader link failed");
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);

    g_angleLoc = gl.glGetUniformLocation(g_program, "angle");
    g_aspectLoc = gl.glGetUniformLocation(g_program, "aspect");

    // A VAO must be bound to draw in the core profile, even with no vertex data.
    gl.glGenVertexArrays(1, &g_vao);
}

void ApplyVSync() {
    if (wglSwapIntervalEXT_) wglSwapIntervalEXT_(g_vsync ? 1 : 0);
}

void SetDisplayMode(DisplayMode mode) {
    if (mode == g_mode) return;

    if (g_mode == DisplayMode::Windowed && mode != DisplayMode::Windowed) {
        GetWindowRect(g_hwnd, &g_windowedRect);
    }
    g_exclusiveRefused = false;

    if (mode == DisplayMode::Windowed) {
        ChangeDisplaySettingsW(nullptr, 0);   // leave any exclusive mode
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, g_windowedRect.left, g_windowedRect.top,
                     g_windowedRect.right - g_windowedRect.left,
                     g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED);
    } else {
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);

        // OpenGL on Windows has no exclusive-fullscreen API of its own; the
        // convention is a borderless monitor-sized window, optionally with a
        // real display-mode change for "exclusive". A ChangeDisplaySettings
        // mode change is the closest equivalent; if it fails, borderless still
        // covers the screen.
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);

        if (mode == DisplayMode::Exclusive) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            dm.dmPelsWidth = mi.rcMonitor.right - mi.rcMonitor.left;
            dm.dmPelsHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
            dm.dmBitsPerPel = 32;
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
            if (ChangeDisplaySettingsW(&dm, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL) {
                g_exclusiveRefused = true;
            }
        }
    }

    g_mode = mode;
}

void UpdateTitle(double frameMs) {
    g_titleAccumMs += frameMs;
    if (g_titleAccumMs < 250.0) return;
    g_titleAccumMs = 0.0;

    size_t take = std::min<size_t>(g_frameTimes.size(), 2000);
    std::vector<double> recent(g_frameTimes.end() - static_cast<ptrdiff_t>(take), g_frameTimes.end());
    std::sort(recent.begin(), recent.end());

    double avg = 0.0;
    for (double v : recent) avg += v;
    avg = recent.empty() ? 0.0 : avg / recent.size();
    double p50 = recent.empty() ? 0.0 : recent[recent.size() / 2];
    double p99 = recent.empty() ? 0.0 : recent[static_cast<size_t>(recent.size() * 0.99)];

    const char* modeName = g_mode == DisplayMode::Windowed   ? "windowed"
                         : g_mode == DisplayMode::Borderless ? "borderless"
                                                             : "EXCLUSIVE";
    char title[380];
    sprintf_s(title,
              "SampleGameOpenGL [%s]%s vsync:%s | %.0f fps | avg %.3f ms  p50 %.3f  p99 %.3f | input %u | pid %lu",
              modeName, g_exclusiveRefused ? " (exclusive REFUSED)" : "",
              g_vsync ? "on" : "off",
              avg > 0.0 ? 1000.0 / avg : 0.0, avg, p50, p99,
              g_inputReceived, GetCurrentProcessId());
    SetWindowTextA(g_hwnd, title);
}

void RenderFrame() {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    static float angle = 0.0f;
    angle += 0.01f;

    glViewport(0, 0, width, height);
    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    gl.glUseProgram(g_program);
    gl.glUniform1f(g_angleLoc, angle);
    gl.glUniform1f(g_aspectLoc, static_cast<float>(width) / static_cast<float>(height));
    gl.glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // wglSwapBuffers is where the overlay hooks in.
    SwapBuffers(g_dc);

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double frameUs = static_cast<double>(now.QuadPart - g_lastFrameQpc.QuadPart) * 1e6
                   / static_cast<double>(g_qpcFreq.QuadPart);
    g_lastFrameQpc = now;

    if (frameUs > 0.0 && frameUs < 1e6) {
        g_frameTimes.push_back(frameUs / 1000.0);
        if (g_frameTimes.size() > 200000) {
            g_frameTimes.erase(g_frameTimes.begin(), g_frameTimes.begin() + 100000);
        }
        UpdateTitle(frameUs / 1000.0);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_CHAR:
        ++g_inputReceived;
        break;
    default:
        break;
    }

    switch (msg) {
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;

    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN) {
            SetDisplayMode(g_mode == DisplayMode::Exclusive ? DisplayMode::Windowed
                                                            : DisplayMode::Exclusive);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        ++g_inputReceived;
        switch (wparam) {
        case VK_ESCAPE: g_running = false; PostQuitMessage(0); return 0;
        case VK_F1: SetDisplayMode(DisplayMode::Windowed);   return 0;
        case VK_F2: SetDisplayMode(DisplayMode::Borderless); return 0;
        case VK_F3: SetDisplayMode(DisplayMode::Exclusive);  return 0;
        case 'V':   g_vsync = !g_vsync; ApplyVSync(); g_frameTimes.clear(); return 0;
        case 'C':   g_frameTimes.clear(); return 0;
        default: break;
        }
        break;

    default: break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    // Real GL games declare DPI awareness; without it Windows stretches the
    // window and the overlay's pointer mapping is thrown off by the scale.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring cmd(lpCmdLine ? lpCmdLine : L"");
    g_vsync = cmd.find(L"-vsync") != std::wstring::npos;

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastFrameQpc);
    g_frameTimes.reserve(200000);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SampleGameOpenGLWindow";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SampleGameOpenGL", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) Fatal("CreateWindowEx failed");

    InitGL();
    ApplyVSync();
    ShowWindow(g_hwnd, SW_SHOW);
    GetWindowRect(g_hwnd, &g_windowedRect);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;
        RenderFrame();
    }

    ChangeDisplaySettingsW(nullptr, 0);
    wglMakeCurrent(nullptr, nullptr);
    if (g_rc) wglDeleteContext(g_rc);
    return 0;
}

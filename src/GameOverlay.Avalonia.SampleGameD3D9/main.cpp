// SampleGameD3D9 - a small Direct3D 9 "game" used as an overlay test target.
//
// It is the D3D9 counterpart to the other sample games: a spinning triangle in a
// resizable window, whose title reports its pid, input count and frame pacing so
// the capture/measure harnesses can drive and read it. Its purpose is to
// exercise the overlay's D3D9 path end to end - the EndScene/Reset hooks and the
// CPU shared-memory frame transport.
//
// Controls:  F1 windowed (no-op; kept for harness parity)   ESC quit

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>

#pragma comment(lib, "d3d9.lib")

namespace {

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
};
constexpr DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

HWND                  g_hwnd = nullptr;
IDirect3D9*           g_d3d = nullptr;
IDirect3DDevice9*     g_device = nullptr;
D3DPRESENT_PARAMETERS g_pp{};
bool                  g_running = true;
bool                  g_deviceLost = false;
unsigned              g_inputReceived = 0;

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_lastFrameQpc{};
double        g_titleAccumMs = 0.0;
double        g_lastFrameMs = 0.0;

void Fatal(const char* what, HRESULT hr) {
    char msg[512];
    sprintf_s(msg, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    MessageBoxA(nullptr, msg, "SampleGameD3D9", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

void InitD3D() {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) Fatal("Direct3DCreate9", E_FAIL);

    RECT rc{};
    GetClientRect(g_hwnd, &rc);

    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_pp.BackBufferWidth = rc.right - rc.left;
    g_pp.BackBufferHeight = rc.bottom - rc.top;
    g_pp.hDeviceWindow = g_hwnd;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   // vsync off, so overlay cost shows

    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_device);
    if (FAILED(hr)) {
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_device);
    }
    if (FAILED(hr)) Fatal("CreateDevice", hr);
}

void ResetDevice() {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    if (rc.right - rc.left == 0 || rc.bottom - rc.top == 0) return;

    g_pp.BackBufferWidth = rc.right - rc.left;
    g_pp.BackBufferHeight = rc.bottom - rc.top;

    // DrawPrimitiveUP uses no D3DPOOL_DEFAULT resources of ours, so there is
    // nothing to release before the reset.
    HRESULT hr = g_device->Reset(&g_pp);
    if (FAILED(hr)) {
        g_deviceLost = true;   // try again next frame
        return;
    }
    g_deviceLost = false;
}

void UpdateTitle() {
    g_titleAccumMs += g_lastFrameMs;
    if (g_titleAccumMs < 250.0) return;
    g_titleAccumMs = 0.0;

    char title[256];
    sprintf_s(title, "SampleGameD3D9 [windowed] | %.0f fps | %.3f ms | input %u | pid %lu",
              g_lastFrameMs > 0.0 ? 1000.0 / g_lastFrameMs : 0.0,
              g_lastFrameMs, g_inputReceived, GetCurrentProcessId());
    SetWindowTextA(g_hwnd, title);
}

void RenderFrame() {
    if (g_deviceLost) {
        if (g_device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) ResetDevice();
        if (g_deviceLost) { Sleep(10); return; }
    }

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    float w = static_cast<float>(rc.right - rc.left);
    float h = static_cast<float>(rc.bottom - rc.top);
    if (w <= 0.0f || h <= 0.0f) return;

    static float angle = 0.0f;
    angle += 0.01f;

    float cx = w * 0.5f, cy = h * 0.5f;
    float radius = (w < h ? w : h) * 0.3f;
    const DWORD colors[3] = { 0xFFFF4040, 0xFF40FF40, 0xFF4080FF };
    Vertex verts[3];
    for (int i = 0; i < 3; ++i) {
        float a = angle + i * 2.0943951f;   // 120 degrees apart
        verts[i] = { cx + std::cos(a) * radius, cy + std::sin(a) * radius, 0.0f, 1.0f, colors[i] };
    }

    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(15, 18, 26), 1.0f, 0);
    if (SUCCEEDED(g_device->BeginScene())) {
        g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_device->SetFVF(kFVF);
        g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, verts, sizeof(Vertex));
        g_device->EndScene();
    }

    HRESULT hr = g_device->Present(nullptr, nullptr, nullptr, nullptr);
    if (hr == D3DERR_DEVICELOST) g_deviceLost = true;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double frameUs = static_cast<double>(now.QuadPart - g_lastFrameQpc.QuadPart) * 1e6
                   / static_cast<double>(g_qpcFreq.QuadPart);
    g_lastFrameQpc = now;
    if (frameUs > 0.0 && frameUs < 1e6) {
        g_lastFrameMs = frameUs / 1000.0;
        UpdateTitle();
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

    case WM_SIZE:
        if (g_device && wparam != SIZE_MINIMIZED) ResetDevice();
        return 0;

    case WM_KEYDOWN:
        ++g_inputReceived;
        if (wparam == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastFrameQpc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SampleGameD3D9Window";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SampleGameD3D9", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) Fatal("CreateWindowEx", HRESULT_FROM_WIN32(GetLastError()));

    InitD3D();
    ShowWindow(g_hwnd, SW_SHOW);

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

    if (g_device) g_device->Release();
    if (g_d3d) g_d3d->Release();
    return 0;
}

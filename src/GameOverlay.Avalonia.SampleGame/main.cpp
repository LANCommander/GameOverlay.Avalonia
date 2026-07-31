// SampleGame - a deliberately small D3D11 "game" used as the overlay test target.
//
// It exists so the overlay can be developed and, more importantly, *measured*
// against something we control, instead of against anti-cheat-protected
// software. It therefore cares about two things a toy triangle normally
// wouldn't:
//
//   * all three display modes (windowed / borderless / exclusive fullscreen),
//     because exclusive fullscreen is the mode that invalidates every
//     layered-window overlay approach and is the whole reason we hook Present;
//   * accurate frame pacing statistics, so "what does the overlay cost?" has a
//     real answer rather than a vibe.
//
// Controls:
//   F1  windowed          F2  borderless fullscreen     F3  exclusive fullscreen
//   V   toggle vsync      F5  dump frame times to CSV   ESC quit
//
// Command line:
//   -srgb     render through an _SRGB RTV over a UNORM flip-model backbuffer
//   -bitblt   use the legacy bitblt swap effect with a true _SRGB backbuffer
//   -vsync    start with vsync on (default off, so overlay cost is visible)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

enum class DisplayMode { Windowed, Borderless, Exclusive };

struct Options {
    bool srgbRtv = false;
    bool bitblt = false;
    bool vsync = false;
};

HWND                     g_hwnd = nullptr;
ID3D11Device*            g_device = nullptr;
ID3D11DeviceContext*     g_context = nullptr;
IDXGISwapChain1*         g_swapChain = nullptr;
ID3D11RenderTargetView*  g_rtv = nullptr;
ID3D11VertexShader*      g_vs = nullptr;
ID3D11PixelShader*       g_ps = nullptr;
ID3D11Buffer*            g_cb = nullptr;
ID3D11RasterizerState*   g_rasterizer = nullptr;

Options     g_opts;
DisplayMode g_mode = DisplayMode::Windowed;
bool        g_running = true;
bool        g_inSizeMove = false;
bool        g_exclusiveRefused = false;

// Counts input the game actually received. When the overlay is capturing, this
// must stop advancing - that is how "the game is not seeing input" becomes an
// observable fact rather than an assumption.
unsigned    g_inputReceived = 0;
UINT        g_swapFlags = 0;
RECT        g_windowedRect = { 0, 0, 1280, 720 };

// Frame pacing statistics. Frame times are recorded in microseconds.
std::vector<double> g_frameTimes;
LARGE_INTEGER       g_qpcFreq{};
LARGE_INTEGER       g_lastFrameQpc{};
double              g_titleAccumMs = 0.0;

const char kShaderSrc[] = R"(
cbuffer Params : register(b0) { float angle; float aspect; float2 pad; };

struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Three corners of a triangle, rotated on the CPU-supplied angle.
    float a = angle + id * 2.0943951;      // 120 degrees apart
    float2 p = float2(cos(a), sin(a)) * 0.6;
    p.x /= aspect;

    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.col = float3(id == 0, id == 1, id == 2);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(i.col, 1.0);
}
)";

void Fatal(const char* what, HRESULT hr) {
    char msg[512];
    sprintf_s(msg, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    MessageBoxA(nullptr, msg, "SampleGame", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

DXGI_FORMAT BackbufferFormat() {
    // Flip-model swap chains cannot use an _SRGB backbuffer format; the
    // sanctioned way to get sRGB output is a UNORM backbuffer with an _SRGB
    // render target view over it. The legacy bitblt path has no such rule, so
    // -bitblt is how we exercise a genuinely _SRGB swapchain format.
    if (g_opts.bitblt && g_opts.srgbRtv) return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) Fatal("GetBuffer", hr);

    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = (g_opts.srgbRtv && !g_opts.bitblt) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                     : BackbufferFormat();
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    hr = g_device->CreateRenderTargetView(backBuffer, &desc, &g_rtv);
    backBuffer->Release();
    if (FAILED(hr)) Fatal("CreateRenderTargetView", hr);
}

void ReleaseRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

void ResizeSwapChain(UINT width, UINT height) {
    if (!g_swapChain || width == 0 || height == 0) return;
    ReleaseRenderTarget();
    // Passing 0/0 lets DXGI pick up the current client size, which is what we
    // want on an exclusive-fullscreen mode transition.
    HRESULT hr = g_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, g_swapFlags);
    if (FAILED(hr)) Fatal("ResizeBuffers", hr);
    CreateRenderTarget();
}

void InitD3D() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device, nullptr, &context);
    if (FAILED(hr)) {
        // Retry without the debug layer; it is absent unless the Graphics Tools
        // optional feature is installed, and that is a very common footgun.
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device, nullptr, &context);
    }
    if (FAILED(hr)) Fatal("D3D11CreateDevice", hr);
    g_device = device;
    g_context = context;

    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) Fatal("QI IDXGIDevice", hr);

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) Fatal("GetAdapter", hr);

    IDXGIFactory2* factory = nullptr;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) Fatal("GetParent IDXGIFactory2", hr);

    RECT rc{};
    GetClientRect(g_hwnd, &rc);

    // ALLOW_MODE_SWITCH is what makes SetFullscreenState actually take an
    // exclusive display mode rather than silently staying composited.
    g_swapFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = rc.right - rc.left;
    scd.Height = rc.bottom - rc.top;
    scd.Format = BackbufferFormat();
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = g_opts.bitblt ? 1 : 2;
    scd.SwapEffect = g_opts.bitblt ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = g_swapFlags;

    hr = factory->CreateSwapChainForHwnd(g_device, g_hwnd, &scd, nullptr, nullptr, &g_swapChain);
    if (FAILED(hr)) Fatal("CreateSwapChainForHwnd", hr);

    // We drive fullscreen transitions ourselves so the mode is deterministic
    // and testable; DXGI's built-in Alt+Enter would fight us.
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    factory->Release();
    adapter->Release();
    dxgiDevice->Release();

    CreateRenderTarget();

    // --- shaders -------------------------------------------------------
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                    "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        MessageBoxA(nullptr, errBlob ? static_cast<char*>(errBlob->GetBufferPointer()) : "?",
                    "VS compile", MB_ICONERROR);
        ExitProcess(1);
    }
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        MessageBoxA(nullptr, errBlob ? static_cast<char*>(errBlob->GetBufferPointer()) : "?",
                    "PS compile", MB_ICONERROR);
        ExitProcess(1);
    }

    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    vsBlob->Release();
    psBlob->Release();

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_device->CreateBuffer(&cbd, nullptr, &g_cb);
    if (FAILED(hr)) Fatal("CreateBuffer", hr);

    // The triangle's three vertices are generated at 120-degree intervals,
    // which winds them counter-clockwise. D3D11's default rasterizer treats
    // clockwise as front-facing and would cull them at every angle, so cull
    // nothing rather than depending on the generated winding.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = g_device->CreateRasterizerState(&rd, &g_rasterizer);
    if (FAILED(hr)) Fatal("CreateRasterizerState", hr);
}

void SetDisplayMode(DisplayMode mode) {
    if (mode == g_mode) return;

    BOOL currentlyExclusive = FALSE;
    g_swapChain->GetFullscreenState(&currentlyExclusive, nullptr);

    // Always drop out of exclusive first; changing window styles underneath an
    // exclusive swapchain is a reliable way to wedge DXGI.
    if (currentlyExclusive) {
        g_swapChain->SetFullscreenState(FALSE, nullptr);
    }

    if (g_mode == DisplayMode::Windowed && mode != DisplayMode::Windowed) {
        GetWindowRect(g_hwnd, &g_windowedRect);
    }

    if (mode == DisplayMode::Exclusive) {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        HRESULT hr = g_swapChain->SetFullscreenState(TRUE, nullptr);
        if (FAILED(hr)) {
            // Exclusive mode can legitimately be refused (another exclusive app
            // owns the output, the window is not foreground, ...). Fall back
            // silently: a modal dialog here would block the message loop, and a
            // game that stops calling Present stops the overlay too, which
            // looks exactly like an overlay bug.
            g_exclusiveRefused = true;
            mode = DisplayMode::Borderless;
        } else {
            g_exclusiveRefused = false;
        }
    }

    if (mode == DisplayMode::Borderless) {
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else if (mode == DisplayMode::Windowed) {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, g_windowedRect.left, g_windowedRect.top,
                     g_windowedRect.right - g_windowedRect.left,
                     g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED);
    }

    g_mode = mode;

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    ResizeSwapChain(rc.right - rc.left, rc.bottom - rc.top);
}

void DumpFrameTimes() {
    if (g_frameTimes.empty()) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string out(path);
    out = out.substr(0, out.find_last_of('\\') + 1) + "frametimes.csv";

    FILE* f = nullptr;
    if (fopen_s(&f, out.c_str(), "w") != 0 || !f) return;
    fprintf(f, "frame_index,frame_time_us\n");
    for (size_t i = 0; i < g_frameTimes.size(); ++i) {
        fprintf(f, "%zu,%.2f\n", i, g_frameTimes[i]);
    }
    fclose(f);

    std::string msg = "Wrote " + std::to_string(g_frameTimes.size()) + " frame times to\n" + out;
    MessageBoxA(g_hwnd, msg.c_str(), "SampleGame", MB_OK | MB_ICONINFORMATION);
}

void UpdateTitle(double frameMs) {
    g_titleAccumMs += frameMs;
    if (g_titleAccumMs < 250.0) return;
    g_titleAccumMs = 0.0;

    // Percentiles over the recent window, which is what actually reveals a
    // per-frame overlay cost; a mean hides it.
    size_t n = g_frameTimes.size();
    size_t take = std::min<size_t>(n, 2000);
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
    char title[320];
    sprintf_s(title,
              "SampleGame [%s]%s vsync:%s | %.0f fps | avg %.3f ms  p50 %.3f  p99 %.3f | input %u | pid %lu",
              modeName, g_exclusiveRefused ? " (exclusive REFUSED)" : "",
              g_opts.vsync ? "on" : "off",
              avg > 0.0 ? 1000.0 / avg : 0.0,
              avg, p50, p99, g_inputReceived, GetCurrentProcessId());
    SetWindowTextA(g_hwnd, title);
}

void RenderFrame() {
    if (!g_rtv) return;

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    float height = static_cast<float>(rc.bottom - rc.top);
    if (width <= 0.0f || height <= 0.0f) return;

    static float angle = 0.0f;
    angle += 0.01f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float* p = static_cast<float*>(mapped.pData);
        p[0] = angle;
        p[1] = width / height;
        p[2] = 0.0f;
        p[3] = 0.0f;
        g_context->Unmap(g_cb, 0);
    }

    const float clear[4] = { 0.06f, 0.07f, 0.10f, 1.0f };
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    g_context->RSSetViewports(1, &vp);
    g_context->RSSetState(g_rasterizer);

    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->IASetInputLayout(nullptr);
    g_context->VSSetShader(g_vs, nullptr, 0);
    g_context->VSSetConstantBuffers(0, 1, &g_cb);
    g_context->PSSetShader(g_ps, nullptr, 0);
    g_context->Draw(3, 0);

    g_swapChain->Present(g_opts.vsync ? 1 : 0, 0);

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double frameUs = static_cast<double>(now.QuadPart - g_lastFrameQpc.QuadPart) * 1e6
                   / static_cast<double>(g_qpcFreq.QuadPart);
    g_lastFrameQpc = now;

    if (frameUs > 0.0 && frameUs < 1e6) {
        g_frameTimes.push_back(frameUs / 1000.0);   // store as milliseconds
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

    case WM_ENTERSIZEMOVE: g_inSizeMove = true;  return 0;
    case WM_EXITSIZEMOVE:  g_inSizeMove = false; {
        RECT rc{}; GetClientRect(hwnd, &rc);
        ResizeSwapChain(rc.right - rc.left, rc.bottom - rc.top);
        return 0;
    }

    case WM_SIZE:
        if (g_swapChain && wparam != SIZE_MINIMIZED && !g_inSizeMove) {
            ResizeSwapChain(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;

    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN) {   // Alt+Enter
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
        case VK_F5: DumpFrameTimes(); return 0;
        case 'V':   g_opts.vsync = !g_opts.vsync; g_frameTimes.clear(); return 0;
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
    std::wstring cmd(lpCmdLine ? lpCmdLine : L"");
    g_opts.srgbRtv = cmd.find(L"-srgb")   != std::wstring::npos;
    g_opts.bitblt  = cmd.find(L"-bitblt") != std::wstring::npos;
    g_opts.vsync   = cmd.find(L"-vsync")  != std::wstring::npos;

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastFrameQpc);
    g_frameTimes.reserve(200000);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SampleGameWindow";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SampleGame", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) Fatal("CreateWindowEx", HRESULT_FROM_WIN32(GetLastError()));

    InitD3D();
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

    // Releasing a swapchain while it still holds an exclusive mode leaves the
    // display in a bad state, so drop out of fullscreen first.
    if (g_swapChain) g_swapChain->SetFullscreenState(FALSE, nullptr);

    ReleaseRenderTarget();
    if (g_rasterizer) g_rasterizer->Release();
    if (g_cb) g_cb->Release();
    if (g_ps) g_ps->Release();
    if (g_vs) g_vs->Release();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    return 0;
}

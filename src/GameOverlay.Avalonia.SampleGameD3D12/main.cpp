// SampleGameD3D12 - the D3D12 counterpart of SampleGame.
//
// D3D12 is the case that matters for real titles and the one the overlay finds
// hardest: there is no immediate context to borrow, the command queue cannot be
// retrieved from the swapchain, and every backbuffer write has to sit between
// explicit resource barriers. This target exists so all of that can be
// exercised against something we control.
//
// It deliberately mirrors SampleGame's controls, title-bar statistics and input
// counter so the same tools work against either renderer.
//
// Controls:
//   F1  windowed          F2  borderless fullscreen     F3  exclusive fullscreen
//   V   toggle vsync      C   clear frame stats         ESC quit

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

enum class DisplayMode { Windowed, Borderless, Exclusive };

// Three buffers so the CPU can stay ahead of the GPU, which is what a real
// title does and what makes per-frame barrier handling in the overlay matter.
constexpr UINT kFrameCount = 3;

HWND                          g_hwnd = nullptr;
ComPtr<ID3D12Device>          g_device;
ComPtr<ID3D12CommandQueue>    g_queue;
ComPtr<IDXGISwapChain3>       g_swapChain;
ComPtr<ID3D12DescriptorHeap>  g_rtvHeap;
ComPtr<ID3D12Resource>        g_renderTargets[kFrameCount];
ComPtr<ID3D12CommandAllocator> g_allocators[kFrameCount];
ComPtr<ID3D12GraphicsCommandList> g_commandList;
ComPtr<ID3D12RootSignature>   g_rootSignature;
ComPtr<ID3D12PipelineState>   g_pipelineState;
ComPtr<ID3D12Fence>           g_fence;
HANDLE                        g_fenceEvent = nullptr;
UINT64                        g_fenceValues[kFrameCount]{};
UINT                          g_frameIndex = 0;
UINT                          g_rtvDescriptorSize = 0;

bool        g_vsync = false;
DisplayMode g_mode = DisplayMode::Windowed;
bool        g_running = true;
bool        g_inSizeMove = false;
bool        g_exclusiveRefused = false;
UINT        g_swapFlags = 0;
RECT        g_windowedRect = { 0, 0, 1280, 720 };
unsigned    g_inputReceived = 0;

std::vector<double> g_frameTimes;
LARGE_INTEGER       g_qpcFreq{};
LARGE_INTEGER       g_lastFrameQpc{};
double              g_titleAccumMs = 0.0;

const char kShaderSrc[] = R"(
cbuffer Params : register(b0) { float angle; float aspect; float2 pad; };

struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };

VSOut VSMain(uint id : SV_VertexID)
{
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
    MessageBoxA(nullptr, msg, "SampleGameD3D12", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) Fatal(what, hr);
}

// Blocks until the GPU has finished everything queued. Needed before touching
// the swapchain buffers, which resize and teardown both do.
void WaitForGpu() {
    if (!g_queue || !g_fence) return;
    const UINT64 value = g_fenceValues[g_frameIndex];
    ThrowIfFailed(g_queue->Signal(g_fence.Get(), value), "Signal");
    ThrowIfFailed(g_fence->SetEventOnCompletion(value, g_fenceEvent), "SetEventOnCompletion");
    WaitForSingleObjectEx(g_fenceEvent, INFINITE, FALSE);
    g_fenceValues[g_frameIndex]++;
}

// Standard N-buffered advance: signal the frame just submitted, then only wait
// if the buffer we are about to reuse is still in flight.
void MoveToNextFrame() {
    const UINT64 submitted = g_fenceValues[g_frameIndex];
    ThrowIfFailed(g_queue->Signal(g_fence.Get(), submitted), "Signal");

    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    if (g_fence->GetCompletedValue() < g_fenceValues[g_frameIndex]) {
        ThrowIfFailed(g_fence->SetEventOnCompletion(g_fenceValues[g_frameIndex], g_fenceEvent),
                      "SetEventOnCompletion");
        WaitForSingleObjectEx(g_fenceEvent, INFINITE, FALSE);
    }
    g_fenceValues[g_frameIndex] = submitted + 1;
}

void CreateRenderTargets() {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i])), "GetBuffer");
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += g_rtvDescriptorSize;
    }
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
}

void ReleaseRenderTargets() {
    for (auto& rt : g_renderTargets) rt.Reset();
}

void ResizeSwapChain() {
    if (!g_swapChain) return;

    // Outstanding references to the buffers make ResizeBuffers fail outright,
    // and in-flight work referencing them would be undefined.
    WaitForGpu();
    ReleaseRenderTargets();

    ThrowIfFailed(g_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, g_swapFlags),
                  "ResizeBuffers");

    // Every buffer is fresh, so no frame is in flight against any of them.
    const UINT64 base = g_fenceValues[g_frameIndex];
    for (auto& v : g_fenceValues) v = base;

    CreateRenderTargets();
}

void InitD3D() {
    UINT factoryFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)),
                  "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue");

    RECT rc{};
    GetClientRect(g_hwnd, &rc);

    g_swapFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount = kFrameCount;
    scd.Width = rc.right - rc.left;
    scd.Height = rc.bottom - rc.top;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.Flags = g_swapFlags;

    // For D3D12 the swapchain is created against the COMMAND QUEUE, not the
    // device. That is also why an overlay cannot recover the queue from the
    // swapchain later - DXGI keeps no public accessor for it.
    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(g_queue.Get(), g_hwnd, &scd, nullptr, nullptr, &swapChain1),
                  "CreateSwapChainForHwnd");
    ThrowIfFailed(swapChain1.As(&g_swapChain), "QI IDXGISwapChain3");

    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)), "CreateDescriptorHeap");
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CreateRenderTargets();

    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&g_allocators[i])),
                      "CreateCommandAllocator");
    }

    // --- root signature: two floats of root constants, no descriptors -------
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParam.Constants.ShaderRegister = 0;
    rootParam.Constants.RegisterSpace = 0;
    rootParam.Constants.Num32BitValues = 4;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &rootParam;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &signature, &error),
                  "D3D12SerializeRootSignature");
    ThrowIfFailed(g_device->CreateRootSignature(0, signature->GetBufferPointer(),
                                                signature->GetBufferSize(),
                                                IID_PPV_ARGS(&g_rootSignature)),
                  "CreateRootSignature");

    // --- shaders and PSO -----------------------------------------------------
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                          "VSMain", "vs_5_0", 0, 0, &vs, &err))) {
        MessageBoxA(nullptr, err ? static_cast<char*>(err->GetBufferPointer()) : "?", "VS", MB_ICONERROR);
        ExitProcess(1);
    }
    if (FAILED(D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                          "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
        MessageBoxA(nullptr, err ? static_cast<char*>(err->GetBufferPointer()) : "?", "PS", MB_ICONERROR);
        ExitProcess(1);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Same reason as the D3D11 target: the generated vertices wind
    // counter-clockwise and would otherwise be culled at every angle.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineState)),
                  "CreateGraphicsPipelineState");

    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g_allocators[g_frameIndex].Get(),
                                              g_pipelineState.Get(), IID_PPV_ARGS(&g_commandList)),
                  "CreateCommandList");
    ThrowIfFailed(g_commandList->Close(), "Close");

    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence");
    g_fenceValues[g_frameIndex] = 1;
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) Fatal("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
}

void SetDisplayMode(DisplayMode mode) {
    if (mode == g_mode) return;

    BOOL exclusive = FALSE;
    g_swapChain->GetFullscreenState(&exclusive, nullptr);
    if (exclusive) g_swapChain->SetFullscreenState(FALSE, nullptr);

    if (g_mode == DisplayMode::Windowed && mode != DisplayMode::Windowed) {
        GetWindowRect(g_hwnd, &g_windowedRect);
    }

    if (mode == DisplayMode::Exclusive) {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        if (FAILED(g_swapChain->SetFullscreenState(TRUE, nullptr))) {
            // Fall back silently; a modal dialog here would block the message
            // loop and stop the game presenting, which looks like an overlay bug.
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
    ResizeSwapChain();
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
    char title[360];
    sprintf_s(title,
              "SampleGameD3D12 [%s]%s vsync:%s | %.0f fps | avg %.3f ms  p50 %.3f  p99 %.3f | input %u | pid %lu",
              modeName, g_exclusiveRefused ? " (exclusive REFUSED)" : "",
              g_vsync ? "on" : "off",
              avg > 0.0 ? 1000.0 / avg : 0.0, avg, p50, p99,
              g_inputReceived, GetCurrentProcessId());
    SetWindowTextA(g_hwnd, title);
}

void RenderFrame() {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    float height = static_cast<float>(rc.bottom - rc.top);
    if (width <= 0.0f || height <= 0.0f) return;

    static float angle = 0.0f;
    angle += 0.01f;

    ThrowIfFailed(g_allocators[g_frameIndex]->Reset(), "allocator Reset");
    ThrowIfFailed(g_commandList->Reset(g_allocators[g_frameIndex].Get(), g_pipelineState.Get()),
                  "commandList Reset");

    D3D12_RESOURCE_BARRIER toTarget{};
    toTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toTarget.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    toTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(1, &toTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;
    g_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float clear[4] = { 0.06f, 0.07f, 0.10f, 1.0f };
    g_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    g_commandList->RSSetViewports(1, &vp);
    g_commandList->RSSetScissorRects(1, &scissor);

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    const float params[4] = { angle, width / height, 0.0f, 0.0f };
    g_commandList->SetGraphicsRoot32BitConstants(0, 4, params, 0);
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_commandList->DrawInstanced(3, 1, 0, 0);

    // Back to PRESENT. The overlay will insert its own barrier pair after this
    // command list has been executed, from inside the Present hook.
    D3D12_RESOURCE_BARRIER toPresent = toTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(g_commandList->Close(), "commandList Close");

    ID3D12CommandList* lists[] = { g_commandList.Get() };
    g_queue->ExecuteCommandLists(1, lists);

    g_swapChain->Present(g_vsync ? 1 : 0, 0);
    MoveToNextFrame();

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

    case WM_ENTERSIZEMOVE: g_inSizeMove = true;  return 0;
    case WM_EXITSIZEMOVE:  g_inSizeMove = false; ResizeSwapChain(); return 0;

    case WM_SIZE:
        if (g_swapChain && wparam != SIZE_MINIMIZED && !g_inSizeMove) ResizeSwapChain();
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
        case 'V':   g_vsync = !g_vsync; g_frameTimes.clear(); return 0;
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
    g_vsync = cmd.find(L"-vsync") != std::wstring::npos;

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastFrameQpc);
    g_frameTimes.reserve(200000);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SampleGameD3D12Window";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SampleGameD3D12", WS_OVERLAPPEDWINDOW,
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

    WaitForGpu();
    if (g_swapChain) g_swapChain->SetFullscreenState(FALSE, nullptr);
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
    return 0;
}

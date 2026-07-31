#include "d3d10_renderer.h"

#include <d3dcompiler.h>
#include "log.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace overlay {
namespace {

// Identical to the D3D11 backend's shader: a fullscreen triangle from
// SV_VertexID with no vertex/index buffer, sampling a premultiplied BGRA
// overlay. Shader model 4 covers D3D10 unchanged.
const char kShaderSrc[] = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

Texture2D    overlayTex : register(t0);
SamplerState overlaySmp : register(s0);

float4 PSMain(VSOut i) : SV_Target
{
    return overlayTex.Sample(overlaySmp, i.uv);
}
)";

// See the D3D11 backend for why we always view the backbuffer as non-sRGB and
// write our already-encoded bytes verbatim.
DXGI_FORMAT StripSrgb(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:   return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:                              return format;
    }
}

bool IsSrgbFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

// Full pipeline state save/restore. D3D10 has no class instances (a D3D11
// addition), so this is a little simpler than the D3D11 version, but the intent
// is the same: hand the game back exactly the state it had. Roughly 1 KB on the
// stack, because the Present hook must not allocate.
struct StateBackup {
    UINT                     scissorCount = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT                     viewportCount = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D10_RECT               scissors[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D10_VIEWPORT           viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D10RasterizerState*   rasterizer = nullptr;
    ID3D10BlendState*        blend = nullptr;
    FLOAT                    blendFactor[4]{};
    UINT                     sampleMask = 0;
    UINT                     stencilRef = 0;
    ID3D10DepthStencilState* depthStencil = nullptr;
    ID3D10ShaderResourceView* psSrv = nullptr;
    ID3D10SamplerState*      psSampler = nullptr;
    ID3D10PixelShader*       ps = nullptr;
    ID3D10VertexShader*      vs = nullptr;
    ID3D10GeometryShader*    gs = nullptr;
    D3D10_PRIMITIVE_TOPOLOGY topology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D10Buffer*            indexBuffer = nullptr;
    ID3D10Buffer*            vertexBuffer = nullptr;
    ID3D10Buffer*            vsConstantBuffer = nullptr;
    UINT                     indexOffset = 0;
    UINT                     vertexStride = 0;
    UINT                     vertexOffset = 0;
    DXGI_FORMAT              indexFormat = DXGI_FORMAT_UNKNOWN;
    ID3D10InputLayout*       inputLayout = nullptr;
    ID3D10RenderTargetView*  rtvs[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D10DepthStencilView*  dsv = nullptr;

    void Capture(ID3D10Device1* dev) {
        dev->RSGetScissorRects(&scissorCount, scissors);
        dev->RSGetViewports(&viewportCount, viewports);
        dev->RSGetState(&rasterizer);
        dev->OMGetBlendState(&blend, blendFactor, &sampleMask);
        dev->OMGetDepthStencilState(&depthStencil, &stencilRef);
        dev->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        dev->PSGetShaderResources(0, 1, &psSrv);
        dev->PSGetSamplers(0, 1, &psSampler);
        dev->PSGetShader(&ps);
        dev->VSGetShader(&vs);
        dev->VSGetConstantBuffers(0, 1, &vsConstantBuffer);
        dev->GSGetShader(&gs);
        dev->IAGetPrimitiveTopology(&topology);
        dev->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        dev->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        dev->IAGetInputLayout(&inputLayout);
    }

    void Restore(ID3D10Device1* dev) {
        dev->RSSetScissorRects(scissorCount, scissors);
        dev->RSSetViewports(viewportCount, viewports);
        dev->RSSetState(rasterizer);                           SafeRelease(rasterizer);
        dev->OMSetBlendState(blend, blendFactor, sampleMask);  SafeRelease(blend);
        dev->OMSetDepthStencilState(depthStencil, stencilRef); SafeRelease(depthStencil);
        dev->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        for (auto& rtv : rtvs) SafeRelease(rtv);
        SafeRelease(dsv);
        dev->PSSetShaderResources(0, 1, &psSrv);               SafeRelease(psSrv);
        dev->PSSetSamplers(0, 1, &psSampler);                  SafeRelease(psSampler);
        dev->PSSetShader(ps);                                  SafeRelease(ps);
        dev->VSSetShader(vs);                                  SafeRelease(vs);
        dev->VSSetConstantBuffers(0, 1, &vsConstantBuffer);    SafeRelease(vsConstantBuffer);
        dev->GSSetShader(gs);                                  SafeRelease(gs);
        dev->IASetPrimitiveTopology(topology);
        dev->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset); SafeRelease(indexBuffer);
        dev->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        SafeRelease(vertexBuffer);
        dev->IASetInputLayout(inputLayout);                    SafeRelease(inputLayout);
    }
};

} // namespace

bool D3D10Renderer::EnsureInitialized(IDXGISwapChain* swapChain, SharedState* state) {
    if (initialized_) return true;
    if (pipelineFailed_) return false;

    // The keyed mutex on a legacy-shared resource is only reachable through the
    // 10.1 runtime. A pure 10.0 game fails this QI, which is exactly how we
    // decline to draw rather than crash inside it.
    ID3D10Device* device = nullptr;
    HRESULT hr = swapChain->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        OVERLAY_LOG_ONCE("GetDevice(ID3D10Device) failed: 0x%08lX", static_cast<unsigned long>(hr));
        pipelineFailed_ = true;
        return false;
    }

    hr = device->QueryInterface(IID_PPV_ARGS(&device_));
    device->Release();
    if (FAILED(hr) || !device_) {
        OVERLAY_LOG("ID3D10Device1 unavailable; keyed-mutex shared textures require D3D10.1");
        pipelineFailed_ = true;
        return false;
    }

    if (!CreatePipeline()) {
        pipelineFailed_ = true;
        return false;
    }

    // Publish the adapter and backbuffer details the host needs to match us.
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                LARGE_INTEGER luid{};
                luid.LowPart = desc.AdapterLuid.LowPart;
                luid.HighPart = desc.AdapterLuid.HighPart;
                state->adapterLuid = static_cast<uint64_t>(luid.QuadPart);
                OVERLAY_LOG("D3D10 game adapter: %ls (LUID 0x%016llX)", desc.Description,
                            static_cast<unsigned long long>(state->adapterLuid));
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    if (SUCCEEDED(swapChain->GetDesc(&scd))) {
        state->gameHwnd = reinterpret_cast<uint64_t>(scd.OutputWindow);
        state->backbufferIsSrgb = IsSrgbFormat(scd.BufferDesc.Format) ? 1u : 0u;
    }

    initialized_ = true;
    OVERLAY_LOG("D3D10 renderer initialized");
    return true;
}

bool D3D10Renderer::CreatePipeline() {
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                            "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        OVERLAY_LOG("overlay VS compile failed: %s",
                    errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "?");
        SafeRelease(errBlob);
        return false;
    }
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        OVERLAY_LOG("overlay PS compile failed: %s",
                    errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "?");
        SafeRelease(errBlob);
        SafeRelease(vsBlob);
        return false;
    }

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &vs_);
    if (SUCCEEDED(hr)) {
        hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), &ps_);
    }
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    if (FAILED(hr)) {
        OVERLAY_LOG("CreateShader failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    D3D10_SAMPLER_DESC sd{};
    sd.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D10_COMPARISON_NEVER;
    sd.MaxLOD = D3D10_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;

    // Premultiplied alpha over the game's opaque backbuffer. D3D10's blend desc
    // carries per-target enables in an array rather than a per-target struct.
    D3D10_BLEND_DESC bd{};
    bd.BlendEnable[0] = TRUE;
    bd.SrcBlend = D3D10_BLEND_ONE;
    bd.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
    bd.BlendOp = D3D10_BLEND_OP_ADD;
    bd.SrcBlendAlpha = D3D10_BLEND_ONE;
    bd.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
    bd.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    bd.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &blend_))) return false;

    D3D10_RASTERIZER_DESC rd{};
    rd.FillMode = D3D10_FILL_SOLID;
    rd.CullMode = D3D10_CULL_NONE;      // the fullscreen triangle has no meaningful winding
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) return false;

    D3D10_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = FALSE;
    if (FAILED(device_->CreateDepthStencilState(&dd, &depth_))) return false;

    return true;
}

bool D3D10Renderer::OpenSharedTexture(SharedState* state) {
    const uint64_t handle = state->sharedHandle;
    if (handle == 0) return false;

    const uint32_t width = state->texWidth;
    const uint32_t height = state->texHeight;
    if (width == 0 || height == 0) return false;

    if (sharedTex_ && handle == openedHandle_ &&
        width == openedTexWidth_ && height == openedTexHeight_) {
        return true;
    }

    ReleaseSharedTexture();

    // Legacy shared handle: the value the host published is valid in this
    // process directly (unlike an NT handle, it needs no duplication), and
    // ID3D10Device::OpenSharedResource is the D3D10 way in.
    ID3D10Texture2D* tex = nullptr;
    HRESULT hr = device_->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&tex));
    if (FAILED(hr) || !tex) {
        OVERLAY_LOG_ONCE("OpenSharedResource(0x%llX) failed: 0x%08lX",
                         static_cast<unsigned long long>(handle), static_cast<unsigned long>(hr));
        return false;
    }

    hr = tex->QueryInterface(IID_PPV_ARGS(&sharedMutex_));
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("shared texture has no IDXGIKeyedMutex: 0x%08lX", static_cast<unsigned long>(hr));
        tex->Release();
        return false;
    }
    sharedTex_ = tex;

    D3D10_TEXTURE2D_DESC desc{};
    sharedTex_->GetDesc(&desc);

    D3D10_TEXTURE2D_DESC pd{};
    pd.Width = desc.Width;
    pd.Height = desc.Height;
    pd.MipLevels = 1;
    pd.ArraySize = 1;
    pd.Format = desc.Format;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D10_USAGE_DEFAULT;
    pd.BindFlags = D3D10_BIND_SHADER_RESOURCE;

    hr = device_->CreateTexture2D(&pd, nullptr, &privateTex_);
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("private texture creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        ReleaseSharedTexture();
        return false;
    }

    D3D10_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = StripSrgb(desc.Format);
    srvd.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;

    hr = device_->CreateShaderResourceView(privateTex_, &srvd, &privateSrv_);
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("private SRV creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        ReleaseSharedTexture();
        return false;
    }

    openedHandle_ = handle;
    openedTexWidth_ = width;
    openedTexHeight_ = height;
    privateTexHasContent_ = false;
    OVERLAY_LOG("opened shared texture %ux%u fmt=%d", desc.Width, desc.Height, static_cast<int>(desc.Format));
    return true;
}

bool D3D10Renderer::EnsureRenderTarget(IDXGISwapChain* swapChain) {
    ID3D10Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        OVERLAY_LOG_ONCE("GetBuffer failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    if (rtv_ && backBuffer == rtvBackbuffer_) {
        backBuffer->Release();
        return true;
    }

    SafeRelease(rtv_);
    SafeRelease(rtvBackbuffer_);

    D3D10_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    D3D10_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = StripSrgb(desc.Format);
    rtvd.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE2D;

    hr = device_->CreateRenderTargetView(backBuffer, &rtvd, &rtv_);
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("CreateRenderTargetView failed: 0x%08lX", static_cast<unsigned long>(hr));
        backBuffer->Release();
        return false;
    }

    rtvBackbuffer_ = backBuffer;
    return true;
}

void D3D10Renderer::Render(IDXGISwapChain* swapChain, SharedState* state) {
    if (!OpenSharedTexture(state)) return;

    // Service the mutex even while hidden, for the same reason as D3D11: the
    // host acquires at key 0 and we release back to 0, so ceasing to consume
    // would wedge the producer permanently.
    const bool visible = state->visible != 0;
    HRESULT hr = sharedMutex_->AcquireSync(1, 0);   // zero timeout: never block the game
    if (hr == S_OK || hr == static_cast<HRESULT>(WAIT_ABANDONED)) {
        if (hr == S_OK && visible) {
            device_->CopyResource(privateTex_, sharedTex_);
            privateTexHasContent_ = true;
        } else if (hr != S_OK) {
            OVERLAY_LOG_ONCE("keyed mutex was abandoned by the host; reclaiming it");
        }
        sharedMutex_->ReleaseSync(0);
    } else if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->mutexTimeoutCount));
    } else {
        OVERLAY_LOG_ONCE("AcquireSync failed: 0x%08lX", static_cast<unsigned long>(hr));
        return;
    }

    if (!visible || !privateTexHasContent_) {
        OVERLAY_LOG_ONCE("not drawing: visible=%u hasContent=%u",
                         state->visible, privateTexHasContent_ ? 1u : 0u);
        return;
    }
    if (!EnsureRenderTarget(swapChain)) return;

    D3D10_TEXTURE2D_DESC bbDesc{};
    rtvBackbuffer_->GetDesc(&bbDesc);

    StateBackup backup;
    backup.Capture(device_);

    // D3D10 viewports use integer extents, unlike D3D11's floats.
    D3D10_VIEWPORT vp{};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = bbDesc.Width;
    vp.Height = bbDesc.Height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    device_->OMSetRenderTargets(1, &rtv_, nullptr);
    device_->RSSetViewports(1, &vp);
    device_->RSSetState(raster_);
    device_->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    device_->OMSetDepthStencilState(depth_, 0);
    device_->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    device_->IASetInputLayout(nullptr);
    device_->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    device_->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    device_->VSSetShader(vs_);
    device_->PSSetShader(ps_);
    device_->PSSetShaderResources(0, 1, &privateSrv_);
    device_->PSSetSamplers(0, 1, &sampler_);
    device_->GSSetShader(nullptr);

    device_->Draw(3, 0);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->drawCount));

    backup.Restore(device_);
    // No Flush(), for the same reason as every other backend: it would impose a
    // sync point on the game's frame.
}

void D3D10Renderer::OnResizeBuffers() {
    SafeRelease(rtv_);
    SafeRelease(rtvBackbuffer_);
}

void D3D10Renderer::ReleaseSharedTexture() {
    SafeRelease(privateSrv_);
    SafeRelease(privateTex_);
    SafeRelease(sharedMutex_);
    SafeRelease(sharedTex_);
    openedHandle_ = 0;
    openedTexWidth_ = 0;
    openedTexHeight_ = 0;
    privateTexHasContent_ = false;
}

void D3D10Renderer::Shutdown() {
    ReleaseSharedTexture();
    SafeRelease(rtv_);
    SafeRelease(rtvBackbuffer_);
    SafeRelease(depth_);
    SafeRelease(raster_);
    SafeRelease(blend_);
    SafeRelease(sampler_);
    SafeRelease(ps_);
    SafeRelease(vs_);
    SafeRelease(device_);
    initialized_ = false;
}

} // namespace overlay

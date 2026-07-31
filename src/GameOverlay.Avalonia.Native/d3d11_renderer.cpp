#include "d3d11_renderer.h"

#include <d3dcompiler.h>
#include "log.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace overlay {
namespace {

// A fullscreen triangle generated entirely from SV_VertexID: no vertex buffer,
// no index buffer, no input layout. That keeps the state we have to disturb on
// the game's context to an absolute minimum.
//
// The overlay texture is premultiplied BGRA, so the blend is (ONE,
// INV_SRC_ALPHA) and the shader does no alpha maths of its own.
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

// We always render through a non-sRGB view of the backbuffer and write our
// already-sRGB-encoded UI bytes verbatim.
//
// This is deliberate and covers both cases correctly. Flip-model swapchains
// are forbidden from having an _SRGB backbuffer format, so a game that wants
// sRGB output creates a UNORM backbuffer and an _SRGB RTV over it - meaning
// the bits actually stored are already sRGB-encoded. Bitblt swapchains may
// have a true _SRGB format, and viewing it as UNORM likewise stores our bytes
// verbatim. Either way, matching raw bytes is what makes the overlay's colours
// come out identical to the host's.
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

// Full pipeline state save/restore. The game gets its context back exactly as
// it left it - anything less produces corruption that only shows up in one
// game out of ten and is miserable to diagnose.
//
// This lives on the stack (roughly 7 KB) rather than the heap because the
// Present hook must not allocate.
struct StateBackup {
    UINT                        scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT                        viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT                  scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_VIEWPORT              viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RasterizerState*      rasterizer = nullptr;
    ID3D11BlendState*           blend = nullptr;
    FLOAT                       blendFactor[4]{};
    UINT                        sampleMask = 0;
    UINT                        stencilRef = 0;
    ID3D11DepthStencilState*    depthStencil = nullptr;
    ID3D11ShaderResourceView*   psSrv = nullptr;
    ID3D11SamplerState*         psSampler = nullptr;
    ID3D11PixelShader*          ps = nullptr;
    ID3D11VertexShader*         vs = nullptr;
    ID3D11GeometryShader*       gs = nullptr;
    UINT                        psInstanceCount = 256;
    UINT                        vsInstanceCount = 256;
    UINT                        gsInstanceCount = 256;
    ID3D11ClassInstance*        psInstances[256]{};
    ID3D11ClassInstance*        vsInstances[256]{};
    ID3D11ClassInstance*        gsInstances[256]{};
    D3D11_PRIMITIVE_TOPOLOGY    topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*               indexBuffer = nullptr;
    ID3D11Buffer*               vertexBuffer = nullptr;
    ID3D11Buffer*               vsConstantBuffer = nullptr;
    UINT                        indexOffset = 0;
    UINT                        vertexStride = 0;
    UINT                        vertexOffset = 0;
    DXGI_FORMAT                 indexFormat = DXGI_FORMAT_UNKNOWN;
    ID3D11InputLayout*          inputLayout = nullptr;
    ID3D11RenderTargetView*     rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView*     dsv = nullptr;

    void Capture(ID3D11DeviceContext* ctx) {
        ctx->RSGetScissorRects(&scissorCount, scissors);
        ctx->RSGetViewports(&viewportCount, viewports);
        ctx->RSGetState(&rasterizer);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&depthStencil, &stencilRef);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        ctx->PSGetShaderResources(0, 1, &psSrv);
        ctx->PSGetSamplers(0, 1, &psSampler);
        ctx->PSGetShader(&ps, psInstances, &psInstanceCount);
        ctx->VSGetShader(&vs, vsInstances, &vsInstanceCount);
        ctx->VSGetConstantBuffers(0, 1, &vsConstantBuffer);
        ctx->GSGetShader(&gs, gsInstances, &gsInstanceCount);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        ctx->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        ctx->IAGetInputLayout(&inputLayout);
    }

    void Restore(ID3D11DeviceContext* ctx) {
        ctx->RSSetScissorRects(scissorCount, scissors);
        ctx->RSSetViewports(viewportCount, viewports);
        ctx->RSSetState(rasterizer);                          SafeRelease(rasterizer);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);  SafeRelease(blend);
        ctx->OMSetDepthStencilState(depthStencil, stencilRef); SafeRelease(depthStencil);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        for (auto& rtv : rtvs) SafeRelease(rtv);
        SafeRelease(dsv);
        ctx->PSSetShaderResources(0, 1, &psSrv);              SafeRelease(psSrv);
        ctx->PSSetSamplers(0, 1, &psSampler);                 SafeRelease(psSampler);
        ctx->PSSetShader(ps, psInstances, psInstanceCount);   SafeRelease(ps);
        for (UINT i = 0; i < psInstanceCount; ++i) SafeRelease(psInstances[i]);
        ctx->VSSetShader(vs, vsInstances, vsInstanceCount);   SafeRelease(vs);
        for (UINT i = 0; i < vsInstanceCount; ++i) SafeRelease(vsInstances[i]);
        ctx->VSSetConstantBuffers(0, 1, &vsConstantBuffer);   SafeRelease(vsConstantBuffer);
        ctx->GSSetShader(gs, gsInstances, gsInstanceCount);   SafeRelease(gs);
        for (UINT i = 0; i < gsInstanceCount; ++i) SafeRelease(gsInstances[i]);
        ctx->IASetPrimitiveTopology(topology);
        ctx->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset); SafeRelease(indexBuffer);
        ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        SafeRelease(vertexBuffer);
        ctx->IASetInputLayout(inputLayout);                   SafeRelease(inputLayout);
    }
};

} // namespace

bool D3D11Renderer::EnsureInitialized(IDXGISwapChain* swapChain, SharedState* state) {
    if (initialized_) return true;
    if (pipelineFailed_) return false;

    HRESULT hr = swapChain->GetDevice(IID_PPV_ARGS(&device_));
    if (FAILED(hr) || !device_) {
        // A D3D12 swapchain will fail this QI. That is the expected way we
        // detect an unsupported title rather than misbehaving inside it.
        OVERLAY_LOG_ONCE("GetDevice(ID3D11Device) failed: 0x%08lX - not a D3D11 swapchain "
                         "(D3D12 is not supported in this iteration)", static_cast<unsigned long>(hr));
        pipelineFailed_ = true;
        return false;
    }

    device_->GetImmediateContext(&context_);
    device_->QueryInterface(IID_PPV_ARGS(&device1_));   // optional; needed for OpenSharedResource1

    if (!device1_) {
        OVERLAY_LOG("ID3D11Device1 unavailable; NT-handle shared textures require D3D11.1");
        pipelineFailed_ = true;
        return false;
    }

    if (!CreatePipeline()) {
        pipelineFailed_ = true;
        return false;
    }

    // Publish what the host needs to match our adapter and size.
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
                OVERLAY_LOG("game adapter: %ls (LUID 0x%016llX)", desc.Description,
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
    OVERLAY_LOG("D3D11 renderer initialized");
    return true;
}

bool D3D11Renderer::CreatePipeline() {
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

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) {
        hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);
    }
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    if (FAILED(hr)) {
        OVERLAY_LOG("CreateShader failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;

    // Premultiplied alpha over the game's already-opaque backbuffer.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &blend_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;      // the fullscreen triangle has no meaningful winding
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) return false;

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = FALSE;
    if (FAILED(device_->CreateDepthStencilState(&dd, &depth_))) return false;

    return true;
}

bool D3D11Renderer::OpenSharedTexture(SharedState* state) {
    const uint64_t handle = state->sharedHandle;
    if (handle == 0) return false;

    const uint32_t width = state->texWidth;
    const uint32_t height = state->texHeight;
    if (width == 0 || height == 0) return false;

    // Re-open only when the host actually republishes (resize, restart).
    if (sharedTex_ && handle == openedHandle_ &&
        width == openedTexWidth_ && height == openedTexHeight_) {
        return true;
    }

    ReleaseSharedTexture();

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device1_->OpenSharedResource1(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&tex));
    if (FAILED(hr) || !tex) {
        // Almost always either a duplicated-handle problem or the host having
        // created its device on a different adapter than ours.
        OVERLAY_LOG_ONCE("OpenSharedResource1(0x%llX) failed: 0x%08lX",
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

    D3D11_TEXTURE2D_DESC desc{};
    sharedTex_->GetDesc(&desc);

    // Private copy: same format, plain default usage, shader-readable. Copying
    // into this while holding the mutex - rather than sampling the shared
    // texture directly - is what lets a busy mutex degrade to "redraw the last
    // frame" instead of "flicker" or "block the game".
    D3D11_TEXTURE2D_DESC pd{};
    pd.Width = desc.Width;
    pd.Height = desc.Height;
    pd.MipLevels = 1;
    pd.ArraySize = 1;
    pd.Format = desc.Format;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = device_->CreateTexture2D(&pd, nullptr, &privateTex_);
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("private texture creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        ReleaseSharedTexture();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = StripSrgb(desc.Format);
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
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

bool D3D11Renderer::EnsureRenderTarget(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        OVERLAY_LOG_ONCE("GetBuffer failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    if (rtv_ && backBuffer == rtvBackbuffer_) {
        backBuffer->Release();
        return true;    // cache hit, which is the common case every frame
    }

    SafeRelease(rtv_);
    SafeRelease(rtvBackbuffer_);

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = StripSrgb(desc.Format);
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    hr = device_->CreateRenderTargetView(backBuffer, &rtvd, &rtv_);
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("CreateRenderTargetView failed: 0x%08lX", static_cast<unsigned long>(hr));
        backBuffer->Release();
        return false;
    }

    rtvBackbuffer_ = backBuffer;   // keep the ref; it is our cache key
    return true;
}

void D3D11Renderer::Render(IDXGISwapChain* swapChain, SharedState* state) {
    if (!OpenSharedTexture(state)) return;

    // Service the keyed mutex even while hidden. The host acquires at key 0 and
    // we release back to key 0, so if we stopped consuming, the host's producer
    // would never be able to acquire again and would wedge permanently.
    const bool visible = state->visible != 0;
    HRESULT hr = sharedMutex_->AcquireSync(1, 0);   // zero timeout: never block the game
    if (hr == S_OK || hr == static_cast<HRESULT>(WAIT_ABANDONED)) {
        // WAIT_ABANDONED means the host died holding the mutex. We now own it,
        // so releasing is mandatory - bailing out here would strand the mutex
        // and permanently wedge the overlay. The texture may be half-written,
        // so keep the previous frame rather than copying it.
        if (hr == S_OK && visible) {
            context_->CopyResource(privateTex_, sharedTex_);
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

    D3D11_TEXTURE2D_DESC bbDesc{};
    rtvBackbuffer_->GetDesc(&bbDesc);

    StateBackup backup;
    backup.Capture(context_);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f,
                       static_cast<float>(bbDesc.Width), static_cast<float>(bbDesc.Height),
                       0.0f, 1.0f };
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_);
    context_->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(depth_, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context_->VSSetShader(vs_, nullptr, 0);
    context_->PSSetShader(ps_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &privateSrv_);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->GSSetShader(nullptr, nullptr, 0);

    context_->Draw(3, 0);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->drawCount));

    backup.Restore(context_);
    // Deliberately no Flush() here: forcing a submit would add a CPU/GPU sync
    // point to the game's frame, which is exactly the cost we promised not to
    // impose.
}

void D3D11Renderer::OnResizeBuffers() {
    // The backbuffer is about to be recreated, so our cached RTV (and the ref
    // we hold on the old backbuffer, which would block the resize) must go.
    SafeRelease(rtv_);
    SafeRelease(rtvBackbuffer_);
}

void D3D11Renderer::ReleaseSharedTexture() {
    SafeRelease(privateSrv_);
    SafeRelease(privateTex_);
    SafeRelease(sharedMutex_);
    SafeRelease(sharedTex_);
    openedHandle_ = 0;
    openedTexWidth_ = 0;
    openedTexHeight_ = 0;
    privateTexHasContent_ = false;
}

void D3D11Renderer::Shutdown() {
    ReleaseSharedTexture();
    SafeRelease(rtv_);
    SafeRelease(rtvBackbuffer_);
    SafeRelease(depth_);
    SafeRelease(raster_);
    SafeRelease(blend_);
    SafeRelease(sampler_);
    SafeRelease(ps_);
    SafeRelease(vs_);
    SafeRelease(context_);
    SafeRelease(device1_);
    SafeRelease(device_);
    initialized_ = false;
}

} // namespace overlay

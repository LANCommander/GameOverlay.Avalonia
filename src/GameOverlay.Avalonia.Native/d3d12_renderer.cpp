#include "d3d12_renderer.h"

#include <d3dcompiler.h>
#include "log.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace overlay {
namespace {

// Same fullscreen triangle as the D3D11 compositor: generated from
// SV_VertexID, no vertex buffer, no input layout.
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

DXGI_FORMAT StripSrgb(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:                              return format;
    }
}

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

} // namespace

bool D3D12Renderer::EnsureInitialized(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue,
                                      SharedState* state) {
    if (initialized_) return true;
    if (pipelineFailed_ || !queue) return false;

    HRESULT hr = swapChain->GetDevice(IID_PPV_ARGS(&device_));
    if (FAILED(hr) || !device_) {
        OVERLAY_LOG_ONCE("GetDevice(ID3D12Device) failed: 0x%08lX", static_cast<unsigned long>(hr));
        pipelineFailed_ = true;
        return false;
    }

    // GetCurrentBackBufferIndex is on IDXGISwapChain3 and there is no other way
    // to know which buffer Present is about to use.
    hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3_));
    if (FAILED(hr) || !swapChain3_) {
        OVERLAY_LOG_ONCE("QI IDXGISwapChain3 failed: 0x%08lX", static_cast<unsigned long>(hr));
        pipelineFailed_ = true;
        return false;
    }

    queue_ = queue;
    queue_->AddRef();

    if (!AcquireBackBuffers(swapChain) || !CreatePipeline()) {
        pipelineFailed_ = true;
        return false;
    }

    state->graphicsApi = kGraphicsApiD3D12;
    initialized_ = true;
    OVERLAY_LOG("D3D12 renderer initialized (%u buffers)", bufferCount_);
    return true;
}

bool D3D12Renderer::AcquireBackBuffers(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return false;

    bufferCount_ = desc.BufferCount;
    if (bufferCount_ == 0 || bufferCount_ > kMaxBuffers) {
        OVERLAY_LOG("unsupported swapchain buffer count %u", bufferCount_);
        return false;
    }
    rtvFormat_ = StripSrgb(desc.BufferDesc.Format);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = bufferCount_;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)))) {
        OVERLAY_LOG("CreateDescriptorHeap(RTV) failed");
        return false;
    }
    rtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = rtvFormat_;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])))) {
            OVERLAY_LOG("GetBuffer(%u) failed", i);
            return false;
        }
        device_->CreateRenderTargetView(backBuffers_[i], &rtvDesc, handle);
        handle.ptr += rtvSize_;

        if (!allocators_[i] &&
            FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&allocators_[i])))) {
            OVERLAY_LOG("CreateCommandAllocator(%u) failed", i);
            return false;
        }
        fenceValues_[i] = 0;
    }
    return true;
}

bool D3D12Renderer::EnsureFences(SharedState* state) {
    // Our own fence, published for the host to open. It tells the host when the
    // GPU has finished reading a frame, so it knows when the texture is free to
    // overwrite - the half of the keyed-mutex handshake D3D12 cannot express.
    if (!consumeFence_) {
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&consumeFence_)))) {
            OVERLAY_LOG_ONCE("CreateFence(SHARED) failed");
            return false;
        }

        HANDLE handle = nullptr;
        HRESULT hr = device_->CreateSharedHandle(consumeFence_, nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr) || !handle) {
            OVERLAY_LOG_ONCE("CreateSharedHandle(consume fence) failed: 0x%08lX",
                             static_cast<unsigned long>(hr));
            return false;
        }
        // Valid in this process; the host duplicates it out.
        state->consumeFenceHandle = reinterpret_cast<uint64_t>(handle);
        OVERLAY_LOG("published consume fence handle 0x%llX",
                    static_cast<unsigned long long>(state->consumeFenceHandle));
    }

    const uint64_t produceHandle = state->produceFenceHandle;
    if (produceHandle == 0) return false;

    if (!produceFence_ || produceHandle != openedProduceHandle_) {
        if (produceFence_) { produceFence_->Release(); produceFence_ = nullptr; }

        HRESULT hr = device_->OpenSharedHandle(reinterpret_cast<HANDLE>(produceHandle),
                                               IID_PPV_ARGS(&produceFence_));
        if (FAILED(hr) || !produceFence_) {
            OVERLAY_LOG_ONCE("OpenSharedHandle(produce fence) failed: 0x%08lX",
                             static_cast<unsigned long>(hr));
            return false;
        }
        openedProduceHandle_ = produceHandle;
        OVERLAY_LOG("opened host produce fence");
    }
    return true;
}

bool D3D12Renderer::EnsureSharedTexture(SharedState* state) {
    const uint64_t handle = state->sharedHandle;
    if (handle == 0) return false;
    if (sharedTexture_ && handle == openedHandle_) return true;

    ReleaseSharedTexture();

    HRESULT hr = device_->OpenSharedHandle(reinterpret_cast<HANDLE>(handle),
                                           IID_PPV_ARGS(&sharedTexture_));
    if (FAILED(hr) || !sharedTexture_) {
        // Cross-API sharing requires the host to have created the texture
        // without a keyed mutex; D3D12 refuses to open one that has it.
        OVERLAY_LOG_ONCE("OpenSharedHandle(texture 0x%llX) failed: 0x%08lX",
                         static_cast<unsigned long long>(handle), static_cast<unsigned long>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)))) {
        OVERLAY_LOG_ONCE("CreateDescriptorHeap(SRV) failed");
        ReleaseSharedTexture();
        return false;
    }

    const D3D12_RESOURCE_DESC desc = sharedTexture_->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = StripSrgb(desc.Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(sharedTexture_, &srv,
                                      srvHeap_->GetCPUDescriptorHandleForHeapStart());

    openedHandle_ = handle;
    lastConsumedValue_ = 0;
    OVERLAY_LOG("opened shared texture %llux%u fmt=%d",
                static_cast<unsigned long long>(desc.Width), desc.Height,
                static_cast<int>(desc.Format));
    return true;
}

void D3D12Renderer::ReleaseSharedTexture() {
    SafeRelease(srvHeap_);
    SafeRelease(sharedTexture_);
    openedHandle_ = 0;
}

bool D3D12Renderer::CreatePipeline() {
    // One SRV in a descriptor table, plus a static sampler so no sampler heap
    // is needed.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &range;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &rootParam;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* signature = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &signature, &error);
    if (FAILED(hr)) {
        OVERLAY_LOG("D3D12SerializeRootSignature failed: %s",
                    error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        SafeRelease(error);
        return false;
    }
    SafeRelease(error);

    hr = device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                      IID_PPV_ARGS(&rootSignature_));
    SafeRelease(signature);
    if (FAILED(hr)) {
        OVERLAY_LOG("CreateRootSignature failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                    "VSMain", "vs_5_0", 0, 0, &vs, &error);
    if (FAILED(hr)) {
        OVERLAY_LOG("overlay VS compile failed: %s",
                    error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        SafeRelease(error);
        return false;
    }
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", 0, 0, &ps, &error);
    if (FAILED(hr)) {
        OVERLAY_LOG("overlay PS compile failed: %s",
                    error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        SafeRelease(error);
        SafeRelease(vs);
        return false;
    }
    SafeRelease(error);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    // Premultiplied alpha, matching the D3D11 path and the host's output.
    auto& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvFormat_;
    pso.SampleDesc.Count = 1;

    hr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState_));
    SafeRelease(vs);
    SafeRelease(ps);
    if (FAILED(hr)) {
        OVERLAY_LOG("CreateGraphicsPipelineState failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0],
                                    pipelineState_, IID_PPV_ARGS(&commandList_));
    if (FAILED(hr)) {
        OVERLAY_LOG("CreateCommandList failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    commandList_->Close();

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        OVERLAY_LOG("CreateFence failed");
        return false;
    }

    return true;
}

void D3D12Renderer::Render(IDXGISwapChain* swapChain, SharedState* state) {
    if (!state->visible) return;

    // A resize released the buffers and their descriptor heap; rebuild them
    // against the new ones before drawing.
    if (!rtvHeap_ && !AcquireBackBuffers(swapChain)) return;

    if (!EnsureFences(state)) return;
    if (!EnsureSharedTexture(state)) return;

    // Only draw a frame the host has finished writing. Polling the fence rather
    // than issuing queue->Wait keeps this strictly non-blocking: a GPU wait
    // would sit in the game's queue and delay the game's own work too.
    const uint64_t ready = state->produceFenceValue;
    if (ready == 0 || produceFence_->GetCompletedValue() < ready) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->mutexTimeoutCount));
        return;
    }

    const uint32_t index = swapChain3_->GetCurrentBackBufferIndex();
    if (index >= bufferCount_ || !backBuffers_[index]) return;

    // Never block the game's render thread. If the GPU has not finished with
    // this allocator yet, skip the overlay for one frame instead of waiting.
    if (fenceValues_[index] != 0 && fence_->GetCompletedValue() < fenceValues_[index]) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->mutexTimeoutCount));
        return;
    }

    if (FAILED(allocators_[index]->Reset())) return;
    if (FAILED(commandList_->Reset(allocators_[index], pipelineState_))) return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backBuffers_[index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // The game has already transitioned back to PRESENT by the time Present is
    // called, so that is the state we inherit.
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * rtvSize_;
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_RESOURCE_DESC desc = backBuffers_[index]->GetDesc();
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f,
                             static_cast<float>(desc.Width), static_cast<float>(desc.Height),
                             0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);

    commandList_->SetGraphicsRootSignature(rootSignature_);
    ID3D12DescriptorHeap* heaps[] = { srvHeap_ };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootDescriptorTable(0, srvHeap_->GetGPUDescriptorHandleForHeapStart());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);

    // Hand the buffer back exactly as we found it, or Present fails.
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &barrier);

    if (FAILED(commandList_->Close())) return;

    ID3D12CommandList* lists[] = { commandList_ };
    queue_->ExecuteCommandLists(1, lists);

    // Record when this allocator becomes reusable. Signalling on the game's
    // queue costs it nothing measurable and needs no CPU wait.
    fenceValues_[index] = ++fenceCounter_;
    queue_->Signal(fence_, fenceValues_[index]);

    // Tell the host the GPU has finished reading this frame, so it knows the
    // texture is free to overwrite.
    if (ready != lastConsumedValue_) {
        lastConsumedValue_ = ready;
        queue_->Signal(consumeFence_, ready);
    }

    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->drawCount));
}

void D3D12Renderer::OnResizeBuffers() {
    // Our references would make ResizeBuffers fail, and any in-flight work
    // against the old buffers must be allowed to finish first.
    if (fence_ && fenceCounter_ > 0) {
        if (fence_->GetCompletedValue() < fenceCounter_) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event) {
                if (SUCCEEDED(fence_->SetEventOnCompletion(fenceCounter_, event))) {
                    WaitForSingleObject(event, 1000);
                }
                CloseHandle(event);
            }
        }
    }

    ReleaseBackBuffers();
    SafeRelease(rtvHeap_);
    for (auto& value : fenceValues_) value = 0;
}

void D3D12Renderer::ReleaseBackBuffers() {
    for (auto& buffer : backBuffers_) SafeRelease(buffer);
}

void D3D12Renderer::Shutdown() {
    OnResizeBuffers();

    ReleaseSharedTexture();
    SafeRelease(produceFence_);
    SafeRelease(consumeFence_);
    for (auto& allocator : allocators_) SafeRelease(allocator);
    SafeRelease(commandList_);
    SafeRelease(fence_);
    SafeRelease(pipelineState_);
    SafeRelease(rootSignature_);
    SafeRelease(rtvHeap_);
    SafeRelease(swapChain3_);
    SafeRelease(queue_);
    SafeRelease(device_);
    initialized_ = false;
}

} // namespace overlay

#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

#include "shared_state.h"

namespace overlay {

// Composites the overlay onto a D3D12 game's backbuffer.
//
// D3D12 makes this materially harder than D3D11:
//
//   * There is no immediate context. We record our own command list and submit
//     it on the game's queue, after the game's own work for that frame.
//   * The queue cannot be obtained from the swapchain - DXGI exposes no
//     accessor - so it has to be captured by hooking ExecuteCommandLists.
//   * The backbuffer is in PRESENT state when Present is called, so we must
//     transition it to RENDER_TARGET and back around our draw.
//   * Command allocators cannot be reset while the GPU is still consuming
//     them, so each backbuffer gets its own and we skip a frame rather than
//     block if one is still in flight.
class D3D12Renderer {
public:
    // Binds to the game's device, queue and swapchain. Safe to call every
    // frame; returns false until it can do useful work.
    bool EnsureInitialized(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, SharedState* state);

    void Render(IDXGISwapChain* swapChain, SharedState* state);

    // The swapchain buffers are about to be recreated.
    void OnResizeBuffers();

    void Shutdown();

    bool initialized() const { return initialized_; }

private:
    static constexpr uint32_t kMaxBuffers = 8;

    bool CreatePipeline();
    bool AcquireBackBuffers(IDXGISwapChain* swapChain);
    void ReleaseBackBuffers();
    bool EnsureSharedTexture(SharedState* state);
    bool EnsureFences(SharedState* state);
    void ReleaseSharedTexture();

    ID3D12Device*              device_ = nullptr;
    ID3D12CommandQueue*        queue_ = nullptr;
    IDXGISwapChain3*           swapChain3_ = nullptr;

    ID3D12RootSignature*       rootSignature_ = nullptr;
    ID3D12PipelineState*       pipelineState_ = nullptr;
    ID3D12DescriptorHeap*      rtvHeap_ = nullptr;
    uint32_t                   rtvSize_ = 0;

    ID3D12Resource*            backBuffers_[kMaxBuffers]{};
    ID3D12CommandAllocator*    allocators_[kMaxBuffers]{};
    uint64_t                   fenceValues_[kMaxBuffers]{};
    ID3D12GraphicsCommandList* commandList_ = nullptr;

    ID3D12Fence*               fence_ = nullptr;
    uint64_t                   fenceCounter_ = 0;

    // The host's texture, opened straight into D3D12. No private copy is
    // needed here: the fence pair guarantees the host is not writing while we
    // read, which is what the D3D11 path used a keyed mutex for.
    ID3D12Resource*            sharedTexture_ = nullptr;
    ID3D12DescriptorHeap*      srvHeap_ = nullptr;
    uint64_t                   openedHandle_ = 0;

    // produce: signalled by the host when a frame is ready.
    // consume: signalled by us once the GPU has finished reading it.
    ID3D12Fence*               produceFence_ = nullptr;
    ID3D12Fence*               consumeFence_ = nullptr;
    uint64_t                   openedProduceHandle_ = 0;
    uint64_t                   lastConsumedValue_ = 0;

    uint32_t                   bufferCount_ = 0;
    DXGI_FORMAT                rtvFormat_ = DXGI_FORMAT_UNKNOWN;

    bool initialized_ = false;
    bool pipelineFailed_ = false;
};

} // namespace overlay

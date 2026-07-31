#include "d3d9_renderer.h"

#include "log.h"

namespace overlay {
namespace {

// Fixed-function transformed-and-lit vertex: screen-space position with a
// pre-divided w, a diffuse colour (ignored by the texture stage but part of the
// FVF), and one texture coordinate.
struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

constexpr DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

} // namespace

bool D3D9Renderer::EnsureTexture(IDirect3DDevice9* device, uint32_t width, uint32_t height) {
    if (tex_ && width == texWidth_ && height == texHeight_) return true;
    if (width == 0 || height == 0) return false;

    SafeRelease(tex_);
    texHasContent_ = false;

    // A8R8G8B8 stored little-endian is B,G,R,A in memory, which matches the
    // host's premultiplied BGRA exactly, so the upload is a straight copy.
    // Dynamic + DEFAULT pool so LockRect with D3DLOCK_DISCARD never stalls on
    // the previous frame's draw.
    HRESULT hr = device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC,
                                       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex_, nullptr);
    if (FAILED(hr) || !tex_) {
        OVERLAY_LOG_ONCE("CreateTexture(%ux%u) failed: 0x%08lX", width, height,
                         static_cast<unsigned long>(hr));
        tex_ = nullptr;
        return false;
    }

    texWidth_ = width;
    texHeight_ = height;
    staging_.assign(static_cast<size_t>(width) * height * 4, 0);
    return true;
}

bool D3D9Renderer::OpenFrameMapping(SharedState* state) {
    const uint32_t generation = state->cpuFrameGeneration;
    if (generation == 0) return false;   // host has not published a frame yet

    if (framePixels_ && generation == openedGeneration_) return true;

    ReleaseFrameMapping();

    const uint32_t width = state->texWidth;
    const uint32_t height = state->texHeight;
    if (width == 0 || height == 0) return false;

    wchar_t name[80];
    FormatFrameMappingName(name, GetCurrentProcessId(), generation);

    frameMap_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!frameMap_) return false;   // host may not have created it yet; retry next frame

    const size_t bytes = static_cast<size_t>(width) * height * 4;
    framePixels_ = static_cast<const uint8_t*>(MapViewOfFile(frameMap_, FILE_MAP_READ, 0, 0, bytes));
    if (!framePixels_) {
        OVERLAY_LOG_ONCE("MapViewOfFile('%ls') failed: %lu", name, GetLastError());
        CloseHandle(frameMap_);
        frameMap_ = nullptr;
        return false;
    }

    openedGeneration_ = generation;
    OVERLAY_LOG("opened cpu frame mapping %ux%u gen %u", width, height, generation);
    return true;
}

void D3D9Renderer::ReleaseFrameMapping() {
    if (framePixels_) { UnmapViewOfFile(framePixels_); framePixels_ = nullptr; }
    if (frameMap_) { CloseHandle(frameMap_); frameMap_ = nullptr; }
    openedGeneration_ = 0;
}

bool D3D9Renderer::UploadFrame(SharedState* state) {
    if (!framePixels_ || staging_.empty()) return false;

    const size_t bytes = static_cast<size_t>(texWidth_) * texHeight_ * 4;
    if (staging_.size() < bytes) return false;

    // Seqlock read: an odd count means the host is mid-copy, and an unequal
    // count before and after means the frame was overwritten while we read it.
    // Either way we keep the previous texture rather than upload a torn frame -
    // never blocking, exactly like the keyed-mutex backends skip a busy frame.
    const uint32_t s1 = state->cpuFrameSeq;
    if (s1 & 1u) return texHasContent_;

    memcpy(staging_.data(), framePixels_, bytes);

    MemoryBarrier();
    const uint32_t s2 = state->cpuFrameSeq;
    if (s1 != s2) return texHasContent_;

    D3DLOCKED_RECT locked{};
    HRESULT hr = tex_->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("LockRect failed: 0x%08lX", static_cast<unsigned long>(hr));
        return texHasContent_;
    }

    const uint32_t rowBytes = texWidth_ * 4;
    auto* dst = static_cast<uint8_t*>(locked.pBits);
    const uint8_t* src = staging_.data();
    for (uint32_t y = 0; y < texHeight_; ++y) {
        memcpy(dst + static_cast<size_t>(y) * locked.Pitch,
               src + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    tex_->UnlockRect(0);

    texHasContent_ = true;
    return true;
}

void D3D9Renderer::DrawQuad(IDirect3DDevice9* device) {
    // Backbuffer dimensions define the quad; the overlay is authored at the same
    // size, so this is a 1:1 blit.
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) {
        return;
    }
    D3DSURFACE_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);
    backBuffer->Release();

    const float w = static_cast<float>(bbDesc.Width);
    const float h = static_cast<float>(bbDesc.Height);

    // The half-texel offset maps texel centres to pixel centres, the standard
    // D3D9 rule for pixel-accurate 2D blits.
    const Vertex verts[4] = {
        { -0.5f,     -0.5f,     0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { w - 0.5f,  -0.5f,     0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { -0.5f,     h - 0.5f,  0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { w - 0.5f,  h - 0.5f,  0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
    };

    // Capturing D3DSBT_ALL and applying it afterwards hands the game back every
    // render/sampler/texture-stage state we touch. Render targets are NOT part
    // of a state block, but we never change the target - we draw onto whatever
    // the game was already rendering to, which at EndScene is the backbuffer.
    IDirect3DStateBlock9* block = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &block)) || !block) return;

    device->SetPixelShader(nullptr);
    device->SetVertexShader(nullptr);
    device->SetFVF(kFVF);
    device->SetTexture(0, tex_);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);          // premultiplied
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(Vertex));

    block->Apply();
    block->Release();
}

void D3D9Renderer::Render(IDirect3DDevice9* device, SharedState* state) {
    if (state->visible == 0) return;
    if (!OpenFrameMapping(state)) return;
    if (!EnsureTexture(device, state->texWidth, state->texHeight)) return;

    UploadFrame(state);
    if (!texHasContent_) return;

    DrawQuad(device);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->drawCount));
}

void D3D9Renderer::OnDeviceLost() {
    // The dynamic texture lives in D3DPOOL_DEFAULT, so it must go before Reset.
    // The frame mapping is plain shared memory and survives untouched.
    SafeRelease(tex_);
    texWidth_ = 0;
    texHeight_ = 0;
    texHasContent_ = false;
}

void D3D9Renderer::Shutdown() {
    OnDeviceLost();
    ReleaseFrameMapping();
    staging_.clear();
    staging_.shrink_to_fit();
}

} // namespace overlay

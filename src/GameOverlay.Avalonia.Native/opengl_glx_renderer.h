// opengl_glx_renderer.h - composites the overlay onto a Linux GLX game's
// default framebuffer via the CPU shared-memory frame transport.
//
// This is the Linux counterpart of the Windows opengl_renderer, but it shares
// no code with it: the Windows path bridges through a D3D11 shared texture
// (WGL_NV_DX_interop2), which has no Linux analogue. Here the host publishes
// premultiplied BGRA pixels into a POSIX shm frame buffer (the same CpuFrame
// transport D3D9 uses on Windows), and this renderer uploads them into a GL
// texture and draws a fullscreen quad.
//
// It picks its draw path from the current GL context: a compatibility/legacy
// context uses fixed-function GL (glPushAttrib, glBegin, the matrix stacks - no
// shader plumbing); a core-profile context (GL >= 3.2 core), where none of those
// exist, uses a shader + VAO path with modern GL entry points resolved through
// glXGetProcAddress. Both draw the same premultiplied fullscreen quad.
#pragma once

#include <cstdint>

#include "shared_state.h"

namespace overlay {

class OpenGLGlxRenderer {
public:
    // Draws the overlay for the currently-current GL context. Safe to call every
    // swap; does nothing until the host has published a frame. `width`/`height`
    // are the drawable's pixel dimensions.
    void Render(SharedState* state, int width, int height);

    void Shutdown();

private:
    bool EnsureFrameMapping(SharedState* state);
    void ReleaseFrameMapping();
    bool ReadFrame(SharedState* state);

    void UploadTexture();
    void DrawFixedFunction(int width, int height);
    bool EnsureShaderPipeline();
    void DrawShader(int width, int height);

    unsigned int glTexture_ = 0;   // GLuint

    // Draw-path selection, decided once from the context profile.
    bool profileChecked_ = false;
    bool useShader_ = false;
    bool shaderReady_ = false;
    bool shaderFailed_ = false;

    // Core-profile pipeline objects (GLuint).
    unsigned int program_ = 0;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    int          uTexLocation_ = -1;

    // The host's CPU frame buffer, opened per generation.
    int          frameFd_ = -1;
    const uint8_t* framePixels_ = nullptr;
    std::size_t  frameBytes_ = 0;
    uint32_t     openedGeneration_ = 0;

    // Staging copy taken under the seqlock, then uploaded.
    uint8_t*     staging_ = nullptr;
    std::size_t  stagingBytes_ = 0;

    uint32_t     texWidth_ = 0;
    uint32_t     texHeight_ = 0;
    bool         hasContent_ = false;
};

} // namespace overlay

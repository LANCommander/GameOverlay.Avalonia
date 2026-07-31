#include "opengl_renderer.h"

#include <GL/gl.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <type_traits>

#include "log.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace overlay {
namespace {

// --- GL / WGL enums not in the 1.1 header --------------------------------
constexpr GLenum GL_FRAGMENT_SHADER_ = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER_ = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS_ = 0x8B81;
constexpr GLenum GL_LINK_STATUS_ = 0x8B82;
constexpr GLenum GL_TEXTURE0_ = 0x84C0;
constexpr GLenum GL_CLAMP_TO_EDGE_ = 0x812F;
constexpr GLenum GL_FRAMEBUFFER_SRGB_ = 0x8DB9;
constexpr GLenum GL_CURRENT_PROGRAM_ = 0x8B8D;
constexpr GLenum GL_VERTEX_ARRAY_BINDING_ = 0x85B5;
constexpr GLenum GL_ACTIVE_TEXTURE_ = 0x84E0;
constexpr GLenum GL_TEXTURE_BINDING_2D_ = 0x8069;
constexpr GLenum GL_SAMPLER_BINDING_ = 0x8919;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING_ = 0x8CA6;
constexpr GLenum GL_BLEND_ = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST_ = 0x0B71;
constexpr GLenum GL_CULL_FACE_ = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST_ = 0x0C11;
constexpr GLenum GL_FRAMEBUFFER_ = 0x8D40;
constexpr GLenum GL_VIEWPORT_ = 0x0BA2;

constexpr GLenum GL_BLEND_SRC_RGB_ = 0x80C9;
constexpr GLenum GL_BLEND_DST_RGB_ = 0x80C8;
constexpr GLenum GL_BLEND_SRC_ALPHA_ = 0x80CB;
constexpr GLenum GL_BLEND_DST_ALPHA_ = 0x80CA;
constexpr GLenum GL_FUNC_ADD_ = 0x8006;
constexpr GLenum GL_ONE_ = 1;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA_ = 0x0303;

constexpr int WGL_ACCESS_READ_ONLY_NV_ = 0x00000000;

using GLchar = char;
using GLsizeiptr = intptr_t;

// --- GL 2.0+ and interop entry points ------------------------------------
struct Gl {
    // shaders / program
    GLuint (APIENTRY* CreateShader)(GLenum);
    void   (APIENTRY* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void   (APIENTRY* CompileShader)(GLuint);
    void   (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint (APIENTRY* CreateProgram)(void);
    void   (APIENTRY* AttachShader)(GLuint, GLuint);
    void   (APIENTRY* LinkProgram)(GLuint);
    void   (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* UseProgram)(GLuint);
    void   (APIENTRY* DeleteShader)(GLuint);
    GLint  (APIENTRY* GetUniformLocation)(GLuint, const GLchar*);
    void   (APIENTRY* Uniform1i)(GLint, GLint);
    // vao / textures / samplers
    void   (APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void   (APIENTRY* BindVertexArray)(GLuint);
    void   (APIENTRY* ActiveTexture)(GLenum);
    void   (APIENTRY* GenSamplers)(GLsizei, GLuint*);
    void   (APIENTRY* BindSampler)(GLuint, GLuint);
    void   (APIENTRY* SamplerParameteri)(GLuint, GLenum, GLint);
    void   (APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    // WGL_NV_DX_interop2
    HANDLE (APIENTRY* DXOpenDeviceNV)(void*);
    BOOL   (APIENTRY* DXCloseDeviceNV)(HANDLE);
    HANDLE (APIENTRY* DXRegisterObjectNV)(HANDLE, void*, GLuint, GLenum, GLenum);
    BOOL   (APIENTRY* DXUnregisterObjectNV)(HANDLE, HANDLE);
    BOOL   (APIENTRY* DXLockObjectsNV)(HANDLE, GLint, HANDLE*);
    BOOL   (APIENTRY* DXUnlockObjectsNV)(HANDLE, GLint, HANDLE*);

    bool Load() {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(wglGetProcAddress(name));
            if (!fn) { OVERLAY_LOG("wglGetProcAddress(%s) failed", name); ok = false; }
        };
        get(CreateShader, "glCreateShader");
        get(ShaderSource, "glShaderSource");
        get(CompileShader, "glCompileShader");
        get(GetShaderiv, "glGetShaderiv");
        get(GetShaderInfoLog, "glGetShaderInfoLog");
        get(CreateProgram, "glCreateProgram");
        get(AttachShader, "glAttachShader");
        get(LinkProgram, "glLinkProgram");
        get(GetProgramiv, "glGetProgramiv");
        get(UseProgram, "glUseProgram");
        get(DeleteShader, "glDeleteShader");
        get(GetUniformLocation, "glGetUniformLocation");
        get(Uniform1i, "glUniform1i");
        get(GenVertexArrays, "glGenVertexArrays");
        get(BindVertexArray, "glBindVertexArray");
        get(ActiveTexture, "glActiveTexture");
        get(GenSamplers, "glGenSamplers");
        get(BindSampler, "glBindSampler");
        get(SamplerParameteri, "glSamplerParameteri");
        get(BlendFuncSeparate, "glBlendFuncSeparate");
        get(DXOpenDeviceNV, "wglDXOpenDeviceNV");
        get(DXCloseDeviceNV, "wglDXCloseDeviceNV");
        get(DXRegisterObjectNV, "wglDXRegisterObjectNV");
        get(DXUnregisterObjectNV, "wglDXUnregisterObjectNV");
        get(DXLockObjectsNV, "wglDXLockObjectsNV");
        get(DXUnlockObjectsNV, "wglDXUnlockObjectsNV");
        return ok;
    }
};

Gl gl;

// glGetIntegerv and the 1.1 core calls are available directly from opengl32.
const char* kVertexShader = R"(#version 330 core
out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    // GL samples with (0,0) at the bottom-left, but the overlay texture is
    // top-down, so flip V here rather than re-uploading flipped.
    gl_Position = vec4(vUV * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 vUV;
out vec4 outColor;
uniform sampler2D overlayTex;
void main() {
    // No channel swizzle: WGL_NV_DX_interop preserves the D3D texture's format
    // semantics, so a B8G8R8A8 texture already presents red as .r to the
    // shader. The source is premultiplied; the blend state does the composite.
    outColor = texture(overlayTex, vUV);
}
)";

template <typename T>
void SafeRelease(T*& ptr) { if (ptr) { ptr->Release(); ptr = nullptr; } }

// Snapshot of the GL state the composite pass disturbs, restored afterward so
// the game's next frame is unaffected. The GL equivalent of the D3D11
// StateBackup.
struct GlStateBackup {
    GLint program = 0, vao = 0, activeTexture = 0, texture2d = 0, sampler = 0, framebuffer = 0;
    GLboolean blend = 0, depthTest = 0, cullFace = 0, scissor = 0, srgb = 0;
    GLint viewport[4]{};
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0;

    void Capture() {
        glGetIntegerv(GL_CURRENT_PROGRAM_, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING_, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE_, &activeTexture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_, &texture2d);
        glGetIntegerv(GL_SAMPLER_BINDING_, &sampler);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_, &framebuffer);
        glGetIntegerv(GL_VIEWPORT_, viewport);
        glGetIntegerv(GL_BLEND_SRC_RGB_, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB_, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA_, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA_, &blendDstAlpha);
        blend = glIsEnabled(GL_BLEND_);
        depthTest = glIsEnabled(GL_DEPTH_TEST_);
        cullFace = glIsEnabled(GL_CULL_FACE_);
        scissor = glIsEnabled(GL_SCISSOR_TEST_);
        srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB_);
    }

    void Restore() {
        gl.UseProgram(static_cast<GLuint>(program));
        gl.BindVertexArray(static_cast<GLuint>(vao));
        // Restore the sampler on unit 0, then whatever unit was active.
        gl.ActiveTexture(GL_TEXTURE0_);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2d));
        gl.BindSampler(0, static_cast<GLuint>(sampler));
        gl.ActiveTexture(static_cast<GLenum>(activeTexture));
        Toggle(GL_BLEND_, blend);
        Toggle(GL_DEPTH_TEST_, depthTest);
        Toggle(GL_CULL_FACE_, cullFace);
        Toggle(GL_SCISSOR_TEST_, scissor);
        Toggle(GL_FRAMEBUFFER_SRGB_, srgb);
        gl.BlendFuncSeparate(static_cast<GLenum>(blendSrcRgb), static_cast<GLenum>(blendDstRgb),
                             static_cast<GLenum>(blendSrcAlpha), static_cast<GLenum>(blendDstAlpha));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    static void Toggle(GLenum cap, GLboolean on) {
        if (on) glEnable(cap); else glDisable(cap);
    }
};

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = gl.CreateShader(type);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        char logText[1024];
        gl.GetShaderInfoLog(shader, sizeof(logText), nullptr, logText);
        OVERLAY_LOG("overlay GL shader compile failed: %s", logText);
        return 0;
    }
    return shader;
}

} // namespace

bool OpenGLRenderer::EnsureFunctions() {
    if (functionsLoaded_) return true;
    if (!gl.Load()) return false;
    functionsLoaded_ = true;
    return true;
}

bool OpenGLRenderer::EnsureD3DDevice(SharedState* state) {
    if (d3dDevice_) return true;

    // Create the D3D11 device on the game's adapter, then open it for GL
    // interop. Both are needed: the texture is shared as a D3D resource, and
    // WGL_NV_DX_interop bridges that D3D device into the current GL context.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        uint64_t luid = (static_cast<uint64_t>(desc.AdapterLuid.HighPart) << 32) |
                        desc.AdapterLuid.LowPart;
        if (state->adapterLuid != 0 && luid == state->adapterLuid) { chosen = adapter; break; }
        adapter->Release();
    }
    factory->Release();

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &d3dDevice_, nullptr, &d3dContext_);
    if (chosen) chosen->Release();
    if (FAILED(hr)) {
        OVERLAY_LOG_ONCE("D3D11CreateDevice for GL interop failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    interopDevice_ = gl.DXOpenDeviceNV(d3dDevice_);
    if (!interopDevice_) {
        OVERLAY_LOG_ONCE("wglDXOpenDeviceNV failed");
        SafeRelease(d3dContext_);
        SafeRelease(d3dDevice_);
        return false;
    }
    OVERLAY_LOG("GL interop device opened");
    return true;
}

bool OpenGLRenderer::EnsurePipeline() {
    if (pipelineReady_) return true;

    GLuint vs = CompileShader(GL_VERTEX_SHADER_, kVertexShader);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER_, kFragmentShader);
    if (!vs || !fs) return false;

    program_ = gl.CreateProgram();
    gl.AttachShader(program_, vs);
    gl.AttachShader(program_, fs);
    gl.LinkProgram(program_);
    GLint linked = 0;
    gl.GetProgramiv(program_, GL_LINK_STATUS_, &linked);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!linked) { OVERLAY_LOG("overlay GL program link failed"); return false; }

    gl.GenVertexArrays(1, &vao_);

    gl.GenSamplers(1, &sampler_);
    gl.SamplerParameteri(sampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.SamplerParameteri(sampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.SamplerParameteri(sampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    gl.SamplerParameteri(sampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);

    pipelineReady_ = true;
    return true;
}

bool OpenGLRenderer::EnsureSharedTexture(SharedState* state) {
    const uint64_t handle = state->sharedHandle;
    const uint32_t width = state->texWidth;
    const uint32_t height = state->texHeight;
    if (handle == 0 || width == 0 || height == 0) return false;

    if (sharedTex_ && handle == openedHandle_ && width == texWidth_ && height == texHeight_) {
        return true;
    }

    ReleaseSharedTexture();

    ID3D11Device1* device1 = nullptr;
    if (FAILED(d3dDevice_->QueryInterface(IID_PPV_ARGS(&device1)))) return false;

    HRESULT hr = device1->OpenSharedResource1(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&sharedTex_));
    device1->Release();
    if (FAILED(hr) || !sharedTex_) {
        OVERLAY_LOG_ONCE("OpenSharedResource1(GL path) failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    if (FAILED(sharedTex_->QueryInterface(IID_PPV_ARGS(&sharedMutex_)))) {
        ReleaseSharedTexture();
        return false;
    }

    // Plain private copy target - no keyed mutex, which is exactly what
    // WGL_NV_DX_interop requires (it rejects keyed-mutex textures).
    D3D11_TEXTURE2D_DESC desc{};
    sharedTex_->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC pd{};
    pd.Width = desc.Width;
    pd.Height = desc.Height;
    pd.MipLevels = 1;
    pd.ArraySize = 1;
    pd.Format = desc.Format;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3dDevice_->CreateTexture2D(&pd, nullptr, &privateTex_))) {
        ReleaseSharedTexture();
        return false;
    }

    glGenTextures(1, &glTexture_);
    interopObject_ = gl.DXRegisterObjectNV(interopDevice_, privateTex_, glTexture_,
                                           GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV_);
    if (!interopObject_) {
        OVERLAY_LOG_ONCE("wglDXRegisterObjectNV failed");
        glDeleteTextures(1, &glTexture_);
        glTexture_ = 0;
        ReleaseSharedTexture();
        return false;
    }

    openedHandle_ = handle;
    lastFrameIndex_ = 0;
    hasContent_ = false;
    locked_ = false;
    texWidth_ = width;
    texHeight_ = height;
    OVERLAY_LOG("registered host texture %ux%u with GL interop", width, height);
    return true;
}

void OpenGLRenderer::Render(HDC hdc, SharedState* state) {
    (void)hdc;
    if (failed_ || !state->visible) return;

    if (!EnsureFunctions() || !EnsureD3DDevice(state) || !EnsurePipeline()) {
        failed_ = true;
        return;
    }
    if (!EnsureSharedTexture(state)) return;

    HANDLE object = interopObject_;

    // Refresh the private copy only when the host publishes a new frame. The
    // interop object is briefly unlocked so D3D may write it, then re-locked -
    // so lock/unlock happens at the ~60 Hz publish rate, not on every swap.
    // Locking every swap is what cost this backend ~0.25 ms.
    const uint32_t frameIndex = state->frameIndex;
    if (frameIndex != lastFrameIndex_) {
        HRESULT hr = sharedMutex_->AcquireSync(1, 0);   // zero timeout, never blocks
        if (hr == S_OK || hr == static_cast<HRESULT>(WAIT_ABANDONED)) {
            if (locked_) { gl.DXUnlockObjectsNV(interopDevice_, 1, &object); locked_ = false; }
            d3dContext_->CopyResource(privateTex_, sharedTex_);
            d3dContext_->Flush();
            sharedMutex_->ReleaseSync(0);
            lastFrameIndex_ = frameIndex;
            hasContent_ = true;
        } else if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->mutexTimeoutCount));
        }
    }
    if (!hasContent_) return;

    GlStateBackup backup;
    backup.Capture();

    if (!locked_) {
        if (!gl.DXLockObjectsNV(interopDevice_, 1, &object)) {
            backup.Restore();
            OVERLAY_LOG_ONCE("wglDXLockObjectsNV failed");
            return;
        }
        locked_ = true;
    }

    RECT rc{};
    HWND hwnd = WindowFromDC(hdc);
    if (hwnd) GetClientRect(hwnd, &rc);
    if (rc.right > 0 && rc.bottom > 0) glViewport(0, 0, rc.right, rc.bottom);

    glDisable(GL_DEPTH_TEST_);
    glDisable(GL_CULL_FACE_);
    glDisable(GL_SCISSOR_TEST_);
    glDisable(GL_FRAMEBUFFER_SRGB_);   // write our already-encoded bytes verbatim
    glEnable(GL_BLEND_);
    gl.BlendFuncSeparate(GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_, GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_);

    gl.UseProgram(program_);
    gl.BindVertexArray(vao_);
    gl.ActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, glTexture_);
    gl.BindSampler(0, sampler_);
    GLint loc = gl.GetUniformLocation(program_, "overlayTex");
    if (loc >= 0) gl.Uniform1i(loc, 0);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->drawCount));

    // Deliberately left GL-locked; the next new frame unlocks it for the copy.
    backup.Restore();
}

void OpenGLRenderer::ReleaseSharedTexture() {
    if (interopObject_ && interopDevice_) {
        HANDLE object = interopObject_;
        if (locked_) { gl.DXUnlockObjectsNV(interopDevice_, 1, &object); locked_ = false; }
        gl.DXUnregisterObjectNV(interopDevice_, interopObject_);
        interopObject_ = nullptr;
    }
    if (glTexture_) {
        glDeleteTextures(1, &glTexture_);
        glTexture_ = 0;
    }
    SafeRelease(privateTex_);
    SafeRelease(sharedMutex_);
    SafeRelease(sharedTex_);
    openedHandle_ = 0;
    hasContent_ = false;
}

void OpenGLRenderer::Shutdown() {
    // Best-effort: this may run without the game's GL context current, so only
    // touch GL objects if we can. The D3D side is always safe to release.
    ReleaseSharedTexture();
    if (interopDevice_) {
        gl.DXCloseDeviceNV(interopDevice_);
        interopDevice_ = nullptr;
    }
    SafeRelease(d3dContext_);
    SafeRelease(d3dDevice_);
}

} // namespace overlay

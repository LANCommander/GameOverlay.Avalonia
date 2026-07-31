#include "opengl_glx_renderer.h"

#include "log.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glx.h>

namespace overlay {

namespace {

// Matches platform_linux's State naming and the managed FrameMappingName, with
// the POSIX shm prefix. Both sides derive it from pid + generation.
void FormatFrameShmName(char (&buffer)[80], uint32_t gamePid, uint32_t generation) {
    std::snprintf(buffer, sizeof(buffer), "/AvaloniaOverlay.Frame.%u.%u", gamePid, generation);
}

uint32_t SeqLoad(const volatile uint32_t* p) {
    return std::atomic_ref<uint32_t>(*const_cast<uint32_t*>(p)).load(std::memory_order_acquire);
}

// --- modern GL entry points (resolved once for the core-profile path) --------
// gl.h only declares GL 1.1; everything below is GL 2.0+/3.0+ resolved through
// glXGetProcAddress. Constants that gl.h lacks are defined here.
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

constexpr GLenum GL_VERTEX_SHADER_ = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER_ = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS_ = 0x8B81;
constexpr GLenum GL_LINK_STATUS_ = 0x8B82;
constexpr GLenum GL_ARRAY_BUFFER_ = 0x8892;
constexpr GLenum GL_STATIC_DRAW_ = 0x88E4;
constexpr GLenum GL_TEXTURE0_ = 0x84C0;
constexpr GLenum GL_CURRENT_PROGRAM_ = 0x8B8D;
constexpr GLenum GL_VERTEX_ARRAY_BINDING_ = 0x85B5;
constexpr GLenum GL_ARRAY_BUFFER_BINDING_ = 0x8894;
constexpr GLenum GL_ACTIVE_TEXTURE_ = 0x84E0;
constexpr GLenum GL_CONTEXT_PROFILE_MASK_ = 0x9126;
constexpr GLint  GL_CONTEXT_CORE_PROFILE_BIT_ = 0x1;

struct GLFns {
    GLuint (*CreateShader)(GLenum) = nullptr;
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void (*CompileShader)(GLuint) = nullptr;
    void (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    GLuint (*CreateProgram)() = nullptr;
    void (*AttachShader)(GLuint, GLuint) = nullptr;
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar*) = nullptr;
    void (*LinkProgram)(GLuint) = nullptr;
    void (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*DeleteShader)(GLuint) = nullptr;
    void (*DeleteProgram)(GLuint) = nullptr;
    void (*UseProgram)(GLuint) = nullptr;
    GLint (*GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void (*Uniform1i)(GLint, GLint) = nullptr;
    void (*GenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void (*BindVertexArray)(GLuint) = nullptr;
    void (*DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void (*GenBuffers)(GLsizei, GLuint*) = nullptr;
    void (*BindBuffer)(GLenum, GLuint) = nullptr;
    void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (*DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
    void (*EnableVertexAttribArray)(GLuint) = nullptr;
    void (*ActiveTexture)(GLenum) = nullptr;
    bool loaded = false;
};

GLFns g_gl;

template <typename T>
void Load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
}

bool LoadGLFns() {
    if (g_gl.loaded) return g_gl.CreateProgram != nullptr;
    g_gl.loaded = true;
    Load(g_gl.CreateShader, "glCreateShader");
    Load(g_gl.ShaderSource, "glShaderSource");
    Load(g_gl.CompileShader, "glCompileShader");
    Load(g_gl.GetShaderiv, "glGetShaderiv");
    Load(g_gl.GetShaderInfoLog, "glGetShaderInfoLog");
    Load(g_gl.CreateProgram, "glCreateProgram");
    Load(g_gl.AttachShader, "glAttachShader");
    Load(g_gl.BindAttribLocation, "glBindAttribLocation");
    Load(g_gl.LinkProgram, "glLinkProgram");
    Load(g_gl.GetProgramiv, "glGetProgramiv");
    Load(g_gl.DeleteShader, "glDeleteShader");
    Load(g_gl.DeleteProgram, "glDeleteProgram");
    Load(g_gl.UseProgram, "glUseProgram");
    Load(g_gl.GetUniformLocation, "glGetUniformLocation");
    Load(g_gl.Uniform1i, "glUniform1i");
    Load(g_gl.GenVertexArrays, "glGenVertexArrays");
    Load(g_gl.BindVertexArray, "glBindVertexArray");
    Load(g_gl.DeleteVertexArrays, "glDeleteVertexArrays");
    Load(g_gl.GenBuffers, "glGenBuffers");
    Load(g_gl.BindBuffer, "glBindBuffer");
    Load(g_gl.BufferData, "glBufferData");
    Load(g_gl.DeleteBuffers, "glDeleteBuffers");
    Load(g_gl.VertexAttribPointer, "glVertexAttribPointer");
    Load(g_gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
    Load(g_gl.ActiveTexture, "glActiveTexture");

    bool ok = g_gl.CreateShader && g_gl.CreateProgram && g_gl.GenVertexArrays &&
              g_gl.GenBuffers && g_gl.UseProgram && g_gl.VertexAttribPointer;
    if (!ok) OVERLAY_LOG("core GL entry points unavailable; shader path disabled");
    return ok;
}

const char* kVertexShader =
    "#version 150\n"
    "in vec2 aPos;\n"
    "in vec2 aTex;\n"
    "out vec2 vTex;\n"
    "void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

const char* kFragmentShader =
    "#version 150\n"
    "in vec2 vTex;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uTex;\n"
    "void main() { frag = texture(uTex, vTex); }\n";

GLuint CompileShader(GLenum type, const char* src) {
    GLuint sh = g_gl.CreateShader(type);
    g_gl.ShaderSource(sh, 1, &src, nullptr);
    g_gl.CompileShader(sh);
    GLint ok = 0;
    g_gl.GetShaderiv(sh, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        char log[512];
        g_gl.GetShaderInfoLog(sh, sizeof(log), nullptr, log);
        OVERLAY_LOG("shader compile failed: %s", log);
        g_gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

bool OpenGLGlxRenderer::EnsureFrameMapping(SharedState* state) {
    const uint32_t generation = state->cpuFrameGeneration;
    if (framePixels_ && generation == openedGeneration_) return true;

    ReleaseFrameMapping();

    const uint32_t width = state->texWidth;
    const uint32_t height = state->texHeight;
    if (generation == 0 || width == 0 || height == 0) return false;

    char name[80];
    FormatFrameShmName(name, static_cast<uint32_t>(getpid()), generation);

    int fd = shm_open(name, O_RDONLY, 0600);
    if (fd < 0) return false;   // host may not have created it yet; retry next frame

    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    void* p = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        OVERLAY_LOG_ONCE("mmap frame '%s' failed: %s", name, std::strerror(errno));
        close(fd);
        return false;
    }

    frameFd_ = fd;
    framePixels_ = static_cast<const uint8_t*>(p);
    frameBytes_ = bytes;
    openedGeneration_ = generation;
    texWidth_ = width;
    texHeight_ = height;

    if (stagingBytes_ < bytes) {
        std::free(staging_);
        staging_ = static_cast<uint8_t*>(std::malloc(bytes));
        stagingBytes_ = staging_ ? bytes : 0;
    }

    OVERLAY_LOG("opened cpu frame mapping %ux%u gen %u ('%s')", width, height, generation, name);
    return staging_ != nullptr;
}

void OpenGLGlxRenderer::ReleaseFrameMapping() {
    if (framePixels_) {
        munmap(const_cast<uint8_t*>(framePixels_), frameBytes_);
        framePixels_ = nullptr;
    }
    if (frameFd_ >= 0) {
        close(frameFd_);
        frameFd_ = -1;
    }
    frameBytes_ = 0;
}

bool OpenGLGlxRenderer::ReadFrame(SharedState* state) {
    if (!framePixels_ || !staging_) return false;

    uint32_t s1 = SeqLoad(&state->cpuFrameSeq);
    if (s1 & 1u) return false;                 // host is writing
    std::memcpy(staging_, framePixels_, frameBytes_);
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t s2 = SeqLoad(&state->cpuFrameSeq);
    return s1 == s2;
}

void OpenGLGlxRenderer::UploadTexture() {
    if (glTexture_ == 0) {
        glGenTextures(1, &glTexture_);
        glBindTexture(GL_TEXTURE_2D, glTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, glTexture_);
    }
    // Host output is premultiplied BGRA8888; upload as-is.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(texWidth_),
                 static_cast<GLsizei>(texHeight_), 0, GL_BGRA, GL_UNSIGNED_BYTE, staging_);
}

bool OpenGLGlxRenderer::EnsureShaderPipeline() {
    if (shaderReady_) return true;
    if (shaderFailed_) return false;
    if (!LoadGLFns()) { shaderFailed_ = true; return false; }

    GLuint vs = CompileShader(GL_VERTEX_SHADER_, kVertexShader);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER_, kFragmentShader);
    if (!vs || !fs) { shaderFailed_ = true; return false; }

    program_ = g_gl.CreateProgram();
    g_gl.AttachShader(program_, vs);
    g_gl.AttachShader(program_, fs);
    g_gl.BindAttribLocation(program_, 0, "aPos");
    g_gl.BindAttribLocation(program_, 1, "aTex");
    g_gl.LinkProgram(program_);
    g_gl.DeleteShader(vs);
    g_gl.DeleteShader(fs);

    GLint linked = 0;
    g_gl.GetProgramiv(program_, GL_LINK_STATUS_, &linked);
    if (!linked) {
        OVERLAY_LOG("shader program link failed");
        shaderFailed_ = true;
        return false;
    }
    uTexLocation_ = g_gl.GetUniformLocation(program_, "uTex");

    // Clip-space quad as a triangle strip; texcoord v flipped so the image's top
    // row (BGRA origin top-left) maps to the top of the screen.
    const float quad[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    g_gl.GenVertexArrays(1, &vao_);
    g_gl.BindVertexArray(vao_);
    g_gl.GenBuffers(1, &vbo_);
    g_gl.BindBuffer(GL_ARRAY_BUFFER_, vbo_);
    g_gl.BufferData(GL_ARRAY_BUFFER_, sizeof(quad), quad, GL_STATIC_DRAW_);
    g_gl.EnableVertexAttribArray(0);
    g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    g_gl.EnableVertexAttribArray(1);
    g_gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    g_gl.BindVertexArray(0);
    g_gl.BindBuffer(GL_ARRAY_BUFFER_, 0);

    shaderReady_ = true;
    OVERLAY_LOG("core-profile shader pipeline ready");
    return true;
}

void OpenGLGlxRenderer::DrawShader(int width, int height) {
    // Save the state we disturb, then restore it, so the game's next frame is
    // untouched (the core-profile analogue of glPushAttrib).
    GLint pProg = 0, pVao = 0, pArr = 0, pActive = 0, pTex = 0, vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_CURRENT_PROGRAM_, &pProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING_, &pVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING_, &pArr);
    glGetIntegerv(GL_ACTIVE_TEXTURE_, &pActive);
    glGetIntegerv(GL_VIEWPORT, vp);
    GLboolean pBlend = glIsEnabled(GL_BLEND);
    GLboolean pDepth = glIsEnabled(GL_DEPTH_TEST);
    g_gl.ActiveTexture(GL_TEXTURE0_);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex);

    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied

    g_gl.UseProgram(program_);
    g_gl.ActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, glTexture_);
    if (uTexLocation_ >= 0) g_gl.Uniform1i(uTexLocation_, 0);
    g_gl.BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Restore.
    g_gl.BindVertexArray(static_cast<GLuint>(pVao));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(pTex));
    g_gl.ActiveTexture(static_cast<GLenum>(pActive));
    g_gl.BindBuffer(GL_ARRAY_BUFFER_, static_cast<GLuint>(pArr));
    g_gl.UseProgram(static_cast<GLuint>(pProg));
    if (!pBlend) glDisable(GL_BLEND);
    if (pDepth) glEnable(GL_DEPTH_TEST);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void OpenGLGlxRenderer::DrawFixedFunction(int width, int height) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDepthMask(GL_FALSE);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, glTexture_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
    glEnd();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
}

void OpenGLGlxRenderer::Render(SharedState* state, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (!state->visible) return;

    if (EnsureFrameMapping(state) && ReadFrame(state)) {
        UploadTexture();
        hasContent_ = true;
    }

    if (!hasContent_) {
        OVERLAY_LOG_ONCE("not drawing: visible=%u hasContent=0", state->visible);
        return;
    }

    // Decide the draw path once from the context profile: core profiles have no
    // fixed-function pipeline, so they take the shader path.
    if (!profileChecked_) {
        profileChecked_ = true;
        while (glGetError() != GL_NO_ERROR) { /* drain */ }
        GLint mask = 0;
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK_, &mask);
        bool core = glGetError() == GL_NO_ERROR && (mask & GL_CONTEXT_CORE_PROFILE_BIT_);
        useShader_ = core;
        OVERLAY_LOG("gl draw path: %s", core ? "core (shader)" : "compatibility (fixed-function)");
    }

    bool drew = false;
    if (useShader_ && !shaderFailed_ && EnsureShaderPipeline()) {
        DrawShader(width, height);
        drew = true;
    }
    if (!drew) {
        DrawFixedFunction(width, height);
    }

    state->drawCount++;
}

void OpenGLGlxRenderer::Shutdown() {
    ReleaseFrameMapping();
    if (glTexture_ != 0) {
        glDeleteTextures(1, &glTexture_);
        glTexture_ = 0;
    }
    if (shaderReady_) {
        if (g_gl.DeleteProgram && program_) g_gl.DeleteProgram(program_);
        if (g_gl.DeleteVertexArrays && vao_) g_gl.DeleteVertexArrays(1, &vao_);
        if (g_gl.DeleteBuffers && vbo_) g_gl.DeleteBuffers(1, &vbo_);
        program_ = vao_ = vbo_ = 0;
        shaderReady_ = false;
    }
    std::free(staging_);
    staging_ = nullptr;
    stagingBytes_ = 0;
    hasContent_ = false;
}

} // namespace overlay

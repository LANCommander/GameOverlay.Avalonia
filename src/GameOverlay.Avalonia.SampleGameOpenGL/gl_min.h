// Minimal modern-GL declarations.
//
// opengl32.dll only exports GL 1.1; everything from GL 2.0 on must be fetched
// through wglGetProcAddress. Rather than vendor the multi-megabyte Khronos
// glext.h, this declares just the handful of entry points and enums the sample
// needs, loaded once into a struct.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cstdint>

// --- enums from GL 2.0+ not in the 1.1 gl.h -------------------------------
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER    0x8892
#define GL_STATIC_DRAW     0x88E4
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0        0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE   0x812F
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY*
#endif

using GLchar = char;
using GLsizeiptr = intptr_t;

// --- function pointer typedefs --------------------------------------------
typedef GLuint (APIENTRYP PFN_glCreateShader)(GLenum);
typedef void   (APIENTRYP PFN_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void   (APIENTRYP PFN_glCompileShader)(GLuint);
typedef void   (APIENTRYP PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRYP PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (APIENTRYP PFN_glCreateProgram)(void);
typedef void   (APIENTRYP PFN_glAttachShader)(GLuint, GLuint);
typedef void   (APIENTRYP PFN_glLinkProgram)(GLuint);
typedef void   (APIENTRYP PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRYP PFN_glUseProgram)(GLuint);
typedef void   (APIENTRYP PFN_glDeleteShader)(GLuint);
typedef GLint  (APIENTRYP PFN_glGetUniformLocation)(GLuint, const GLchar*);
typedef void   (APIENTRYP PFN_glUniform1f)(GLint, GLfloat);
typedef void   (APIENTRYP PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRYP PFN_glUniform1i)(GLint, GLint);
typedef void   (APIENTRYP PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void   (APIENTRYP PFN_glBindVertexArray)(GLuint);
typedef void   (APIENTRYP PFN_glActiveTexture)(GLenum);

struct GlFunctions {
    PFN_glCreateShader glCreateShader;
    PFN_glShaderSource glShaderSource;
    PFN_glCompileShader glCompileShader;
    PFN_glGetShaderiv glGetShaderiv;
    PFN_glGetShaderInfoLog glGetShaderInfoLog;
    PFN_glCreateProgram glCreateProgram;
    PFN_glAttachShader glAttachShader;
    PFN_glLinkProgram glLinkProgram;
    PFN_glGetProgramiv glGetProgramiv;
    PFN_glUseProgram glUseProgram;
    PFN_glDeleteShader glDeleteShader;
    PFN_glGetUniformLocation glGetUniformLocation;
    PFN_glUniform1f glUniform1f;
    PFN_glUniform2f glUniform2f;
    PFN_glUniform1i glUniform1i;
    PFN_glGenVertexArrays glGenVertexArrays;
    PFN_glBindVertexArray glBindVertexArray;
    PFN_glActiveTexture glActiveTexture;

    // Returns false if any entry point could not be resolved.
    bool Load() {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(wglGetProcAddress(name));
            if (!fn) ok = false;
        };
        get(glCreateShader, "glCreateShader");
        get(glShaderSource, "glShaderSource");
        get(glCompileShader, "glCompileShader");
        get(glGetShaderiv, "glGetShaderiv");
        get(glGetShaderInfoLog, "glGetShaderInfoLog");
        get(glCreateProgram, "glCreateProgram");
        get(glAttachShader, "glAttachShader");
        get(glLinkProgram, "glLinkProgram");
        get(glGetProgramiv, "glGetProgramiv");
        get(glUseProgram, "glUseProgram");
        get(glDeleteShader, "glDeleteShader");
        get(glGetUniformLocation, "glGetUniformLocation");
        get(glUniform1f, "glUniform1f");
        get(glUniform2f, "glUniform2f");
        get(glUniform1i, "glUniform1i");
        get(glGenVertexArrays, "glGenVertexArrays");
        get(glBindVertexArray, "glBindVertexArray");
        get(glActiveTexture, "glActiveTexture");
        return ok;
    }
};

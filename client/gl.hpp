// Minimal OpenGL 3.3 core loader: only the entry points the client uses.
// Loaded at runtime through SDL_GL_GetProcAddress so no GL headers or import
// libraries are needed on any platform.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define UAW_GLAPI __stdcall
#else
#define UAW_GLAPI
#endif

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

constexpr GLenum GL_FALSE_ = 0, GL_TRUE_ = 1;
constexpr GLenum GL_DEPTH_BUFFER_BIT = 0x0100, GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_TRIANGLES = 0x0004, GL_LINES = 0x0001;
constexpr GLenum GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44, GL_BLEND = 0x0BE2, GL_POLYGON_OFFSET_FILL = 0x8037;
constexpr GLenum GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_ONE = 1;
constexpr GLenum GL_BACK = 0x0405, GL_LEQUAL = 0x0203;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406;
constexpr GLenum GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_RGB = 0x1907;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_TEXTURE1 = 0x84C1, GL_TEXTURE2 = 0x84C2, GL_TEXTURE_2D_ARRAY = 0x8C1A;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_REPEAT = 0x2901;
constexpr GLenum GL_LINEAR = 0x2601, GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_STATIC_DRAW = 0x88E4,
                 GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_DEPTH_ATTACHMENT = 0x8D00, GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_NONE = 0;
constexpr GLenum GL_DEPTH_COMPONENT = 0x1902, GL_DEPTH_COMPONENT24 = 0x81A6, GL_TEXTURE3 = 0x84C3, GL_NEAREST = 0x2600;
constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C, GL_TEXTURE_COMPARE_FUNC = 0x884D, GL_COMPARE_REF_TO_TEXTURE = 0x884E;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;

#define UAW_GL_FUNCS(X)                                                                                  \
    X(void, glViewport, GLint, GLint, GLsizei, GLsizei)                                                  \
    X(void, glClear, GLbitfield)                                                                         \
    X(void, glClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                            \
    X(void, glEnable, GLenum)                                                                            \
    X(void, glDisable, GLenum)                                                                           \
    X(void, glBlendFunc, GLenum, GLenum)                                                                 \
    X(void, glDepthMask, GLboolean)                                                                      \
    X(void, glDepthFunc, GLenum)                                                                         \
    X(void, glCullFace, GLenum)                                                                          \
    X(void, glPolygonOffset, GLfloat, GLfloat)                                                           \
    X(GLenum, glGetError)                                                                                \
    X(const GLubyte*, glGetString, GLenum)                                                               \
    X(GLuint, glCreateShader, GLenum)                                                                    \
    X(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)                         \
    X(void, glCompileShader, GLuint)                                                                     \
    X(void, glGetShaderiv, GLuint, GLenum, GLint*)                                                       \
    X(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                      \
    X(void, glDeleteShader, GLuint)                                                                      \
    X(GLuint, glCreateProgram)                                                                           \
    X(void, glAttachShader, GLuint, GLuint)                                                              \
    X(void, glLinkProgram, GLuint)                                                                       \
    X(void, glGetProgramiv, GLuint, GLenum, GLint*)                                                      \
    X(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                     \
    X(void, glUseProgram, GLuint)                                                                        \
    X(void, glDeleteProgram, GLuint)                                                                     \
    X(GLint, glGetUniformLocation, GLuint, const GLchar*)                                                \
    X(void, glUniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*)                               \
    X(void, glUniform2fv, GLint, GLsizei, const GLfloat*)                                                \
    X(void, glUniform3fv, GLint, GLsizei, const GLfloat*)                                                \
    X(void, glUniform4fv, GLint, GLsizei, const GLfloat*)                                                \
    X(void, glUniform1i, GLint, GLint)                                                                   \
    X(void, glUniform1f, GLint, GLfloat)                                                                 \
    X(void, glGenVertexArrays, GLsizei, GLuint*)                                                         \
    X(void, glBindVertexArray, GLuint)                                                                   \
    X(void, glDeleteVertexArrays, GLsizei, const GLuint*)                                                \
    X(void, glGenBuffers, GLsizei, GLuint*)                                                              \
    X(void, glBindBuffer, GLenum, GLuint)                                                                \
    X(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum)                                       \
    X(void, glDeleteBuffers, GLsizei, const GLuint*)                                                     \
    X(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)               \
    X(void, glEnableVertexAttribArray, GLuint)                                                           \
    X(void, glDrawElements, GLenum, GLsizei, GLenum, const void*)                                        \
    X(void, glDrawArrays, GLenum, GLint, GLsizei)                                                        \
    X(void, glGenTextures, GLsizei, GLuint*)                                                             \
    X(void, glBindTexture, GLenum, GLuint)                                                               \
    X(void, glDeleteTextures, GLsizei, const GLuint*)                                                    \
    X(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)    \
    X(void, glTexImage3D, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
    X(void, glTexSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*) \
    X(void, glTexParameteri, GLenum, GLenum, GLint)                                                      \
    X(void, glGenerateMipmap, GLenum)                                                                    \
    X(void, glActiveTexture, GLenum)                                                                     \
    X(void, glPixelStorei, GLenum, GLint)                                                                \
    X(void, glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)                        \
    X(void, glGenFramebuffers, GLsizei, GLuint*)                                                         \
    X(void, glBindFramebuffer, GLenum, GLuint)                                                           \
    X(void, glDeleteFramebuffers, GLsizei, const GLuint*)                                                \
    X(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)                               \
    X(GLenum, glCheckFramebufferStatus, GLenum)                                                          \
    X(void, glColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                                     \
    X(void, glDrawBuffer, GLenum)                                                                        \
    X(void, glReadBuffer, GLenum)

#define UAW_GL_DECLARE(ret, name, ...) using PFN_##name = ret(UAW_GLAPI*)(__VA_ARGS__); extern PFN_##name name;
UAW_GL_FUNCS(UAW_GL_DECLARE)
#undef UAW_GL_DECLARE

namespace uaw::gl {

using ProcLoader = void* (*)(const char*);

// Loads every entry point; returns the name of the first missing one, or nullptr.
const char* load(ProcLoader loader);

} // namespace uaw::gl

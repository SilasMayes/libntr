#ifndef SIM_OPENGL_H
#define SIM_OPENGL_H

// SIM_GLES is set for targets that render through OpenGL ES 3.2 instead of
// desktop OpenGL 4.2 (Android, or a desktop build configured with -Dgles=true).
#if defined(SDK_BUILD_ANDROID) && !defined(SIM_GLES)
#define SIM_GLES 1
#endif

#ifdef SIM_GLES
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#ifndef GLAPIENTRY
#define GLAPIENTRY GL_APIENTRY
#endif
#elif defined(SDK_BUILD_PROSPERO)
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#elif SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif

#ifdef __cplusplus
#include <string>
namespace SIM {
// Rewrites a "#version 420 core" shader so it compiles on the active GL flavour.
// Desktop builds get the source back unchanged.
std::string PrepareShaderSource(const char *source);
} // namespace SIM
#endif

#endif

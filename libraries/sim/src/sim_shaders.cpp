#include <nitro.h>
#include <simulator/g3_handler.h>
#include <simulator/sim_opengl.h>

#include <cstring>
#include <string>

//Shader
namespace SIM {

const char * G3VertexShader =
#include "shaders/g3_vertex.glsl"
;

const char * G3FragmentShader =
#include "shaders/g3_fragment.glsl"
;

const char * G2VertexShader =
#include "shaders/g2_vertex.glsl"
;

const char * G2FragmentShader =
#include "shaders/g2_fragment.glsl"
;

std::string PrepareShaderSource(const char *source) {
    std::string out(source);
#ifdef SIM_GLES
    static constexpr const char *kDesktopVersion = "#version 420 core";
    static constexpr const char *kEsHeader =
        "#version 320 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n";

    size_t pos = out.find(kDesktopVersion);
    if (pos != std::string::npos) {
        // Drop everything before the directive too: GLSL ES requires #version on the first line.
        out.replace(0, pos + strlen(kDesktopVersion), kEsHeader);
    }
#endif
    return out;
}

}

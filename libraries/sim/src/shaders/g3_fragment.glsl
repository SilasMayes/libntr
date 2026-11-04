R""(
#version 420 core

uniform sampler2D myTexture;
uniform int useTexture;
uniform int polygonMode;
uniform int useFog;
uniform vec4 fogColor;
uniform float fogDepthBoundary[32];
uniform float fogDensity[32];
smooth in vec2 fragTexCoord;
smooth in vec4 fragInColor;
smooth in vec4 coords;
// Edge marking (sim_main.cpp's edge pass) reads what the opaque pixels leave in attr: the
// polygon ID (6 bits, times 4) in r, the depth's high and low bytes in g and b. a is the write
// mask, blended like the colour: 1 for an opaque pixel replaces the pixel's attributes, 0 for a
// translucent one keeps them (on the DS translucent pixels don't change the opaque polygon ID
// either). Opaque is the DS's alpha 31, polygon times texel, as an A3I5 texel can make it.
uniform float polygonId;
layout(location = 0) out vec4 color;
layout(location = 1) out vec4 attr;

void main()
{
    vec4 shadedColor;
    if( useTexture == 1 ) {
        if(polygonMode == 0) {
            shadedColor.r = texture(myTexture, fragTexCoord).r * fragInColor.r;
            shadedColor.g = texture(myTexture, fragTexCoord).g * fragInColor.g;
            shadedColor.b = texture(myTexture, fragTexCoord).b * fragInColor.b;
            shadedColor.a = texture(myTexture, fragTexCoord).a * fragInColor.a;
        } else {
            shadedColor = texture(myTexture, fragTexCoord);
        }
        if( texture(myTexture, fragTexCoord).a < 0.05) {
            discard;
        }
    } else {
        shadedColor = fragInColor;
    }

    float depth16 = floor(clamp(gl_FragCoord.z, 0.0, 1.0) * 65535.0 + 0.5);
    float depthHigh = floor(depth16 / 256.0);
    attr = vec4(polygonId * 4.0 / 255.0, depthHigh / 255.0, (depth16 - depthHigh * 256.0) / 255.0,
                shadedColor.a > 30.5 / 31.0 ? 1.0 : 0.0);
    
    if(useFog == 1) {
        float fog_maxdist = 1.0;
        float fog_mindist = 0.5;
        float dist = length(coords.xyz);
        float fogFactor = 0.0;
        if(coords.z < fogDepthBoundary[0]) {
            
            fogFactor = fogDensity[0];
        } else {
            int foundIdx = 31;
            for(int i=1; i<32; i++) {
                if(coords.z < fogDepthBoundary[i]) {
                    foundIdx = i;
                    break;
                }
            }
            if(foundIdx == 31) {
                fogFactor = fogDensity[foundIdx];
            } else {
                float interpolationFactor = (fogDepthBoundary[foundIdx-1] - coords.z) / (fogDepthBoundary[foundIdx] - fogDepthBoundary[foundIdx-1]);
                fogFactor = (fogDensity[foundIdx - 1] * (interpolationFactor)) + (fogDensity[foundIdx] * (1.0-interpolationFactor));
            }
        }
        color = mix(shadedColor, fogColor, fogFactor);
    } else {
     color = shadedColor;
    }
}
)""
R""(
#version 420 core

uniform sampler2D myTexture;
uniform sampler2D bgTexture[4];
uniform sampler2D objTexture[5]; //The extra element is for OBJs drawn on top of all BGs
uniform sampler2D objWindowTexture;
uniform int bgOrder[4];
uniform int bgPriorities[4];
uniform int bldcnt;
uniform int bldalpha;
uniform int bldy;
uniform int topScreen;
uniform vec4 backdropColor;
uniform int dispcnt;
uniform int winout;
uniform int bg0as3d;
// Widescreen: the screen is wideRatio DS screens wide; the 2D layers stay in its middle
// and the 3D scene (bg0as3d) fills it. tex3dScaleS: how much of the 3D texture's width
// this screen's 3D uses.
uniform float wideRatio;
uniform float tex3dScaleS;

smooth in vec2 fragTexCoord;
//uniform float inAlpha;
uniform float inMasterBrightFactor;
uniform int inMasterBrightMode;
out vec4 color;
void main()
{
    int oamsDrawn = 0;
    int currentBgDrawn = 5; // Backdrop
    bool objWindowEnabled = ((dispcnt >> 15) & 1) == 1;
    if(topScreen == 1) {
        if(fragTexCoord.t < 0.5){
            discard;
        }
    } else {
        if(fragTexCoord.t >= 0.5){
            discard;
        }
    }
    // Fetch every layer once, with constant sampler indices. Indexing sampler
    // arrays with non-uniform values is undefined in GLSL (and breaks on GLES).
    vec2 c2d = vec2((fragTexCoord.s - 0.5) * wideRatio + 0.5, fragTexCoord.t);
    bool inside2d = c2d.s >= 0.0 && c2d.s <= 1.0;
    vec4 none = vec4(0.0);
    vec4 bgc[4];
    if (bg0as3d != 0) {
        bgc[0] = texture(bgTexture[0], vec2(fragTexCoord.s * tex3dScaleS, fragTexCoord.t));
    } else {
        bgc[0] = inside2d ? texture(bgTexture[0], c2d) : none;
    }
    bgc[1] = inside2d ? texture(bgTexture[1], c2d) : none;
    bgc[2] = inside2d ? texture(bgTexture[2], c2d) : none;
    bgc[3] = inside2d ? texture(bgTexture[3], c2d) : none;
    vec4 objc[4];
    objc[0] = inside2d ? texture(objTexture[0], c2d) : none;
    objc[1] = inside2d ? texture(objTexture[1], c2d) : none;
    objc[2] = inside2d ? texture(objTexture[2], c2d) : none;
    objc[3] = inside2d ? texture(objTexture[3], c2d) : none;
    vec4 objWin = inside2d ? texture(objWindowTexture, c2d) : none;
    for(int i=0; i<4; i++) {
        int bgNum = bgOrder[i];
        int curPrio = bgPriorities[bgNum];
        // Draw the OBJs that are at or below the current BG priority
        for(int j=3-oamsDrawn; j>curPrio; j--) {
            if(objc[j].a > 0.1) {
                color = objc[j];
                currentBgDrawn = 4;
            }
            oamsDrawn++;
        }
        
        // Check obj window
        if(objWindowEnabled && objWin.a > 0.1)
        {
            // Inside the window
            if(((winout >> (bgNum+8)) & 1) == 0) {
                continue;
            }
        } else if (objWindowEnabled) {
            // Outside the window
            if(((winout >> bgNum) & 1) == 0) {
                continue;
            }
        }
        // Draw the current bg
        if(bgc[bgNum].a > 0.1) {
            //Handle blending
            if( (((bldcnt >> 6) & 0x3) == 1) && ((bldcnt >> bgNum & 1) == 1) ){
                // Alpha Blending Mode. bgNum is Target 1.
                float alphaA = min(16.0, float(bldalpha & 31)) / 16.0;
                float alphaB = min(16.0, float((bldalpha >> 8) & 31)) / 16.0;
                if(bgNum == 0 && bg0as3d != 0) {
                    alphaA = bgc[0].a;
                    alphaB = 1.0 - alphaA;
                }
                if((currentBgDrawn < 4) && (((bldcnt >> (8+currentBgDrawn)) & 1) == 1) ) {
                    color.r = min(1.0, ((bgc[bgNum].r *alphaA) + (bgc[currentBgDrawn].r * alphaB)));
                    color.g = min(1.0, ((bgc[bgNum].g *alphaA) + (bgc[currentBgDrawn].g * alphaB)));
                    color.b = min(1.0, ((bgc[bgNum].b *alphaA) + (bgc[currentBgDrawn].b * alphaB)));
                    color.a = 1.0;
                    currentBgDrawn = bgNum;
                } else if( (currentBgDrawn == 4) && ( ( (bldcnt >> (8+currentBgDrawn)) & 1) == 1) ){
                    // Alpha blending where the highest OBJ pixel is the target
                    color.r = min(1.0, ( (bgc[bgNum].r *alphaA) + color.r * alphaB) );
                    color.g = min(1.0, ( (bgc[bgNum].g *alphaA) + color.g * alphaB) );
                    color.b = min(1.0, ( (bgc[bgNum].b *alphaA) + color.b * alphaB) );
                    color.a = 1.0;
                } else if((currentBgDrawn == 5) && (((bldcnt >> (8+currentBgDrawn)) & 1) == 1)) {
                    // Alpha blending where the backdrop is the target
                    color.r = min(1.0, ((bgc[bgNum].r *alphaA) + backdropColor.r * alphaB));
                    color.g = min(1.0, ((bgc[bgNum].g *alphaA) + backdropColor.g * alphaB));
                    color.b = min(1.0, ((bgc[bgNum].b *alphaA) + backdropColor.b * alphaB));
                    color.a = 1.0;
                } else {
                    color = bgc[bgNum];
                    currentBgDrawn = bgNum;
                }
            } else if((((bldcnt >> 6) & 0x3) == 2) && ((bldcnt >> bgNum & 1) == 1)) {
                // Brightness Increase Mode. bgNum is Target 1.
                float brightFactor = min(16.0, float(bldy & 31)) / 16.0;
                //color.r = min(1.0, ( bgc[bgNum].r + ((1.0 - bgc[bgNum].r)*brightFactor)) );
                //color.g = min(1.0, ( bgc[bgNum].g + ((1.0 - bgc[bgNum].g)*brightFactor)) );
                //color.b = min(1.0, ( bgc[bgNum].b + ((1.0 - bgc[bgNum].b)*brightFactor)) );
                color.r = bgc[bgNum].r + (1.0 - bgc[bgNum].r)*brightFactor;
                color.g = bgc[bgNum].g + (1.0 - bgc[bgNum].g)*brightFactor;
                color.b = bgc[bgNum].b + (1.0 - bgc[bgNum].b)*brightFactor;
                color.a = 1.0;
                currentBgDrawn = bgNum;
            } else if((((bldcnt >> 6) & 0x3) == 3) && ((bldcnt >> bgNum & 1) == 1)) {
                // Brightness Decrease Mode. bgNum is Target 1.
                float brightFactor = min(16.0, float(bldy & 31)) / 16.0;
                color.r = max(0.0, ( bgc[bgNum].r - ((bgc[bgNum].r)*brightFactor)) );
                color.g = max(0.0, ( bgc[bgNum].g - ((bgc[bgNum].g)*brightFactor)) );
                color.b = max(0.0, ( bgc[bgNum].b - ((bgc[bgNum].b)*brightFactor)) );
                color.a = 1.0;
                currentBgDrawn = bgNum;
            } else {
                color.rgb = bgc[bgNum].rgb;
                color.a = 1.0;
                currentBgDrawn = bgNum;
            }
        }
    }
    //Draw the remaining objs
    for(int i=3-oamsDrawn; i>=0; i--) {
        if(objc[i].a > 0.1) {
            color = objc[i];
            currentBgDrawn = 4;
        }
    }
    if(currentBgDrawn == 5) {
        color = backdropColor;
    }
    
    if(inMasterBrightMode == 1) {
        // brightness up
        color.r = color.r + ((1.0 - color.r) * (inMasterBrightFactor/16.0));
        color.g = color.g + ((1.0 - color.g) * (inMasterBrightFactor/16.0));
        color.b = color.b + ((1.0 - color.b) * (inMasterBrightFactor/16.0));
    } else if(inMasterBrightMode == 2) {
        // brightness down
        color.r = color.r - (color.r * (inMasterBrightFactor / 16.0));
        color.g = color.g - (color.g * (inMasterBrightFactor / 16.0));
        color.b = color.b - (color.b * (inMasterBrightFactor / 16.0));
    }
}
)""
#ifndef LIBNTR_SIM_COMPANION_H
#define LIBNTR_SIM_COMPANION_H

// Companion UI on the secondary output (the AYN Thor's lower panel, or the desktop
// "Bottom Screen" window): a second Dear ImGui context, drawn at the output's native
// resolution, that a game fills in through the SIM_Prj_Companion* hooks.
//
// Layout: when the output is noticeably taller than 4:3 (the Thor's lower panel is
// 1080x1240), the DS bottom screen sits at the top and the companion takes the space
// below it. The companion can also take the whole output ("full screen", the DS
// bottom screen is hidden then): L3 on Android, F1 on desktop, or the game's own UI.
// Touches are routed by where they start: on the DS screen they are DS touches,
// elsewhere they go to the companion.

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SIM_CompanionFrame {
    float x, y, width, height; // area the companion owns, in output pixels (top-left origin)
    float outputWidth, outputHeight;
    BOOL fullScreen;           // the DS bottom screen is hidden
    BOOL canShareScreen;       // the output has room for the DS screen and the companion
} SIM_CompanionFrame;

// Game hooks (weak no-op defaults in libntr)
BOOL SIM_Prj_CompanionEnabled(void);
void SIM_Prj_CompanionSetup(float outputWidth); // once, with the companion's ImGui context current
void SIM_Prj_CompanionDraw(const SIM_CompanionFrame *frame);

// Pictures for the companion's (or the settings') ImGui drawing: an RGBA8 image (rows top
// to bottom, sharp pixels when scaled) as an ImTextureID. Call while drawing the UI.
unsigned int SIM_UI_CreateImage(int width, int height, const void *rgba);
void SIM_UI_DeleteImage(unsigned int image);

// The companion shows part of the DS touch screen inside itself this frame (a Pokétch tab,
// say): the DS screen's region dsX, dsY, dsWidth x dsHeight (DS pixels) drawn into the
// rectangle x, y, width x height (output pixels, top-left origin) over the companion, and
// touches there go to the DS. Call from SIM_Prj_CompanionDraw, every frame it should show.
void SIM_Companion_ShowDsScreen(float x, float y, float width, float height, int dsX, int dsY, int dsWidth,
                                int dsHeight);

void SIM_Companion_SetFullScreen(BOOL fullScreen);
BOOL SIM_Companion_IsFullScreen(void);
void SIM_Companion_ToggleFullScreen(void);

// libntr internals: layout of the secondary output, touch routing and drawing
typedef struct SIM_CompanionLayout {
    BOOL dsVisible;
    int dsX, dsY, dsWidth, dsHeight; // top-left origin
    BOOL companionVisible;
    SIM_CompanionFrame frame;
} SIM_CompanionLayout;

void SIM_Companion_ComputeLayout(int outputWidth, int outputHeight, SIM_CompanionLayout *layout);
void SIM_Companion_QueueTouch(int type, int x, int y); // SIM_TOUCH_* from sim_display.h
void SIM_Companion_Render(const SIM_CompanionLayout *layout);
typedef struct SIM_CompanionDsView {
    int x, y, width, height;           // output pixels, top-left origin
    int dsX, dsY, dsWidth, dsHeight;   // DS pixels
} SIM_CompanionDsView;
BOOL SIM_Companion_DsView(SIM_CompanionDsView *out); // what the last render asked for

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_COMPANION_H

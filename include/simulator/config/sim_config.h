#ifndef SIM_CONFIG_H
#define SIM_CONFIG_H

#include <nitro.h>

typedef enum {
    SIM_CONFIG_SCREEN_LAYOUT_VERTICAL, /* Vertical screen layout (like a real DS) */
    SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL, /* DS screens side by side */
    SIM_CONFIG_SCREEN_LAYOUT_LARGE /* One large screen and one small screen next to it */
} SIM_config_screen_layout_type;

#define SIM_CONFIG_JOY_HAT_MASK (0x10000)
#define SIM_CONFIG_JOY_AXIS_PLUS_MASK (0x20000)
#define SIM_CONFIG_JOY_AXIS_MINUS_MASK (0x40000)
#define SIM_CONFIG_JOY_INVALID_KEY (-1)

typedef struct {
    int aKey;
    int bKey;
    int xKey;
    int yKey;
    int upKey;
    int downKey;
    int leftKey;
    int rightKey;
    int lKey;
    int rKey;
    int startKey;
    int selectKey;
    int guiKey;
    int frameCapToggleKey;
    int frameCapKey;

    // Joystick keys
    int joyAxisDeadzone;
    int aJoyKey;
    int bJoyKey;
    int xJoyKey;
    int yJoyKey;
    int upJoyKey;
    int downJoyKey;
    int leftJoyKey;
    int rightJoyKey;
    int lJoyKey;
    int rJoyKey;
    int startJoyKey;
    int selectJoyKey;
    int fastForwardJoyKey;
} SIM_config_pad_settings_type;

#define SIM_CONFIG_WINDOW_POS_UNSET (-2147483647 - 1)

typedef struct {
    char serverIp[16]; /* IP of the last server connected to through Net */
} SIM_config_net_settings_type;

typedef struct {
    u32 internalResolutionScale; /* 3D rendering internal resolution scale factor (0: match the screen) */
    u8 antiAliasing; /* 3D anti-aliasing samples: 0 (off), 2 or 4 */
    u8 widescreen; /* The top screen as wide as the display where the game allows (SIM_SetWidescreenScene) */
    u8 texturePacks; /* Replace 3D textures with the packs' (simulator/sim_texture_packs.h) */
    u8 dumpTextures; /* Save the 3D textures the game uses as PNGs */
    u8 edgeMarking; /* Draw the DS's 3D outlines (edge marking) where the game turns them on */
    u32 windowWidth; /* Initial width of the window. If set to 0, will use the default width for the screen layout */
    u32 windowHeight; /* Initial height of the window. If set to 0, will use default height for the screen layout */
    u8 fullScreen; /* TRUE to enable fullscreen */
    s32 windowX; /* Main window position on the desktop (SIM_CONFIG_WINDOW_POS_UNSET: let the system pick) */
    s32 windowY;
    s32 bottomWindowX; /* Desktop: the bottom screen's own window (secondaryScreen), last position and size */
    s32 bottomWindowY;
    u32 bottomWindowWidth; /* 0: the default size */
    u32 bottomWindowHeight;
    u8 bottomFullScreen; /* Desktop: the bottom screen's window covers its display */
    u8 masterVolume; /* Master sound volume (0-127) */
    SIM_config_screen_layout_type screenLayout; /* DS screen layout */
    BOOL swapScreens; /* Swap screen positions */
    BOOL secondaryScreen; /* Show the bottom screen on a second display (Android) or window (PC) when one is available */
    u8 vsyncInterval; /* Vertical sync interval. Set to 0 to disable vsync, 1 every vblank, 2 every second vblank etc*/
    u8 capFrameRate; /* Should the frame rate be capped to 60 fps? */
    u8 perfOverlay; /* Show the frame timings over the game */
    u8 fastForwardSpeed; /* Fast-forward: times the normal speed (0: as fast as it goes) */
    u8 fastForwardToggle; /* Fast-forward button: TRUE switches it on and off, FALSE works while held */
    u8 macAddr[6]; /* MAC address */
    SIM_config_pad_settings_type padSettings; /* Input pad settings */
    NVRAMConfig osNvramConfig; /* OS NVRAM config */
    SIM_config_net_settings_type netSettings; /* Net Settings */
} SIM_config_type;

#ifdef __cplusplus
extern "C" {
#endif

BOOL SIM_Config_LoadConfigFile(SIM_config_type * aConfig);
void SIM_Config_LoadDefaults(SIM_config_type * aConfig);
void SIM_Config_SaveConfigFile(SIM_config_type * aConfig);
void SIM_Config_SetOSNVRAMConfig(SIM_config_type * aConfig);

#ifdef __cplusplus
}
#endif

#endif
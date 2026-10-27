#ifndef SIM_H
#define SIM_H
#include <nitro.h>
#include <nitro/types.h>
#include <nitro/gx/gx_vramcnt.h>
#include <SDL2/SDL.h>
#include <simulator/config/sim_config.h>
#include <simulator/sim_debug.h>

#include "simulator/assert.h"

#define SIM_NDS_SCREEN_WIDTH 256
#define SIM_NDS_SCREEN_HEIGHT 192

#define SIM_NDS_SCREEN_SCALE_FACTOR 4
#define SIM_NDS_WINDOW_SCALE_FACTOR 2

typedef struct WIN_snd_channel_info {
	u8 playing;
	u32 preLoopLen;
	u32 preLoopSamples;
	u32 loopLen;
	u32 loopSamples;
	u32 alarms_bitfield;
	u32 audio_len;
	u32 total_audio_len;
	u32 total_num_samples;
	u8 * audio_pos;
	u8 * audio_data;
	u8 inLoop;
	u32 posPreLoop;
	u32 posInLoop;
	u32 pos;
	u8 stream[500000];
	u8 resampStream[1800000];
	SDL_AudioStream * sdlStream;
	u32 streamSampleRate;
	double pitchRatio;
	u32 prevTimer;
	u8 pitchBend;
} WIN_snd_channel_info;

typedef struct WIN_snd_alarm_info {
	u32 alarmNo;
	u32 time;
	u32 period;
	u32 id;
	pthread_t thread;
	u8 isRunning;
} WIN_snd_alarm_info;

#ifdef __cplusplus
extern "C" {
#endif

void SIM_PreRenderVBlank(void);
void SIM_PostRenderVBlank(void);
void SIM_procPXI(void);
void SIM_procPXI7to9(void);
int SIM_procPxiThread( void * arg );
void SIM_PostPxiThread();
int SIM_procPxi7to9Thread( void * arg );
void SIM_PostPxi7to9Thread();
int SIM_procOSThreadThread( void * arg );
void SIM_procThread(void);
void SIM_pxiInit(void);
void * SIM_Render(void *arg);
void * SIM_RenderInit(void * arg);
void SIM_SetInternalResolution(int internalRes);
void SIM_SetInternalResolutionAfterRender(int internalRes);
int SIM_runNitroOSThread(void * thread);
void * SIM_sndAlarm(void * arg);
void SIM_u16ToRGB( u16 in, u8 * r, u8 *g, u8 * b );
SIM_config_type * SIM_GetConfigPtr();
u64 SIM_GetRenderFrameTime();
u64 SIM_GetFullFrameTime();

// Where each frame's time goes, in milliseconds (smoothed over recent frames)
typedef struct SIM_FrameTimings {
    float gameMs;      // game logic between frames (excluding 3D draw submission)
    float g3SubmitMs;  // 3D draw calls issued by the game (texture lookup, uploads, GL)
    float drawMs;      // compositing both screens, GUI
    float capWaitMs;   // frame limiter sleep
    float swapMs;      // buffer swap (vsync wait, driver)

    // Parts of drawMs
    float g2Ms;        // 2D layers (sprites, backgrounds) drawn on the CPU
    float uploadMs;    // 2D layers sent to the GPU
    float secondaryMs; // the second display's output (DS screen, companion UI), CPU side
    float companionMs;     // part of it: the companion UI
    float secondarySwapMs; // part of it: showing it (buffer swap, back to the main window)
    float guiMs;       // menus and overlays (Dear ImGui)

    // GPU time per shown frame, from timer queries (negative: not available)
    float gpu3dMs;        // the 3D scene
    float gpuMainMs;      // the main window
    float gpuSecondaryMs; // the second display

    float shownPerSecond;   // frames shown
    float vblanksPerSecond; // DS frames run (60 at full speed)
    float draws3d;          // 3D draw calls per DS frame
    float uploads2d;        // 2D layer uploads per shown frame (unchanged layers are skipped)
} SIM_FrameTimings;
void SIM_GetFrameTimings(SIM_FrameTimings *out);
// The game will replace the next vblank's frame before it can be seen (its logic runs at
// 30 Hz: the frame after the next one is the one that matters), so it isn't drawn or
// shown. The game still runs with its input and vblank handlers as usual.
void SIM_SkipNextFrame(void);

// The 3D scene's scale in use (DS screens), and anti-aliasing: the most samples the GPU
// takes (0: not available) and the samples in use
int SIM_GetInternalScale(void);
int SIM_AntiAliasingMaxSamples(void);
int SIM_AntiAliasingSamples(void);

// Widescreen (the Widescreen setting, experimental): with the top screen on its own display,
// the scenes the game marks as able to (SIM_SetWidescreenScene, every frame) show the 3D
// scene as wide as that display, with the 2D layers kept 4:3 in the middle. While
// SIM_IsWidescreenActive(), the game widens its camera's projection by SIM_WidescreenRatio()
// (the 3D scene's width over the DS screen's). Both change only between frames.
void SIM_SetWidescreenScene(BOOL wide);
BOOL SIM_IsWidescreenActive(void);
float SIM_WidescreenRatio(void);

// The DS clock (RTC) the game reads: the device's local time plus this many seconds
void SIM_SetClockOffset(long long seconds);
long long SIM_GetClockOffset(void);

// Fast-forward: the game runs several DS frames per frame shown (FastForwardSpeed times
// the normal speed, or as fast as it goes). On while its button is held, or switched on
// and off by it (FastForwardToggle), or by the game (SIM_SetFastForward, e.g. a button
// in its own UI).
BOOL SIM_IsFastForwarding(void);
void SIM_SetFastForward(BOOL on);
const char * SIM_GetLibntrGitHash();
const char * SIM_GetProjectGitHash();
SDL_Window * SIM_GetSDLWindow();

extern GXVRamTex s_SIM_GXVRamTex;
extern GXVRamTexPltt s_SIM_GXVRamTexPltt;
extern GXVRamBGExtPltt s_SIM_GXBgExtPltt;
extern GXVRamSubBGExtPltt s_SIM_GXSubBgExtPltt;

extern SNDSharedWork * win_SNDi_SharedWork;

extern void * win_snd_channel_bufs[32];
extern WIN_snd_channel_info win_snd_channels[32];
extern WIN_snd_channel_info win_sndi_channels[16];
extern WIN_snd_alarm_info win_snd_alarms[32];

#ifdef __cplusplus
}
#endif

#endif

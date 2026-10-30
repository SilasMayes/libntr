extern "C" void NitroMain();
extern "C" int NitroSpMain(void *arg);
#ifndef SDK_BUILD_ANDROID
// On Android, SDL's Java activity calls SDL_main(): let SDL_main.h rename main() below.
#define SDL_MAIN_HANDLED
#endif

#include <string>
#define _POSIX_C_SOURCE 199309L
#include <pthread.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_events.h>

#include <simulator/sim_opengl.h>
#include <simulator/sim.h>
#include <simulator/sim_audio.h>
#include <simulator/sim_net.h>
#include <simulator/sim_shaders.hpp>

#include <stdio.h>
#include <stdlib.h>

#include <nitro/gx/g3.h>
#include <nitro/gx/gx_load.h>
#include <nitro/gx/gx_vramcnt.h>
#include <nitro/hw/X86/ioreg.h>
#include <nitro/hw/X86/mmap_shared.h>
#include <nitro/os.h>
#include <nitro/pxi/common/fifo.h>

#include <simulator/config/sim_config.h>
#include <simulator/drawmsg.h>
#include <simulator/queue.h>
#include <simulator/simvariables.h>
#include <simulator/g3_draw.h>
#include <simulator/g3_handler.h>
#include <simulator/sim_gui.hpp>
#include <simulator/sim_settings.hpp>
#include <simulator/sim_display.h>
#include <simulator/sim_lifecycle.h>
#include <simulator/sim_companion.h>
#include <simulator/sim_carts.h>
#ifdef SDK_BUILD_ANDROID
#include <simulator/sim_android.h>
#endif

#include <math.h>
#include <time.h>

#include "screenquads.h"

#ifdef SDK_BUILD_WIN64
#include <windows.h>
#include <winsock2.h>
#endif

#ifdef SDK_BUILD_LINUX
#include <signal.h>
#endif

#ifdef SDK_BUILD_NX
#include "switch.h"
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

#include "sim_g2.h"
#include "git_info.h"

// Simulator configuration
SIM_config_type s_SIM_config = {};

// Thread
extern OSThreadInfo OSi_ThreadInfo;

static u32 s_TouchPanelCoords[4] = {}; // The bounding box of the touch screen

// SND
#include <nitro/snd/common/work.h>
#include <nitro/snd/common/command.h>

#ifdef SDK_BUILD_WIN64
#define NANOTIME_IMPLEMENTATION
#include "nanotime.h"
#endif

#include <nitro/wm/ARM7/wm_sp.h>

// Graphics Debug Variables
u8 s_SIM_DBG_BGenable[4] = {1, 1, 1, 1};
u8 s_SIM_DBG_OAMenable = 1;
u8 s_SIM_DBG_BGSenable[4] = {1, 1, 1, 1};
u8 s_SIM_DBG_OAMSenable = 1;

u8 s_SIM_useWBuffer = 0;

static struct timespec s_SIM_lastFrameEnd;
static u64 sRenderFrameTime;
static u64 sFullFrameTime;

// Per-frame time breakdown (smoothed), shown in the libntr window
static SIM_FrameTimings sTimings;
static u64 sRenderStartNs;
static u64 sBeforeWaitNs;

static u64 SIM_NowNs() {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (u64)now.tv_sec * 1000000000ull + (u64)now.tv_nsec;
}

static void SIM_Smooth(float *value, u64 ns) {
  const float ms = (float)ns / 1000000.0f;
  *value = (*value == 0.0f) ? ms : (*value * 0.9f + ms * 0.1f);
}

static void SIM_SmoothValue(float *value, float sample) {
  *value = (*value == 0.0f) ? sample : (*value * 0.9f + sample * 0.1f);
}

extern "C" void SIM_GetFrameTimings(SIM_FrameTimings *out) {
  *out = sTimings;
}

// A frame the game replaces before it can be seen isn't drawn (see sim.h)
static bool sSkipNextFrame = false;

// Fast-forward (see sim.h)
static bool sFastForwardHeld = false;    // a hold button is down
static bool sFastForwardOn = false;      // switched on (toggle button, or the game)
static bool sFastForwardJoyDown = false; // the controller's button (axes repeat their down events)
static bool sFastForwardSwap = false;    // the swap interval is 0 for it
static u64 sNextVBlankNs = 0;            // fast-forward pacing
static u64 sLastPresentNs = 0;

extern "C" BOOL SIM_IsFastForwarding(void) {
  return (sFastForwardHeld || sFastForwardOn) ? TRUE : FALSE;
}

extern "C" void SIM_SetFastForward(BOOL on) {
  sFastForwardOn = on != FALSE;
}

// The fast-forward button: held, or pressed to switch it on and off (FastForwardToggle)
static void FastForwardButton(bool down) {
  if (SIM_GetConfigPtr()->fastForwardToggle) {
    if (down) {
      sFastForwardOn = !sFastForwardOn;
    }
  } else {
    sFastForwardHeld = down;
  }
}

static void PreciseSleepNs(u64 ns) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ns / 1000000000ull);
  ts.tv_nsec = (long)(ns % 1000000000ull);
  while (nanosleep(&ts, &ts) != 0) {
  }
}

// Fast-forward pacing: one DS frame every period (from a running deadline, so short
// sleeps don't add up to a slower speed)
static void PaceVBlank(u64 period) {
  const u64 now = SIM_NowNs();
  if (sNextVBlankNs == 0 || now > sNextVBlankNs + 100000000ull) {
    sNextVBlankNs = now; // just started, or fell far behind: no catching up
  }
  sNextVBlankNs += period;
  if (sNextVBlankNs > now) {
    PreciseSleepNs(sNextVBlankNs - now);
  }
}

extern "C" void SIM_SkipNextFrame(void) {
  sSkipNextFrame = true;
}

// Frames shown and DS frames run per second
static u32 sShownThisSecond = 0;
static u32 sVBlanksThisSecond = 0;
static u64 sRateWindowStartNs = 0;

static void UpdateRates() {
  const u64 now = SIM_NowNs();
  if (sRateWindowStartNs == 0) {
    sRateWindowStartNs = now;
  }
  const u64 elapsed = now - sRateWindowStartNs;
  if (elapsed >= 1000000000ull) {
    const float seconds = (float)elapsed / 1e9f;
    sTimings.shownPerSecond = (float)sShownThisSecond / seconds;
    sTimings.vblanksPerSecond = (float)sVBlanksThisSecond / seconds;
    sTimings.draws3d = sVBlanksThisSecond > 0 ? (float)G3SIM_TakeDrawCount() / (float)sVBlanksThisSecond : 0.0f;
    sShownThisSecond = 0;
    sVBlanksThisSecond = 0;
    sRateWindowStartNs = now;
  }
}

// GPU time of the parts of a frame, from timer queries read back frames later (it never
// waits for the GPU). Desktop GL has them; OpenGL ES with GL_EXT_disjoint_timer_query.
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
enum GpuPart { GPU_3D, GPU_MAIN, GPU_SECONDARY, GPU_PART_COUNT };
static constexpr int kGpuQueryRing = 8;
static bool sGpuTimers = false;
static GLuint sGpuQueries[GPU_PART_COUNT][kGpuQueryRing];
static bool sGpuQueryPending[GPU_PART_COUNT][kGpuQueryRing];
static int sGpuQueryNext[GPU_PART_COUNT];
static int sGpuActivePart = -1;

static void GpuTimersInit() {
  sTimings.gpu3dMs = sTimings.gpuMainMs = sTimings.gpuSecondaryMs = -1.0f;
#ifdef SIM_GLES
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count && !sGpuTimers; i++) {
    const char *name = (const char *)glGetStringi(GL_EXTENSIONS, i);
    sGpuTimers = name != nullptr && strcmp(name, "GL_EXT_disjoint_timer_query") == 0;
  }
#else
  sGpuTimers = true; // OpenGL 3.3+
#endif
  if (sGpuTimers) {
    glGenQueries(GPU_PART_COUNT * kGpuQueryRing, &sGpuQueries[0][0]);
    sTimings.gpu3dMs = sTimings.gpuMainMs = sTimings.gpuSecondaryMs = 0.0f;
  }
  while (glGetError() != GL_NO_ERROR) {
  }
}

static void GpuTimerBegin(GpuPart part) {
  if (!sGpuTimers || sGpuActivePart >= 0) {
    return;
  }
  const int slot = sGpuQueryNext[part];
  if (sGpuQueryPending[part][slot]) {
    return; // that query's result isn't back yet: this frame goes unmeasured
  }
  glBeginQuery(GL_TIME_ELAPSED, sGpuQueries[part][slot]);
  sGpuActivePart = part;
}

static void GpuTimerEnd() {
  if (sGpuActivePart < 0) {
    return;
  }
  glEndQuery(GL_TIME_ELAPSED);
  const int part = sGpuActivePart;
  const int slot = sGpuQueryNext[part];
  sGpuQueryPending[part][slot] = true;
  sGpuQueryNext[part] = (slot + 1) % kGpuQueryRing;
  sGpuActivePart = -1;
}

static void GpuTimersCollect() {
  if (!sGpuTimers) {
    return;
  }
  GLint disjoint = 0;
#ifdef SIM_GLES
  glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
#endif
  float *targets[GPU_PART_COUNT] = {&sTimings.gpu3dMs, &sTimings.gpuMainMs, &sTimings.gpuSecondaryMs};
  for (int part = 0; part < GPU_PART_COUNT; part++) {
    for (int slot = 0; slot < kGpuQueryRing; slot++) {
      if (!sGpuQueryPending[part][slot]) {
        continue;
      }
      GLuint available = 0;
      glGetQueryObjectuiv(sGpuQueries[part][slot], GL_QUERY_RESULT_AVAILABLE, &available);
      if (!available) {
        continue;
      }
      GLuint ns = 0;
      glGetQueryObjectuiv(sGpuQueries[part][slot], GL_QUERY_RESULT, &ns);
      sGpuQueryPending[part][slot] = false;
      if (!disjoint) {
        SIM_Smooth(targets[part], ns);
      }
    }
  }
}

static constexpr auto SteamDeckWidth = 1280;
static constexpr auto SteamDeckHeight = 800;

u8 bg0tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg1tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg2tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg3tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg0stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg1stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg2stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg3stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];

u8 oam0Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam1Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam2Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam3Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam0sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam1sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam2sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam3sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 objWindowTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 objWindowsTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];

u32 OBJLine[2][256];
u8 OBJWindow[2][256];

GXVRamTex s_SIM_GXVRamTex;
GXVRamTexPltt s_SIM_GXVRamTexPltt;
GXVRamBGExtPltt s_SIM_GXBgExtPltt;
GXVRamSubBGExtPltt s_SIM_GXSubBgExtPltt;

// 4MB! can we reduce this?
u8 s_SIM_g3tex[4 * 1024 * 1024];

// FBOs
static GLuint g3FrameBuffer;
static GLuint fboId;
static GLuint g2FrameBuffer;

// Texture Objects (the 2D layers' are in sLayerSets)
static GLuint g2TextureId;
static GLuint g3RenderTextureId;
static GLuint dummyScreenTextureId;

GLint s_bgTexUnits[] = {0, 1, 2, 3};
static GLint s_objTexUnits[] = {4, 5, 6, 7, 8};
static GLint s_objWindowTexUnit = 9;

// Shader Programs
GLuint g3shaderProgramID;
static GLuint g2shaderProgramID;

// VAOs
static GLuint g2vertexArray;

// VBOs
static GLuint g2vertexBuffer;

// RBOs
static GLuint g3RenderBufferId;

// Shaders
static GLint g2VertexShader;
static GLint g2FragmentShader;
static GLint myVertexShader;
static GLint myFragmentShader;

// GL Context
static SDL_GLContext context;

static SDL_Window *window;

u8 s_SIM_dbg_dumpSound = 0;
static u8 s_SIM_dbg_soundFileOpen = 0;
static FILE *s_SIM_dbg_soundFile = NULL;
u8 s_SIM_dbg_soundSoloChannel = 0xFF;
void *win_snd_channel_bufs[32] = {0};
WIN_snd_channel_info win_snd_channels[32];
WIN_snd_channel_info win_sndi_channels[16] = {0};
WIN_snd_alarm_info win_snd_alarms[32] = {0};
SNDSharedWork *win_SNDi_SharedWork;


OSIrqFunction OS_IRQTable[OS_IRQ_TABLE_MAX] = {
    OS_IrqDummy, // VBlank (for ARM9)
    OS_IrqDummy, // HBlank
    OS_IrqDummy, // VCounter
    OS_IrqDummy, // timer0
    OS_IrqDummy, // timer1
    OS_IrqDummy, // timer2
    OS_IrqDummy, // timer3
    OS_IrqDummy, // serial communication (will not occur)
    OS_IrqDummy, // DMA0
    OS_IrqDummy, // DMA1
    OS_IrqDummy, // DMA2
    OS_IrqDummy, // DMA3
    OS_IrqDummy, // key
    OS_IrqDummy, // cartridge
    OS_IrqDummy, // (not used)
    OS_IrqDummy, // (not used)
    OS_IrqDummy, // sub processor
    OS_IrqDummy, // sub processor send FIFO empty
    OS_IrqDummy, // sub processor receive FIFO not empty
    OS_IrqDummy, // card data transfer finish
    OS_IrqDummy, // card IREQ
    OS_IrqDummy, // geometry command FIFO
    OS_IrqDummy, // Power Management IC
    OS_IrqDummy, // SPI data transfer
    OS_IrqDummy  // Wireless module
};

u8 s_mosaicTable[16][256];

static u8 isMouseDown = 0;

// Local Procedure Declarations
static void ConvertMsgParams(draw_msg_t *msg);
static u8 GetNumParamsFromCommandType(draw_command_type_t type);
static u8 GetParamSizeFromCommandType(draw_command_type_t type);
static u8 isParamSigned(draw_command_type_t type);
static u64 map(u64 x, u64 in_min, u64 in_max, u64 out_min, u64 out_max);
static void process_nibble(unsigned char data4bit, int *Index, int *Pcm16bit);
static void DrawScreenQuad();
static void DrawEngine(BOOL isSub);

// The 2D layer textures hold both screens (256x384); each engine only draws into the
// half for the screen it is shown on, and only that half is uploaded.
extern "C" int SIM_EngineTextureRow(BOOL isSub) {
  const BOOL topScreen = (s_reg_GX_POWCNT >> 15) ? !isSub : isSub;
  return topScreen ? SIM_NDS_SCREEN_HEIGHT : 0;
}

// Each engine has its own textures for its layers (4 backgrounds, 4 sprite priorities and
// the sprite window), in a ring of sets used one frame after another. A set is rewritten
// only frames after the GPU last drew with it, and the engines never share a texture, so
// an upload never has to wait for (or make the driver copy) a texture the GPU is still
// reading, which tiled mobile GPUs pay dearly for. A layer that is the same as what the
// set already holds isn't uploaded at all.
enum {
  LAYER_OBJ0 = 4,
  LAYER_OBJ_WINDOW = 8,
  LAYER_COUNT = 9,
};
static constexpr int kLayerRing = 3;
static constexpr size_t kLayerHalfBytes = 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT;

struct LayerSet {
  GLuint texture[LAYER_COUNT];
  u64 hash[LAYER_COUNT];
  int row[LAYER_COUNT]; // the half the hash is of; -1: unknown
};
static LayerSet sLayerSets[2][kLayerRing];
static int sLayerRingIndex = 0;
static u64 sEmptyLayerHash = 0;

// Profile of the frame being drawn
static u64 sFrameUploadNs = 0;
static u32 sFrameUploads = 0;
static u64 sFrameG2Ns = 0;

// Fast 64-bit hash of a layer half (four independent lanes; data is 8-byte aligned)
static u64 HashLayerHalf(const u8 *data) {
  const u64 *words = (const u64 *)data;
  const size_t count = kLayerHalfBytes / sizeof(u64);
  const u64 k = 0x9E3779B97F4A7C15ull;
  u64 h0 = 0x243F6A8885A308D3ull, h1 = 0x13198A2E03707344ull, h2 = 0xA4093822299F31D0ull,
      h3 = 0x082EFA98EC4E6C89ull;
  for (size_t i = 0; i < count; i += 4) {
    h0 = (h0 ^ words[i]) * k;
    h1 = (h1 ^ words[i + 1]) * k;
    h2 = (h2 ^ words[i + 2]) * k;
    h3 = (h3 ^ words[i + 3]) * k;
    h0 ^= h0 >> 29;
    h1 ^= h1 >> 29;
    h2 ^= h2 >> 29;
    h3 ^= h3 >> 29;
  }
  u64 h = h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7);
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDull;
  h ^= h >> 33;
  return h;
}

static void CreateLayerTextures() {
  static u8 zeros[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
  sEmptyLayerHash = HashLayerHalf(zeros);
  glActiveTexture(GL_TEXTURE0);
  for (int engine = 0; engine < 2; engine++) {
    for (int r = 0; r < kLayerRing; r++) {
      LayerSet &set = sLayerSets[engine][r];
      glGenTextures(LAYER_COUNT, set.texture);
      for (int layer = 0; layer < LAYER_COUNT; layer++) {
        glBindTexture(GL_TEXTURE_2D, set.texture[layer]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        // Both halves start empty (the other half is never sampled, but stays defined)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SIM_NDS_SCREEN_WIDTH, SIM_NDS_SCREEN_HEIGHT * 2, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, zeros);
        set.hash[layer] = sEmptyLayerHash;
        set.row[layer] = -1;
      }
    }
  }
}

extern "C" GLuint SIM_LayerTexture(BOOL isSub, int layer) {
  return sLayerSets[isSub ? 1 : 0][sLayerRingIndex].texture[layer];
}

// Binds the engine's texture for the layer to the active texture unit, with the half of
// fullBuffer for the engine's screen in it (knownEmpty: that half is all zero)
extern "C" void SIM_UploadLayer(BOOL isSub, int layer, const u8 *fullBuffer, BOOL knownEmpty) {
  const u64 start = SIM_NowNs();
  LayerSet &set = sLayerSets[isSub ? 1 : 0][sLayerRingIndex];
  const int row = SIM_EngineTextureRow(isSub);
  const u8 *half = fullBuffer + 4 * SIM_NDS_SCREEN_WIDTH * row;
  const u64 hash = knownEmpty ? sEmptyLayerHash : HashLayerHalf(half);
  glBindTexture(GL_TEXTURE_2D, set.texture[layer]);
  if (set.row[layer] != row || set.hash[layer] != hash) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, SIM_NDS_SCREEN_WIDTH, SIM_NDS_SCREEN_HEIGHT, GL_RGBA,
                    GL_UNSIGNED_BYTE, half);
    set.row[layer] = row;
    set.hash[layer] = hash;
    sFrameUploads++;
  }
  sFrameUploadNs += SIM_NowNs() - start;
}

static void NextLayerSet() {
  sLayerRingIndex = (sLayerRingIndex + 1) % kLayerRing;
}

static u8 s_numG3CommandsThisFrame = 0;

// Vcount stuff
static SDL_sem *vcountResetSemaphore;
static SDL_sem *vcountVblankSemaphore;

SDL_Joystick *s_SIM_SDLJoystick = NULL;

// Which single DS screen DrawScreenQuad() should draw when each screen has its own output
enum SingleScreenQuad {
  SINGLE_SCREEN_NONE = -1, // use the configured multi-screen layout
  SINGLE_SCREEN_TOP = 0,
  SINGLE_SCREEN_BOTTOM = 1,
};
static SingleScreenQuad s_SingleScreenQuad = SINGLE_SCREEN_NONE;

// Touch panel rectangle on the secondary output, in its drawable's pixels
static int s_SecondaryTouchRect[4] = {};
// The DS screen region the touch rect shows (all of it, unless the companion embeds a part)
static int s_SecondaryTouchSrc[4] = {0, 0, SIM_NDS_SCREEN_WIDTH, SIM_NDS_SCREEN_HEIGHT};

// Who owns the finger currently down on the secondary output: the DS touch screen or
// the companion UI next to it (decided where the touch starts)
enum SecondaryTouchOwner { TOUCH_OWNER_NONE, TOUCH_OWNER_DS, TOUCH_OWNER_COMPANION };
static SecondaryTouchOwner s_SecondaryTouchOwner = TOUCH_OWNER_NONE;
static bool s_SecondaryDsTouchDown = false;
static int s_SecondaryDsTouchX = 0;
static int s_SecondaryDsTouchY = 0;
// Was the bottom screen on the secondary output last frame?
static bool s_WasDualScreen = false;

// Fits a DS-shaped rectangle of the given aspect ratio into a drawable, centered.
static void FitViewport(int drawableWidth, int drawableHeight, float aspectRatio,
                        int *viewX, int *viewY, int *viewWidth,
                        int *viewHeight) {
  *viewX = 0;
  *viewWidth = drawableWidth;
  *viewHeight = (int)((float)drawableWidth / aspectRatio);
  *viewY = (drawableHeight - *viewHeight) / 2;
  if (*viewHeight > drawableHeight) {
    // Too tall to fit. Scale down the width instead
    *viewHeight = drawableHeight;
    *viewWidth = (int)((float)*viewHeight * aspectRatio);
    *viewX = (drawableWidth - *viewWidth) / 2;
    *viewY = 0;
  }
}

static void DrawScreenQuad() {
#ifdef SDK_TRACY_ENABLE
  TracyCZone(ctx, 1);
#endif
  glBindVertexArray(g2vertexArray);
  glBindBuffer(GL_ARRAY_BUFFER, g2vertexBuffer);
  G3SIM_Vertex_t *quadArray;
  int arrayCount;
  if (s_SingleScreenQuad != SINGLE_SCREEN_NONE) {
    // Separate outputs: one DS screen fills the whole viewport
    quadArray = sim_GetSingleScreenQuadArray(s_SingleScreenQuad ==
                                             SINGLE_SCREEN_BOTTOM);
    arrayCount = sim_GetSingleScreenQuadVertexCount();
  } else {
    quadArray = sim_GetScreenQuadArray(s_SIM_config.screenLayout,
                                       s_SIM_config.swapScreens);
    arrayCount = sim_GetScreenQuadVertexCount(s_SIM_config.screenLayout,
                                              s_SIM_config.swapScreens);
  }

  glBufferData(GL_ARRAY_BUFFER, sizeof(G3SIM_Vertex_t) * arrayCount, quadArray,
               GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t),
                        (void *)offsetof(G3SIM_Vertex_t, x));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t),
                        (void *)offsetof(G3SIM_Vertex_t, s));

  glDrawArrays(GL_TRIANGLES, 0, arrayCount);
#ifdef SDK_TRACY_ENABLE
  TracyCZoneEnd(ctx);
#endif
}

SIM_config_type *SIM_GetConfigPtr() { return &s_SIM_config; }

SDL_Window * SIM_GetSDLWindow() {return window;}

static u64 map(u64 x, u64 in_min, u64 in_max, u64 out_min, u64 out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void SIM_u16ToRGB(u16 in, u8 *r, u8 *g, u8 *b) {
  *r = ( (in & 0x1F) * 527 + 23 ) >> 6;
  *g = ( ((in >> 5) & 0x1F) * 527 + 23 ) >> 6;
  *b = ( ((in >> 10) & 0x1F) * 527 + 23 ) >> 6;
}

void *runVblankInterrupt(void *arg) {
  OS_IRQTable[OS_IE_V_BLANK - 1]();
  return NULL;
}

void GLAPIENTRY MessageCallback(GLenum source, GLenum type, GLuint id,
                                GLenum severity, GLsizei length,
                                const GLchar *message, const void *userParam) {
  // fprintf( stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message =
  // %s\n",
  //          ( type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : "" ),
  //           type, severity, message );
}

// The 3D scene's scale in use: the setting, or with "match the screen" (0) the smallest
// that is at least as sharp as the screen showing it (from the last frame's layout)
static int sScale = 1;
static int sAutoScale = 5;

static int WantedScale() {
  const int setting = (int)s_SIM_config.internalResolutionScale;
  if (setting > 0) {
    return setting > 20 ? 20 : setting;
  }
  return sAutoScale;
}

extern "C" int SIM_GetInternalScale(void) {
  return sScale;
}

// Anti-aliasing on mobile GPUs (GL_EXT_multisampled_render_to_texture): the samples stay
// in the GPU's tile memory and are averaged as the tile is written to the 3D texture, so
// nothing else changes and it costs little
#ifdef SIM_GLES
typedef void(GL_APIENTRYP FramebufferTexture2DMultisampleFn)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
typedef void(GL_APIENTRYP RenderbufferStorageMultisampleFn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static FramebufferTexture2DMultisampleFn sFramebufferTexture2DMultisample = nullptr;
static RenderbufferStorageMultisampleFn sRenderbufferStorageMultisample = nullptr;
static int sMaxSamples = 0;
#endif
static int sSamples = 0; // in use

static void FindAntiAliasing() {
#ifdef SIM_GLES
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  bool found = false;
  for (GLint i = 0; i < count && !found; i++) {
    const char *name = (const char *)glGetStringi(GL_EXTENSIONS, i);
    found = name != nullptr && strcmp(name, "GL_EXT_multisampled_render_to_texture") == 0;
  }
  if (found) {
    sFramebufferTexture2DMultisample =
        (FramebufferTexture2DMultisampleFn)SDL_GL_GetProcAddress("glFramebufferTexture2DMultisampleEXT");
    sRenderbufferStorageMultisample =
        (RenderbufferStorageMultisampleFn)SDL_GL_GetProcAddress("glRenderbufferStorageMultisampleEXT");
    if (sFramebufferTexture2DMultisample != nullptr && sRenderbufferStorageMultisample != nullptr) {
      glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES_EXT */, &sMaxSamples);
    }
  }
  printf("3D anti-aliasing: %s (up to %d samples)\n", sMaxSamples > 1 ? "available" : "not available", sMaxSamples);
#endif
}

extern "C" int SIM_AntiAliasingMaxSamples(void) {
#ifdef SIM_GLES
  return sMaxSamples;
#else
  return 0;
#endif
}

extern "C" int SIM_AntiAliasingSamples(void) {
  return sSamples;
}

// Widescreen: the 3D framebuffer is as wide as the top display (sWideWidth, in DS pixels at
// the DS screen's height) while the setting is on; the scenes the game marks get a 3D
// viewport that wide (latched at the end of SIM_Render, for the 3D drawn until the next one)
static BOOL sWideRequested = FALSE;
static bool sWideActive = false;
static int sWide3dWidth = SIM_NDS_SCREEN_WIDTH; // the 3D being drawn: its width
static int sWideWidth = SIM_NDS_SCREEN_WIDTH;   // the top display's
static int sFboWidth = SIM_NDS_SCREEN_WIDTH;    // the 3D framebuffer's

// How the wide scene is made (SIM_SetWidescreenSceneOptions), as asked and as latched
static bool sWideScale3DRequested = false;
static u8 sWideBgRequested = 0;
static bool sWideScale3D = false; // libntr narrows the 3D to keep its 4:3 shapes
static u8 sWideBgLayers = 0;      // main-engine BG layers drawn across the whole width

extern "C" void SIM_SetWidescreenScene(BOOL wide) {
  sWideRequested = wide;
}

extern "C" void SIM_SetWidescreenSceneOptions(BOOL scale3D, u8 wideBgLayers) {
  sWideScale3DRequested = scale3D != FALSE;
  sWideBgRequested = wideBgLayers & 0x0F;
}

extern "C" BOOL SIM_IsWidescreenActive(void) {
  return sWideActive ? TRUE : FALSE;
}

extern "C" float SIM_WidescreenRatio(void) {
  return sWideActive ? (float)sWide3dWidth / (float)SIM_NDS_SCREEN_WIDTH : 1.0f;
}

// (Re)creates the 3D framebuffer's storage at the wanted scale and anti-aliasing; leaves
// the 3D framebuffer bound
void SIM_SetInternalResolution(int internalRes) {
  (void)internalRes;
  sScale = WantedScale();
  sFboWidth = s_SIM_config.widescreen ? sWideWidth : SIM_NDS_SCREEN_WIDTH;
  const int width = sFboWidth * sScale;
  const int height = SIM_NDS_SCREEN_HEIGHT * 2 * sScale;

  glBindTexture(GL_TEXTURE_2D, g3RenderTextureId);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  glBindFramebuffer(GL_FRAMEBUFFER, g3FrameBuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, g3RenderBufferId);
  sSamples = 0;
#ifdef SIM_GLES
  int samples = s_SIM_config.antiAliasing;
  if (samples > sMaxSamples) {
    samples = sMaxSamples;
  }
  if (samples > 1) {
    sRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
    sFramebufferTexture2DMultisample(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g3RenderTextureId, 0, samples);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g3RenderBufferId);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      sSamples = samples;
    } else {
      printf("3D anti-aliasing: the framebuffer wasn't complete with %d samples, off\n", samples);
    }
  }
#endif
  if (sSamples == 0) {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g3RenderTextureId, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g3RenderBufferId);
  }
  // A new size starts empty
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static bool sChangeInternalResAfterRender = false;
static int sPendingInternalRes = 1;

void SIM_SetInternalResolutionAfterRender(int internalRes) {
  sChangeInternalResAfterRender = true;
  sPendingInternalRes = internalRes;
}

void *SIM_RenderInit(void *arg) {

  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
           SDL_INIT_JOYSTICK);
  SIM_Audio_Init(44100);

#ifdef SIM_GLES
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetSwapInterval(s_SIM_config.vsyncInterval);

  u32 windowHeight;
  u32 windowWidth;

  bool isSteamDeck = false;

  #ifdef SDK_BUILD_LINUX
  char * steamDeckEnv = getenv("SteamDeck");
  if(steamDeckEnv && (strcmp(steamDeckEnv, "1") == 0)) {
    isSteamDeck = true;
  }
  #endif

  if ((s_SIM_config.windowWidth == 0 || s_SIM_config.windowHeight == 0) ) {
    if(isSteamDeck) {
      windowHeight = SteamDeckHeight;
      windowWidth = SteamDeckWidth;
    } else {
      switch (s_SIM_config.screenLayout) {
      default:
      case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
        windowHeight = SIM_NDS_SCREEN_HEIGHT * 2 * 2;
        windowWidth = SIM_NDS_SCREEN_WIDTH * 2;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
        windowHeight = SIM_NDS_SCREEN_HEIGHT * 2;
        windowWidth = SIM_NDS_SCREEN_WIDTH * 2 * 2;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
        windowHeight = SIM_NDS_SCREEN_HEIGHT * 2 * 2;
        windowWidth = (SIM_NDS_SCREEN_WIDTH * 2 * 2) + (SIM_NDS_SCREEN_WIDTH * 2);
        break;
      }
    }
  } else {
    windowHeight = s_SIM_config.windowHeight;
    windowWidth = s_SIM_config.windowWidth;
  }

  std::string windowTitle = "libntr Application";

  CARDRomHeader *cardHeader = (CARDRomHeader *)(HW_ROM_HEADER_BUF);

  if (strlen(cardHeader->game_name) > 0) {
    char gameName[13] = {0};
    char gameCode[5] = {0};
    char makerCode[3] = {0};

    strncpy(gameName, cardHeader->game_name, 12);
    strncpy(gameCode, (char *)&cardHeader->game_code, 4);
    strncpy(makerCode, (char *)&cardHeader->maker_code, 2);

    windowTitle = std::string(gameName) + " [" + std::string(gameCode) +
                  std::string(makerCode) + "]";
  }

  Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
#ifdef SDK_BUILD_ANDROID
  // Android windows always cover the display; the activity decides orientation.
  windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

  window = SDL_CreateWindow(
      windowTitle.c_str(), 100, 100, windowWidth, windowHeight,
      windowFlags);
  context = SDL_GL_CreateContext(window);
  SDL_GL_SetSwapInterval(s_SIM_config.vsyncInterval);

  if(s_SIM_config.fullScreen) {
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  }

  #if !defined(SDK_BUILD_PROSPERO) && !defined(SIM_GLES)
  //OpenGL is static linked on PS5, and GLES is linked directly
  gladLoadGLLoader(SDL_GL_GetProcAddress);
  #endif

  SDL_GetWindowSize(window, (int *)&windowWidth, (int *)&windowHeight);

  glViewport(0, 0, windowWidth, windowHeight);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

  glEnable(GL_DEBUG_OUTPUT);
  glDebugMessageCallback(MessageCallback, 0);

  GLenum glErrCode;
  GLint status;
  GLint logSize = 0;

  GLchar *errorBuf;

  CreateLayerTextures();

#define GenerateTexture(_id, _width, _height)                                  \
  glGenTextures(1, &_id);                                                      \
  glActiveTexture(GL_TEXTURE0);                                                \
  glBindTexture(GL_TEXTURE_2D, _id);                                           \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);           \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);           \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);                \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);                \
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _width, _height, 0, GL_RGBA,        \
               GL_UNSIGNED_BYTE, NULL);

#define CompileShaderCommon(_id, _source)                                      \
  glShaderSource(_id, 1, &_source, NULL);                                      \
  glCompileShader(_id);                                                        \
  glGetShaderiv(_id, GL_COMPILE_STATUS, &status);                              \
  if (status != GL_TRUE) {                                                     \
    glGetShaderiv(_id, GL_INFO_LOG_LENGTH, &logSize);                          \
    errorBuf = static_cast<GLchar *>(malloc(logSize * sizeof(GLchar)));        \
    glGetShaderInfoLog(_id, logSize, &logSize, (GLchar *)errorBuf);            \
    printf("ERROR COMPILING _source SHADER:\n%s\n", errorBuf);                 \
    free(errorBuf);                                                            \
  }

#define CompileVertexShader(_id, _source)                                      \
  _id = glCreateShader(GL_VERTEX_SHADER);                                      \
  CompileShaderCommon(_id, _source);

#define CompileFragmentShader(_id, _source)                                    \
  _id = glCreateShader(GL_FRAGMENT_SHADER);                                    \
  CompileShaderCommon(_id, _source);

#define LinkShader(_id, _vertex, _fragment)                                    \
  glAttachShader(_id, _vertex);                                                \
  glAttachShader(_id, _fragment);                                              \
  glLinkProgram(_id);                                                          \
  glErrCode = glGetError();                                                    \
  glGetProgramiv(_id, GL_LINK_STATUS, &status);                                \
  if (status != GL_TRUE) {                                                     \
    glGetProgramiv(_id, GL_INFO_LOG_LENGTH, &logSize);                         \
    errorBuf = (GLchar *)malloc(logSize * sizeof(GLchar));                     \
    glGetProgramInfoLog(_id, logSize, &logSize, (GLchar *)errorBuf);           \
    glErrCode = glGetError();                                                  \
    printf("ERROR LINKING _id SHADER:\n%s\n", errorBuf);                       \
    free(errorBuf);                                                            \
  }                                                                            \
  glValidateProgram(_id);                                                      \
  glUseProgram(_id);

  GenerateTexture(dummyScreenTextureId, SIM_NDS_SCREEN_WIDTH,
                  SIM_NDS_SCREEN_HEIGHT * 2);

  // 3D Framebuffer render texture
  glGenFramebuffers(1, &g3FrameBuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, g3FrameBuffer);
  glGenTextures(1, &g3RenderTextureId);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g3RenderTextureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  // Clamped: the screen's edges must not blend in the opposite edge (or, with widescreen,
  // the unused part of the framebuffer)
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g3RenderTextureId, 0);
  glGenRenderbuffers(1, &g3RenderBufferId);
  glBindRenderbuffer(GL_RENDERBUFFER, g3RenderBufferId);
  FindAntiAliasing();
  SIM_SetInternalResolution(s_SIM_config.internalResolutionScale);

  // Check FBO status
  int fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
    printf("Framebuffer error: %d\n", fboStatus);
  }

  // Unbind all that stuff
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glBindTexture(GL_TEXTURE_2D, g2TextureId);

  glGenFramebuffers(1, &g2FrameBuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Shader sources are written for GLSL 4.20; adapt them to the active GL flavour.
  std::string g3VertexSrc = SIM::PrepareShaderSource(SIM::G3VertexShader);
  std::string g3FragmentSrc = SIM::PrepareShaderSource(SIM::G3FragmentShader);
  std::string g2VertexSrc = SIM::PrepareShaderSource(SIM::G2VertexShader);
  std::string g2FragmentSrc = SIM::PrepareShaderSource(SIM::G2FragmentShader);
  const char *g3VertexSrcPtr = g3VertexSrc.c_str();
  const char *g3FragmentSrcPtr = g3FragmentSrc.c_str();
  const char *g2VertexSrcPtr = g2VertexSrc.c_str();
  const char *g2FragmentSrcPtr = g2FragmentSrc.c_str();

  // Setup G3 Shader Program
  g3shaderProgramID = glCreateProgram();
  CompileVertexShader(myVertexShader, g3VertexSrcPtr);
  CompileFragmentShader(myFragmentShader, g3FragmentSrcPtr);
  LinkShader(g3shaderProgramID, myVertexShader, myFragmentShader);

  // Setup G2 Shader Program
  g2shaderProgramID = glCreateProgram();
  CompileVertexShader(g2VertexShader, g2VertexSrcPtr);
  CompileFragmentShader(g2FragmentShader, g2FragmentSrcPtr);
  LinkShader(g2shaderProgramID, g2VertexShader, g2FragmentShader);

  GLint texUnitLoc = glGetUniformLocation(g2shaderProgramID, "myTexture");
  glProgramUniform1i(g2shaderProgramID, texUnitLoc, 0);

  // Set up the texture units in the shader
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "bgTexture"), 4,
               s_bgTexUnits);
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "objTexture"), 5,
               s_objTexUnits);

  glGenVertexArrays(1, &g2vertexArray);
  glBindVertexArray(g2vertexArray);
  glGenBuffers(1, &g2vertexBuffer);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  SDL_GL_SwapWindow(window);

  // Setup ImGui
  SIM::GUI::Init(window, context);

  // Secondary screen output (second display on Android, second window on PC)
  SIM_Display_Init(window, context);

  GpuTimersInit();

  clock_gettime(CLOCK_MONOTONIC, &s_SIM_lastFrameEnd);

  return nullptr;
}

static void DrawEngine(BOOL isSub) {
  const char *zoneName;
  zoneName = isSub ? "DrawEngine (SUB)" : "DrawEngine (MAIN)";
#ifdef SDK_TRACY_ENABLE
  TracyCZone(ctx, 1);
  TracyCZoneName(ctx, zoneName, strlen(zoneName));
#endif
  u8 bg0as3d;
  u8 maxPriority;
  u8 maxPriorityBG;
  u8 bgEnabled[4];
  u8 bgMode;
  GLint bgOrder[4];
  GLint bgPriorities[4];

  u32 dispSel = s_reg_GX_POWCNT >> 15;

  // Get the enabled BGs
  u32 dispcnt = isSub ? s_reg_GXS_DB_DISPCNT : s_reg_GX_DISPCNT;

  bgEnabled[0] = (dispcnt >> 8) & 0x01;
  bg0as3d = (dispcnt >> 3) & 0x01;
  bgEnabled[1] = (dispcnt >> 8) & 0x02;
  bgEnabled[2] = (dispcnt >> 8) & 0x04;
  bgEnabled[3] = (dispcnt >> 8) & 0x08;

  // Get the graphics mode
  bgMode = (u8)(dispcnt & 0x7);

  // Get the backdrop color
  u8 backdropR;
  u8 backdropG;
  u8 backdropB;
  u16 backdropColor;
  backdropColor =
      isSub ? *(u16 *)&s_HW_DB_BG_PLTT[0] : *(u16 *)&s_HW_BG_PLTT[0];
  SIM_u16ToRGB(backdropColor, &backdropR, &backdropG, &backdropB);
  float backdropVec[4];

  backdropVec[0] = (float)backdropR / 255.0f;
  backdropVec[1] = (float)backdropG / 255.0f;
  backdropVec[2] = (float)backdropB / 255.0f;
  backdropVec[3] = 1.0f;

  glUniform1fv(glGetUniformLocation(g2shaderProgramID, "backdropColor"), 4,
               backdropVec);

  glProgramUniform4f(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "backdropColor"),
                     (float)backdropR / 255.0f, (float)backdropG / 255.0f,
                     (float)backdropB / 255.0f, 1.0f);

  // Get the BG priorities
  u16 bg0cnt = isSub ? s_reg_G2S_DB_BG0CNT : s_reg_G2_BG0CNT;
  u16 bg1cnt = isSub ? s_reg_G2S_DB_BG1CNT : s_reg_G2_BG1CNT;
  u16 bg2cnt = isSub ? s_reg_G2S_DB_BG2CNT : s_reg_G2_BG2CNT;
  u16 bg3cnt = isSub ? s_reg_G2S_DB_BG3CNT : s_reg_G2_BG3CNT;

  bgPriorities[0] = bg0cnt & (u16)3;
  bgPriorities[1] = bg1cnt & (u16)3;
  bgPriorities[2] = bg2cnt & (u16)3;
  bgPriorities[3] = bg3cnt & (u16)3;

  // Set up the BGPriorities in the shader
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "bgPriorities"), 4,
               bgPriorities);

  // Determine the order to draw the BGs
  bgOrder[0] = 0;
  bgOrder[1] = 1;
  bgOrder[2] = 2;
  bgOrder[3] = 3;

  for (int i = 0; i < 4; i++) {
    maxPriorityBG = 0;
    maxPriority = 0;
    for (int j = 0; j < 4; j++) {
      if (bgPriorities[j] == 0xFF) {
        continue;
      }
      if (bgPriorities[j] >= maxPriority) {
        maxPriorityBG = j;
        maxPriority = bgPriorities[j];
      }
    }
    bgOrder[i] = maxPriorityBG;
    bgPriorities[maxPriorityBG] = 0xFF;
  }

  // Set up the BGOrder in the shader
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "bgOrder"), 4, bgOrder);

  const u64 g2Start = SIM_NowNs();
  const u64 uploadNsBefore = sFrameUploadNs;
  const int half = SIM_EngineTextureRow(isSub);
  const size_t halfOffset = 4 * SIM_NDS_SCREEN_WIDTH * half;

  // Sprites: only the lines a sprite of that priority covers are worked on
  u8 *objBufs[4] = {isSub ? oam0sTex : oam0Tex, isSub ? oam1sTex : oam1Tex, isSub ? oam2sTex : oam2Tex,
                    isSub ? oam3sTex : oam3Tex};
  u8 *objWindowBuf = isSub ? objWindowsTex : objWindowTex;
  for (int i = 0; i < 4; i++) {
    memset(objBufs[i] + halfOffset, 0, kLayerHalfBytes);
  }
  memset(objWindowBuf + halfOffset, 0, kLayerHalfBytes);

  bool objLayerUsed[5] = {false, false, false, false, false};
  u8 objLines[4][SIM_NDS_SCREEN_HEIGHT];
  memset(objLines, 0, sizeof(objLines));
  if ((dispcnt & 0x1000) && (isSub ? s_SIM_DBG_OAMSenable : s_SIM_DBG_OAMenable)) {
    static const u8 kSpriteHeight[16] = {8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8};
    const u16 *oam = isSub ? (const u16 *)s_HW_DB_OAM : (const u16 *)s_HW_OAM;
    for (int sprite = 0; sprite < 128; sprite++) {
      const u16 *attrib = &oam[sprite * 4];
      const bool rotScale = (attrib[0] & 0x0100) != 0;
      if (!rotScale && (attrib[0] & 0x0200)) {
        continue; // hidden
      }
      const u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
      u32 height = kSpriteHeight[sizeparam];
      if (rotScale && (attrib[0] & 0x0200)) {
        height <<= 1; // double size
      }
      const u32 ypos = attrib[0] & 0xFF;
      const int priority = (attrib[2] >> 10) & 3;
      for (u32 y = 0; y < height; y++) {
        const u32 line = (ypos + y) & 0xFF;
        if (line < SIM_NDS_SCREEN_HEIGHT) {
          objLines[priority][line] = 1;
        }
      }
    }
  }

  const u16 *objPltt = isSub ? (const u16 *)s_HW_DB_OBJ_PLTT : (const u16 *)s_HW_OBJ_PLTT;
  for (int i = 0; i < SIM_NDS_SCREEN_HEIGHT; i++) {
    for (int bgnum = 0x0C00; bgnum >= 0x0000; bgnum -= 0x0400) {
      const int priority = bgnum >> 10;
      if (!objLines[priority][i]) {
        continue;
      }
      memset(OBJLine, 0, sizeof(OBJLine));
      memset(OBJWindow, 0, sizeof(OBJWindow));
      G2SIM_DrawOBJ(i, isSub, bgnum);
      u8 *pixelBuf = objBufs[priority];
      u8 r;
      u8 g;
      u8 b;
      for (int j = 0; j < 256; j++) {
        if (OBJLine[isSub][j]) {
          u8 colorIdx = OBJLine[isSub][j] & 0xFF;
          if (colorIdx) {
            u16 color = objPltt[colorIdx];
            SIM_u16ToRGB(color, &r, &g, &b);
            G2SIM_PlotPixel(pixelBuf, r, g, b, j, i, isSub);
            objLayerUsed[priority] = true;
          }
        }
        if (OBJWindow[isSub][j]) {
          G2SIM_PlotPixel(objWindowBuf, 255, 255, 255, j, i, isSub);
          objLayerUsed[4] = true;
        }
      }
    }
  }

  // Backgrounds (each binds and uploads its texture)
  u8 *bgBufs[4] = {isSub ? bg0stex : bg0tex, isSub ? bg1stex : bg1tex, isSub ? bg2stex : bg2tex,
                   isSub ? bg3stex : bg3tex};
  // Widescreen: the layers the game asked for span the whole top screen
  const bool wideEngine = sWideActive && !isSub && (dispSel ? !isSub : isSub);
  int wideBgMask = 0;
  for (int i = 0; i < 4; i++) {
    const int bg = bgOrder[i];
    glActiveTexture(GL_TEXTURE0 + s_bgTexUnits[bg]);
    if (bgEnabled[bg] && !(bg == 0 && bg0as3d)) {
      if (wideEngine && ((sWideBgLayers >> bg) & 1) && G2SIM_DrawBGWide((u8)bg, bgMode, sWide3dWidth)) {
        wideBgMask |= 1 << bg;
        continue;
      }
      memset(bgBufs[bg] + halfOffset, 0, kLayerHalfBytes);
      G2SIM_DrawBG(bg, bgMode, bg0as3d, isSub);
    } else if (bgEnabled[bg]) {
      glBindTexture(GL_TEXTURE_2D, g3RenderTextureId); // BG0 shows the 3D scene
    } else {
      glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
    }
  }

  // Sprite layers by priority, and the sprite window
  for (int i = 0; i < 4; i++) {
    glActiveTexture(GL_TEXTURE0 + s_objTexUnits[i]);
    SIM_UploadLayer(isSub, LAYER_OBJ0 + i, objBufs[i], objLayerUsed[i] ? FALSE : TRUE);
  }
  glActiveTexture(GL_TEXTURE0 + s_objWindowTexUnit);
  SIM_UploadLayer(isSub, LAYER_OBJ_WINDOW, objWindowBuf, objLayerUsed[4] ? FALSE : TRUE);
  glProgramUniform1i(
      g2shaderProgramID,
      glGetUniformLocation(g2shaderProgramID, "objWindowTexture"),
      s_objWindowTexUnit);

  sFrameG2Ns += (SIM_NowNs() - g2Start) - (sFrameUploadNs - uploadNsBefore);

  // Pass BLDCNT, BLDALPHA, and BLDY value to the shader
  u16 bldcnt = isSub ? s_reg_G2S_DB_BLDCNT : s_reg_G2_BLDCNT;
  u16 bldalpha = isSub ? s_reg_G2S_DB_BLDALPHA : s_reg_G2_BLDALPHA;
  u16 bldy = isSub ? s_reg_G2S_DB_BLDY : s_reg_G2_BLDY;
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bldcnt"), bldcnt);
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bldalpha"),
                     bldalpha);
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bldy"), bldy);

  int topScreen = dispSel ? !isSub : isSub;
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "topScreen"),
                     topScreen);

  // Pass DISPCNT and WINOUT to the shader
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "dispcnt"),
                     dispcnt);
  u16 winout = isSub ? s_reg_G2S_DB_WINOUT : s_reg_G2_WINOUT;
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "winout"), winout);

  // Pass bg0as3d to the shader
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bg0as3d"),
                     bg0as3d);

  // Widescreen: the 3D engine on the top screen shows the whole wide scene, its 2D layers in
  // the middle; otherwise the 3D scene is the left DS screen's width of its framebuffer
  const bool wideHere = sWideActive && !isSub && topScreen;
  glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "wideRatio"),
                     wideHere ? (float)sWide3dWidth / (float)SIM_NDS_SCREEN_WIDTH : 1.0f);
  glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "tex3dScaleS"),
                     (float)(wideHere ? sWide3dWidth : SIM_NDS_SCREEN_WIDTH) / (float)sFboWidth);
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "wideBgMask"),
                     wideHere ? wideBgMask : 0);
  glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "wideBgScaleS"),
                     (float)sWide3dWidth / 512.0f);

  // Pass master bright info to the shader
  float masterBrightFactor = isSub
                                 ? (float)(s_reg_GXS_DB_MASTER_BRIGHT & 0b11111)
                                 : (float)(s_reg_GX_MASTER_BRIGHT & 0b11111);
  if (masterBrightFactor > 16.0f) {
    masterBrightFactor = 16.0f;
  }
  glProgramUniform1f(
      g2shaderProgramID,
      glGetUniformLocation(g2shaderProgramID, "inMasterBrightFactor"),
      masterBrightFactor);
  int masterBrightMode = isSub ? ((s_reg_GXS_DB_MASTER_BRIGHT >> 14) & 3)
                               : ((s_reg_GX_MASTER_BRIGHT >> 14) & 3);
  glProgramUniform1i(
      g2shaderProgramID,
      glGetUniformLocation(g2shaderProgramID, "inMasterBrightMode"),
      masterBrightMode);

  glBindVertexArray(g2vertexArray);
  glBindBuffer(GL_ARRAY_BUFFER, g2vertexBuffer);
  DrawScreenQuad();
#ifdef SDK_TRACY_ENABLE
  TracyCZoneEnd(ctx);
#endif
}

static void HandleJoystickKeyDown(int aKey) {
  if (aKey == s_SIM_config.padSettings.fastForwardJoyKey && aKey != SIM_CONFIG_JOY_INVALID_KEY) {
    if (!sFastForwardJoyDown) {
      sFastForwardJoyDown = true;
      FastForwardButton(true);
    }
    return;
  }
  if (aKey == s_SIM_config.padSettings.aJoyKey) {
    // A Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111110;
  } else if (aKey == s_SIM_config.padSettings.bJoyKey) {
    // B Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111101;
  } else if (aKey == s_SIM_config.padSettings.xJoyKey) {
    // X Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF & 0b1111100000000000;
  } else if (aKey == s_SIM_config.padSettings.yJoyKey) {
    // y Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF & 0b1111010000000000;
  } else if (aKey == s_SIM_config.padSettings.upJoyKey) {
    // Dpad Up
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111110111111;
  } else if (aKey == s_SIM_config.padSettings.downJoyKey) {
    // Dpad Down
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111101111111;
  } else if (aKey == s_SIM_config.padSettings.leftJoyKey) {
    // Dpad Left
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111011111;
  } else if (aKey == s_SIM_config.padSettings.rightJoyKey) {
    // Dpad Right
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111101111;
  } else if (aKey == s_SIM_config.padSettings.lJoyKey) {
    // L
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111110111111111;
  } else if (aKey == s_SIM_config.padSettings.rJoyKey) {
    // R
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111011111111;
  } else if (aKey == s_SIM_config.padSettings.startJoyKey) {
    // Start button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111110111;
  } else if (aKey == s_SIM_config.padSettings.selectJoyKey) {
    // Select button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111011;
  }
#ifdef SDK_BUILD_NX
  else if (aKey == 8) {
    // Left trigger on NX toggles GUI
    SIM::GUI::Toggle();
  }
#endif
#ifdef SDK_BUILD_ANDROID
  else if (aKey == 8) {
    // Right stick click on Android handhelds toggles GUI
    SIM::GUI::Toggle();
  } else if (aKey == 7) {
    // Left stick click: companion UI full screen on the lower display (or back)
    SIM_Companion_ToggleFullScreen();
  }
#endif
}

#ifdef SDK_BUILD_ANDROID
// SDL reports a D-pad as hat motion on some Android controllers and as buttons 11-14
// (SDL_CONTROLLER_BUTTON_DPAD_*) on others. Treat the buttons like the configured hat.
static int TranslateAndroidDpadButton(int aKey) {
  switch (aKey) {
  case 11:
    return s_SIM_config.padSettings.upJoyKey;
  case 12:
    return s_SIM_config.padSettings.downJoyKey;
  case 13:
    return s_SIM_config.padSettings.leftJoyKey;
  case 14:
    return s_SIM_config.padSettings.rightJoyKey;
  default:
    return aKey;
  }
}
#endif

static void HandleJoystickKeyUp(int aKey) {
  if (aKey == s_SIM_config.padSettings.fastForwardJoyKey && aKey != SIM_CONFIG_JOY_INVALID_KEY) {
    if (sFastForwardJoyDown) {
      sFastForwardJoyDown = false;
      FastForwardButton(false);
    }
    return;
  }
  if (aKey == s_SIM_config.padSettings.aJoyKey) {
    // A Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000001;
  } else if (aKey == s_SIM_config.padSettings.bJoyKey) {
    // B Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000010;
  } else if (aKey == s_SIM_config.padSettings.xJoyKey) {
    // X Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF | 0b0000010000000000;
  } else if (aKey == s_SIM_config.padSettings.yJoyKey) {
    // y Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF | 0b0000100000000000;
  } else if (aKey == s_SIM_config.padSettings.upJoyKey) {
    // Dpad Up
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000001000000;
  } else if (aKey == s_SIM_config.padSettings.downJoyKey) {
    // Dpad Down
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000010000000;
  } else if (aKey == s_SIM_config.padSettings.leftJoyKey) {
    // Dpad Left
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000100000;
  } else if (aKey == s_SIM_config.padSettings.rightJoyKey) {
    // Dpad Right
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000010000;
  } else if (aKey == s_SIM_config.padSettings.lJoyKey) {
    // L
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000001000000000;
  } else if (aKey == s_SIM_config.padSettings.rJoyKey) {
    // R
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000100000000;
  } else if (aKey == s_SIM_config.padSettings.startJoyKey) {
    // Start button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000001000;
  } else if (aKey == s_SIM_config.padSettings.selectJoyKey) {
    // Select button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000100;
  }
}

void *SIM_Render(void *arg) {

  SDL_Event Event;
  char c;
  int x;
  int y;
  u8 r;
  u8 g;
  u8 b;
  u16 keys;
  u32 i;

  {
    // Everything since the last frame was shown: the game's own work, including the 3D
    // draw calls it issues (counted separately in G3SIM_TakeSubmitNs)
    sRenderStartNs = SIM_NowNs();
    u64 lastFrameEndNs = (u64)s_SIM_lastFrameEnd.tv_sec * 1000000000ull + (u64)s_SIM_lastFrameEnd.tv_nsec;
    u64 g3SubmitNs = G3SIM_TakeSubmitNs();
    u64 sinceLastFrame = sRenderStartNs - lastFrameEndNs;
    SIM_Smooth(&sTimings.gameMs, sinceLastFrame > g3SubmitNs ? sinceLastFrame - g3SubmitNs : 0);
    SIM_Smooth(&sTimings.g3SubmitMs, g3SubmitNs);
  }

  G3SIM_FlushArray();

  // Draw the translucent 3D stuff
  G3SIM_DrawItems();

  GpuTimerEnd(); // the 3D scene's GPU time (started after the last frame)
  GpuTimersCollect();
#ifdef SDK_BUILD_ANDROID
  static bool sThreadSetUp = false;
  if (!sThreadSetUp) {
    sThreadSetUp = true;
    SIM_Android_GameThreadStarted();
  }
#endif

  do {
#ifdef SDK_TRACY_ENABLE
    TracyCZone(SimRenderZone, 1);
#endif
    // A frame the game will replace before it's seen (SIM_SkipNextFrame) isn't drawn: the
    // game still runs, with its input and vblank handlers
    const bool paused = SIM::GUI::IsGameLogicPaused();
    const bool fastForward = SIM_IsFastForwarding() && !paused;
    if (fastForward != sFastForwardSwap) {
      // Fast-forward doesn't wait for the display's refresh
      sFastForwardSwap = fastForward;
      SDL_GL_SetSwapInterval(fastForward ? 0 : s_SIM_config.vsyncInterval);
      sNextVBlankNs = 0;
    }
    bool present = !(sSkipNextFrame && !paused);
    if (fastForward && SIM_NowNs() - sLastPresentNs < 15000000ull) {
      present = false; // about 60 frames a second shown, whatever the speed
    }
    sSkipNextFrame = false;
    sVBlanksThisSecond++;

    // Unbind 3D Framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glUseProgram(g2shaderProgramID);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    int windowHeight;
    int windowWidth;
    int viewHeight;
    int viewWidth;
    int viewX = 0;
    int viewY = 0;
    float aspectRatio;

    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    // With a secondary output, the main window shows only the top screen (or the
    // bottom one when swapped) and the other screen goes to the secondary output.
    const bool dualScreen = SIM_Display_SecondaryActive();

    // Scale the viewport to the correct aspect ratio and center it
    switch (s_SIM_config.screenLayout) {
    default:
    case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
      aspectRatio = 256.0f / 384.0f;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
      aspectRatio = 512.0f / 192.0f;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
      aspectRatio = 768.0f / 384.0f;
      break;
    }
    if (dualScreen) {
      aspectRatio = 256.0f / 192.0f;
      if (sWideActive && !s_SIM_config.swapScreens) {
        aspectRatio = (float)sWide3dWidth / 192.0f;
      }

      // Widescreen: the top display's width at the DS screen's height, even, up to 2:1
      if (!s_SIM_config.swapScreens && windowHeight > 0) {
        int wide = (int)((float)SIM_NDS_SCREEN_HEIGHT * (float)windowWidth / (float)windowHeight / 2.0f + 0.5f) * 2;
        wide = wide < SIM_NDS_SCREEN_WIDTH ? SIM_NDS_SCREEN_WIDTH : (wide > SIM_NDS_SCREEN_HEIGHT * 2 ? SIM_NDS_SCREEN_HEIGHT * 2 : wide);
        if (wide != sWideWidth) {
          sWideWidth = wide;
          if (s_SIM_config.widescreen) {
            SIM_SetInternalResolutionAfterRender(0);
          }
        }
      }
    }
    if ((s_SIM_config.widescreen ? sWideWidth : SIM_NDS_SCREEN_WIDTH) != sFboWidth) {
      SIM_SetInternalResolutionAfterRender(0); // the setting changed
    }

    FitViewport(windowWidth, windowHeight, aspectRatio, &viewX, &viewY,
                &viewWidth, &viewHeight);

    glViewport(viewX, viewY, viewWidth, viewHeight);

    // "Match the screen": the 3D scene at least as sharp as the screen it is shown on
    {
      const int screenPixels = (s_SIM_config.screenLayout == SIM_CONFIG_SCREEN_LAYOUT_VERTICAL && !dualScreen)
                                   ? viewHeight / 2
                                   : viewHeight;
      int autoScale = (screenPixels + SIM_NDS_SCREEN_HEIGHT - 1) / SIM_NDS_SCREEN_HEIGHT;
      autoScale = autoScale < 1 ? 1 : (autoScale > 8 ? 8 : autoScale);
      if (autoScale != sAutoScale) {
        sAutoScale = autoScale;
        if (s_SIM_config.internalResolutionScale == 0) {
          SIM_SetInternalResolutionAfterRender(0);
        }
      }
    }

    // Set up the bounding box of the touch panel
    if (s_SIM_config.swapScreens) {
      switch (s_SIM_config.screenLayout) {
      default:
      case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + (viewHeight / 2);
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + (viewWidth / 2);
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + ((viewWidth / 3) * 2);
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      }
    } else {
      switch (s_SIM_config.screenLayout) {
      default:
      case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY + (viewHeight / 2);
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
        s_TouchPanelCoords[0] = viewX + (viewWidth / 2);
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
        s_TouchPanelCoords[0] = viewX + ((viewWidth / 3) * 2);
        s_TouchPanelCoords[1] = viewY + (viewHeight / 2);
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      }
    }

    // Check Joysticks
    if (s_SIM_SDLJoystick == NULL && SDL_NumJoysticks() >= 1) {
      s_SIM_SDLJoystick = SDL_JoystickOpen(0);
    }

    while (SDL_PollEvent(&Event)) {
      if (SIM_Display_ProcessEvent(&Event)) {
        // Input/close event for the secondary output window
        continue;
      }
      if (SIM::Settings::CaptureEvent(&Event)) {
        // A key or button chosen for a control in the settings
        continue;
      }
      SIM::GUI::ProcessEvent(&Event);

      // Give the game a chance to save before the app pauses or exits
      if (Event.type == SDL_APP_WILLENTERBACKGROUND) {
        SIM_Prj_OnLifecycleEvent(SIM_LIFECYCLE_BACKGROUND);
      } else if (Event.type == SDL_APP_TERMINATING) {
        SIM_Prj_OnLifecycleEvent(SIM_LIFECYCLE_QUIT);
      } else if (Event.type == SDL_QUIT) {
        // Ctrl+C / SIGTERM / logging out on desktop; the activity finishing on Android
        SIM_Prj_OnLifecycleEvent(SIM_LIFECYCLE_QUIT);
#ifndef SDK_BUILD_ANDROID
        exit(0);
#endif
      }

      if (Event.type == SDL_WINDOWEVENT) {
        switch (Event.window.event) {
        case SDL_WINDOWEVENT_CLOSE:
          SIM_Prj_OnLifecycleEvent(SIM_LIFECYCLE_QUIT);
          exit(0);
          break;
        }
      }
      if (Event.type == SDL_KEYDOWN) {
        int keyRead = Event.key.keysym.sym;
        if (keyRead == s_SIM_config.padSettings.aKey) {
          // A Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111110;
        } else if (keyRead == s_SIM_config.padSettings.bKey) {
          // B Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111101;
        } else if (keyRead == s_SIM_config.padSettings.xKey) {
          // X Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF & 0b1111100000000000;
        } else if (keyRead == s_SIM_config.padSettings.yKey) {
          // y Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF & 0b1111010000000000;
        } else if (keyRead == s_SIM_config.padSettings.upKey) {
          // Dpad Up
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111110111111;
        } else if (keyRead == s_SIM_config.padSettings.downKey) {
          // Dpad Down
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111101111111;
        } else if (keyRead == s_SIM_config.padSettings.leftKey) {
          // Dpad Left
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111011111;
        } else if (keyRead == s_SIM_config.padSettings.rightKey) {
          // Dpad Right
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111101111;
        } else if (keyRead == s_SIM_config.padSettings.lKey) {
          // L
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111110111111111;
        } else if (keyRead == s_SIM_config.padSettings.rKey) {
          // R
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111011111111;
        } else if (keyRead == s_SIM_config.padSettings.startKey) {
          // Start button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111110111;
        } else if (keyRead == s_SIM_config.padSettings.selectKey) {
          // Select button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111011;
        } else if (keyRead == SDLK_F1) {
          // Companion UI full screen on the secondary window (or back)
          SIM_Companion_ToggleFullScreen();
        } else if (keyRead == s_SIM_config.padSettings.guiKey) {
          // Toggle Debug GUI
          SIM::GUI::Toggle();
        } else if (keyRead == s_SIM_config.padSettings.frameCapToggleKey) {
          // Fast-forward on or off
          if (!Event.key.repeat) {
            sFastForwardOn = !sFastForwardOn;
          }
        } else if(keyRead == s_SIM_config.padSettings.frameCapKey) {
          if (!Event.key.repeat) {
            FastForwardButton(true);
          }
        }
      }
      if (Event.type == SDL_KEYUP) {
        int keyRead = Event.key.keysym.sym;
        if (keyRead == s_SIM_config.padSettings.aKey) {
          // A Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000001;
        } else if (keyRead == s_SIM_config.padSettings.bKey) {
          // B Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000010;
        } else if (keyRead == s_SIM_config.padSettings.xKey) {
          // X Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF | 0b0000010000000000;
        } else if (keyRead == s_SIM_config.padSettings.yKey) {
          // y Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF | 0b0000100000000000;
        } else if (keyRead == s_SIM_config.padSettings.upKey) {
          // Dpad Up
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000001000000;
        } else if (keyRead == s_SIM_config.padSettings.downKey) {
          // Dpad Down
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000010000000;
        } else if (keyRead == s_SIM_config.padSettings.leftKey) {
          // Dpad Left
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000100000;
        } else if (keyRead == s_SIM_config.padSettings.rightKey) {
          // Dpad Right
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000010000;
        } else if (keyRead == s_SIM_config.padSettings.lKey) {
          // L
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000001000000000;
        } else if (keyRead == s_SIM_config.padSettings.rKey) {
          // R
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000100000000;
        } else if (keyRead == s_SIM_config.padSettings.startKey) {
          // Start button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000001000;
        } else if (keyRead == s_SIM_config.padSettings.selectKey) {
          // Select button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000100;
        } else if(keyRead == s_SIM_config.padSettings.frameCapKey) {
          FastForwardButton(false);
          SDL_GL_SetSwapInterval(s_SIM_config.vsyncInterval);
        }
      }
      if (Event.type == SDL_JOYBUTTONDOWN) {
#ifdef SDK_BUILD_ANDROID
        HandleJoystickKeyDown(TranslateAndroidDpadButton(Event.jbutton.button));
#else
        HandleJoystickKeyDown(Event.jbutton.button);
#endif
      }
      if (Event.type == SDL_JOYBUTTONUP) {
#ifdef SDK_BUILD_ANDROID
        HandleJoystickKeyUp(TranslateAndroidDpadButton(Event.jbutton.button));
#else
        HandleJoystickKeyUp(Event.jbutton.button);
#endif
      }
      if (Event.type == SDL_JOYHATMOTION) {
        if (Event.jhat.value == SDL_HAT_CENTERED) {
          // send keyUp event for all hats
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_UP);
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_DOWN);
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_LEFT);
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_RIGHT);
        } else {
          if (Event.jhat.value & SDL_HAT_UP) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_UP);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_UP);
          }
          if (Event.jhat.value & SDL_HAT_DOWN) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_DOWN);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_DOWN);
          }
          if (Event.jhat.value & SDL_HAT_LEFT) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_LEFT);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_LEFT);
          }
          if (Event.jhat.value & SDL_HAT_RIGHT) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_RIGHT);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_RIGHT);
          }
        }
      }
      if (Event.type == SDL_JOYAXISMOTION) {
        if (Event.jaxis.value > s_SIM_config.padSettings.joyAxisDeadzone ||
            Event.jaxis.value <
                (s_SIM_config.padSettings.joyAxisDeadzone * -1)) {
          if (Event.jaxis.value > 0) {
            HandleJoystickKeyDown(Event.jaxis.axis |
                                  SIM_CONFIG_JOY_AXIS_PLUS_MASK);
            HandleJoystickKeyUp(Event.jaxis.axis |
                                SIM_CONFIG_JOY_AXIS_MINUS_MASK);
          } else {
            HandleJoystickKeyDown(Event.jaxis.axis |
                                  SIM_CONFIG_JOY_AXIS_MINUS_MASK);
            HandleJoystickKeyUp(Event.jaxis.axis |
                                SIM_CONFIG_JOY_AXIS_PLUS_MASK);
          }
        } else {
          HandleJoystickKeyUp(Event.jaxis.axis | SIM_CONFIG_JOY_AXIS_PLUS_MASK);
          HandleJoystickKeyUp(Event.jaxis.axis |
                              SIM_CONFIG_JOY_AXIS_MINUS_MASK);
        }
      }
      if (Event.type == SDL_MOUSEBUTTONDOWN) {
        isMouseDown = 1;
        s_tpData.validity = 0;
        s_tpData.touch = 1;
      }
      if (Event.type == SDL_MOUSEBUTTONUP) {
        isMouseDown = 0;
        s_tpData.validity = 0;
        s_tpData.touch = 0;
      }
    }
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT;

    if (dualScreen) {
      // The touch screen lives on the secondary output. Touches/clicks on the main
      // window are ignored (they still reach the ImGui settings overlay).
      const bool bottomOnSecondary = !s_SIM_config.swapScreens;
      SIM_TouchEvent touch;
      while (SIM_Display_PollSecondaryTouch(&touch)) {
        if (touch.type == SIM_TOUCH_DOWN) {
          const bool onDsScreen =
              bottomOnSecondary && touch.x >= s_SecondaryTouchRect[0] &&
              touch.x < s_SecondaryTouchRect[2] && touch.y >= s_SecondaryTouchRect[1] &&
              touch.y < s_SecondaryTouchRect[3];
          s_SecondaryTouchOwner = onDsScreen ? TOUCH_OWNER_DS : TOUCH_OWNER_COMPANION;
        }
        if (s_SecondaryTouchOwner == TOUCH_OWNER_DS) {
          s_SecondaryDsTouchDown = touch.type != SIM_TOUCH_UP;
          s_SecondaryDsTouchX = touch.x;
          s_SecondaryDsTouchY = touch.y;
        } else if (s_SecondaryTouchOwner == TOUCH_OWNER_COMPANION) {
          SIM_Companion_QueueTouch(touch.type, touch.x, touch.y);
        }
        if (touch.type == SIM_TOUCH_UP) {
          s_SecondaryTouchOwner = TOUCH_OWNER_NONE;
        }
      }

      if (bottomOnSecondary && s_SecondaryDsTouchDown &&
          s_SecondaryTouchRect[2] > s_SecondaryTouchRect[0]) {
        // Keep a drag that leaves the screen on its edge
        int touchX = s_SecondaryDsTouchX;
        int touchY = s_SecondaryDsTouchY;
        touchX = touchX < s_SecondaryTouchRect[0] ? s_SecondaryTouchRect[0] : touchX;
        touchX = touchX > s_SecondaryTouchRect[2] - 1 ? s_SecondaryTouchRect[2] - 1 : touchX;
        touchY = touchY < s_SecondaryTouchRect[1] ? s_SecondaryTouchRect[1] : touchY;
        touchY = touchY > s_SecondaryTouchRect[3] - 1 ? s_SecondaryTouchRect[3] - 1 : touchY;
        s_tpData.x = (u16)map(touchX, s_SecondaryTouchRect[0],
                              s_SecondaryTouchRect[2], s_SecondaryTouchSrc[0], s_SecondaryTouchSrc[2]);
        s_tpData.y = (u16)map(touchY, s_SecondaryTouchRect[1],
                              s_SecondaryTouchRect[3], s_SecondaryTouchSrc[1], s_SecondaryTouchSrc[3]);
        s_tpData.validity = 0;
        s_tpData.touch = 1;
      } else if (!bottomOnSecondary && isMouseDown) {
        // Swapped: the bottom screen is on the main window, which is all touch panel
        SDL_GetMouseState(&x, &y);
        if (x >= viewX && x < viewX + viewWidth && y >= viewY &&
            y < viewY + viewHeight) {
          s_tpData.x = (u16)map(x, viewX, viewX + viewWidth, 0,
                                SIM_NDS_SCREEN_WIDTH);
          s_tpData.y = (u16)map(y, viewY, viewY + viewHeight, 0,
                                SIM_NDS_SCREEN_HEIGHT);
          s_tpData.validity = 0;
          s_tpData.touch = 1;
        } else {
          s_tpData.touch = 0;
        }
      } else {
        s_tpData.validity = 0;
        s_tpData.touch = 0;
      }
    } else {
      SDL_GetMouseState(&x, &y);

      // Convert touch coordinate to DS screen coordinates
      if (x >= s_TouchPanelCoords[0] && x <= s_TouchPanelCoords[2] &&
          y >= s_TouchPanelCoords[1] && y <= s_TouchPanelCoords[3]) {
        s_tpData.x = (u16)map(x, s_TouchPanelCoords[0], s_TouchPanelCoords[2], 0,
                              SIM_NDS_SCREEN_WIDTH);
        s_tpData.y = (u16)map(y, s_TouchPanelCoords[1], s_TouchPanelCoords[3], 0,
                              SIM_NDS_SCREEN_HEIGHT);
      } else {
        s_tpData.validity = 0;
        s_tpData.touch = 0;
      }
    }

    sFrameG2Ns = 0;
    sFrameUploadNs = 0;
    sFrameUploads = 0;
    u64 secondaryNs = 0;
    u64 companionNs = 0;
    u64 secondarySwapNs = 0;
    u64 guiNs = 0;
    if (present) {
    const u64 guiStart = SIM_NowNs();
    SIM::GUI::NewFrame();
    SIM::GUI::Main();
    guiNs += SIM_NowNs() - guiStart;

    if (dualScreen) {
      // Both outputs must be drawn before the VBlank handlers below change VRAM,
      // registers and OAM for the next frame.
      //
      // POWCNT bit 15 picks which 2D engine drives the top LCD.
      const BOOL topIsSub = (s_reg_GX_POWCNT >> 15) ? FALSE : TRUE;
      const bool swapped = s_SIM_config.swapScreens;

      // Secondary output first: everything drawn into the main window must happen
      // after switching back to it, since a drawable switch may not keep its back
      // buffer intact on every driver.
      int secondaryWidth;
      int secondaryHeight;
      const u64 secondaryStart = SIM_NowNs();
      const u64 secondaryG2Before = sFrameG2Ns + sFrameUploadNs;
      if (SIM_Display_BeginSecondary(&secondaryWidth, &secondaryHeight)) {
        GpuTimerBegin(GPU_SECONDARY);
        // DS screen, plus the game's companion UI when it has one (sim_companion.h)
        SIM_CompanionLayout layout;
        SIM_Companion_ComputeLayout(secondaryWidth, secondaryHeight, &layout);

        glViewport(0, 0, secondaryWidth, secondaryHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (layout.dsVisible) {
          // GL's origin is bottom-left, layout and touch coordinates are top-left
          glViewport(layout.dsX, secondaryHeight - (layout.dsY + layout.dsHeight),
                     layout.dsWidth, layout.dsHeight);
          s_SecondaryTouchRect[0] = layout.dsX;
          s_SecondaryTouchRect[1] = layout.dsY;
          s_SecondaryTouchRect[2] = layout.dsX + layout.dsWidth;
          s_SecondaryTouchRect[3] = layout.dsY + layout.dsHeight;
          s_SecondaryTouchSrc[0] = 0;
          s_SecondaryTouchSrc[1] = 0;
          s_SecondaryTouchSrc[2] = SIM_NDS_SCREEN_WIDTH;
          s_SecondaryTouchSrc[3] = SIM_NDS_SCREEN_HEIGHT;

          s_SingleScreenQuad = swapped ? SINGLE_SCREEN_TOP : SINGLE_SCREEN_BOTTOM;
          DrawEngine(swapped ? topIsSub : !topIsSub);
        } else {
          s_SecondaryTouchRect[0] = s_SecondaryTouchRect[1] = 0;
          s_SecondaryTouchRect[2] = s_SecondaryTouchRect[3] = 0;
          s_SecondaryDsTouchDown = false;
        }

        const u64 companionStart = SIM_NowNs();
        SIM_Companion_Render(&layout);
        companionNs = SIM_NowNs() - companionStart;

        // Part of the DS touch screen shown inside the companion (a Pokétch tab, say): drawn
        // over it, scaled so the region fills the rectangle, and it takes the touches
        SIM_CompanionDsView view;
        if (!swapped && SIM_Companion_DsView(&view)) {
          const float sx = (float)view.width / (float)view.dsWidth;
          const float sy = (float)view.height / (float)view.dsHeight;
          const int vx = (int)((float)view.x - (float)view.dsX * sx);
          const int vyTop = (int)((float)view.y - (float)view.dsY * sy);
          const int vw = (int)((float)SIM_NDS_SCREEN_WIDTH * sx + 0.5f);
          const int vh = (int)((float)SIM_NDS_SCREEN_HEIGHT * sy + 0.5f);
          glUseProgram(g2shaderProgramID);
          glBindVertexArray(g2vertexArray);
          glEnable(GL_SCISSOR_TEST);
          glScissor(view.x, secondaryHeight - (view.y + view.height), view.width, view.height);
          glViewport(vx, secondaryHeight - (vyTop + vh), vw, vh);
          s_SingleScreenQuad = SINGLE_SCREEN_BOTTOM;
          DrawEngine(!topIsSub);
          glDisable(GL_SCISSOR_TEST);

          s_SecondaryTouchRect[0] = view.x;
          s_SecondaryTouchRect[1] = view.y;
          s_SecondaryTouchRect[2] = view.x + view.width;
          s_SecondaryTouchRect[3] = view.y + view.height;
          s_SecondaryTouchSrc[0] = view.dsX;
          s_SecondaryTouchSrc[1] = view.dsY;
          s_SecondaryTouchSrc[2] = view.dsX + view.dsWidth;
          s_SecondaryTouchSrc[3] = view.dsY + view.dsHeight;
        }
        GpuTimerEnd();
        const u64 endStart = SIM_NowNs();
        SIM_Display_EndSecondary();
        secondarySwapNs = SIM_NowNs() - endStart;
      }
      secondaryNs = (SIM_NowNs() - secondaryStart) - ((sFrameG2Ns + sFrameUploadNs) - secondaryG2Before);

      // Main window: the top screen (bottom screen when swapped)
      GpuTimerBegin(GPU_MAIN);
      glViewport(0, 0, windowWidth, windowHeight);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glViewport(viewX, viewY, viewWidth, viewHeight);
      s_SingleScreenQuad = swapped ? SINGLE_SCREEN_BOTTOM : SINGLE_SCREEN_TOP;
      DrawEngine(swapped ? !topIsSub : topIsSub);

      s_SingleScreenQuad = SINGLE_SCREEN_NONE;
      s_WasDualScreen = true;
    } else {
      if (s_WasDualScreen) {
        // Secondary output was just turned off: blank it instead of freezing a frame
        int secondaryWidth;
        int secondaryHeight;
        if (SIM_Display_BeginSecondary(&secondaryWidth, &secondaryHeight)) {
          glViewport(0, 0, secondaryWidth, secondaryHeight);
          glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
          glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
          SIM_Display_EndSecondary();
          glViewport(viewX, viewY, viewWidth, viewHeight);
        }
        s_WasDualScreen = false;
      }

      GpuTimerBegin(GPU_MAIN);
      glViewport(0, 0, windowWidth, windowHeight);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glViewport(viewX, viewY, viewWidth, viewHeight);

      // Draw MAIN engine
      DrawEngine(FALSE);

      // SUB engine
      DrawEngine(TRUE);
    }
    } // present

    // Run HBlank Interrupt
    OS_IRQTable[OS_IE_H_BLANK - 1]();

    // Run VBlank Interrupt
    OS_IRQTable[OS_IE_V_BLANK - 1]();
    s_HW_INTR_CHECK_BUF |= 1;
    *((u32 *)HW_VBLANK_COUNT_BUF) = *((u32 *)HW_VBLANK_COUNT_BUF) + 1;

    // The game's frame is done: the wireless callbacks it waits for run from here until
    // the next frame starts (see SIM_GameLock_Engage)
    SIM_GameLock_Release();
    if ((*((u32 *)HW_VBLANK_COUNT_BUF) % 10) == 0) {
      SIM_Net_SendBeaconIndication();
    }

    if (present) {
      const u64 guiStart = SIM_NowNs();
      SIM::GUI::Render();
      guiNs += SIM_NowNs() - guiStart;
      GpuTimerEnd();
      SIM_Smooth(&sTimings.g2Ms, sFrameG2Ns);
      SIM_Smooth(&sTimings.uploadMs, sFrameUploadNs);
      SIM_Smooth(&sTimings.secondaryMs, secondaryNs);
      SIM_Smooth(&sTimings.companionMs, companionNs);
      SIM_Smooth(&sTimings.secondarySwapMs, secondarySwapNs);
      SIM_Smooth(&sTimings.guiMs, guiNs);
      SIM_SmoothValue(&sTimings.uploads2d, (float)sFrameUploads);
    }

    // Calculate frametime
    struct timespec curTime;
    clock_gettime(CLOCK_MONOTONIC, &curTime);
    u64 frameNs;
    frameNs = ((curTime.tv_sec - s_SIM_lastFrameEnd.tv_sec) * 1000000000) +
              (curTime.tv_nsec - s_SIM_lastFrameEnd.tv_nsec);
    sRenderFrameTime = frameNs;

#ifdef SDK_TRACY_ENABLE
    TracyCZoneEnd(SimRenderZone);
#endif
    sBeforeWaitNs = SIM_NowNs();
    // (skipping samples that span a pause, e.g. while the settings menu holds the game)
    if (present && sBeforeWaitNs - sRenderStartNs < 250000000ull) {
      SIM_Smooth(&sTimings.drawMs, sBeforeWaitNs - sRenderStartNs);
    }
#ifdef SDK_BUILD_ANDROID
    SIM_Android_ReportFrameWork((long long)frameNs);
#endif

    // Limit framerate to 60 fps
    if (fastForward) {
      if (s_SIM_config.fastForwardSpeed > 0 && s_SIM_config.capFrameRate) {
        PaceVBlank(16666667ull / s_SIM_config.fastForwardSpeed);
      }
    } else if (s_SIM_config.capFrameRate) {

      // 60 Fps
      if (frameNs < 16600000) {
        u64 frameTimeRemaining = 16600000 - frameNs;
        u64 frameTimeRemainingMs = frameTimeRemaining / 1000000;
        if (frameTimeRemainingMs > 1) {
          SDL_Delay(frameTimeRemainingMs - 1);
        }
      }
    }

    u64 beforeSwapNs = SIM_NowNs();
    if (present) {
      SIM_Smooth(&sTimings.capWaitMs, beforeSwapNs - sBeforeWaitNs);

      SDL_GL_SwapWindow(window);
      sLastPresentNs = SIM_NowNs();

      SIM_Smooth(&sTimings.swapMs, sLastPresentNs - beforeSwapNs);
      NextLayerSet();
      sShownThisSecond++;
    }
    UpdateRates();

    {
      // LIBNTR_PERF_LOG=1: print the frame time breakdown every two seconds
      static int sPerfLog = -1;
      static u32 sPerfFrames = 0;
      if (sPerfLog < 0) {
        const char *env = getenv("LIBNTR_PERF_LOG");
        sPerfLog = (env != NULL && env[0] == '1') ? 1 : 0;
      }
      if (sPerfLog && ++sPerfFrames % 120 == 0) {
        printf("perf: game %.2f  3d %.2f  draw %.2f (2d %.2f upload %.2f second %.2f gui %.2f)  wait %.2f  swap %.2f "
               "ms [companion %.2f, 2nd swap %.2f] | gpu 3d %.2f main %.2f second %.2f | shown %.0f/s vblanks %.0f/s draws %.0f uploads %.1f\n",
               sTimings.gameMs, sTimings.g3SubmitMs, sTimings.drawMs, sTimings.g2Ms, sTimings.uploadMs,
               sTimings.secondaryMs, sTimings.guiMs, sTimings.capWaitMs, sTimings.swapMs, sTimings.companionMs,
               sTimings.secondarySwapMs, sTimings.gpu3dMs,
               sTimings.gpuMainMs, sTimings.gpuSecondaryMs, sTimings.shownPerSecond, sTimings.vblanksPerSecond,
               sTimings.draws3d, sTimings.uploads2d);
        fflush(stdout);
      }
    }

    if(sChangeInternalResAfterRender) {
      sChangeInternalResAfterRender = false;
      SIM_SetInternalResolution(sPendingInternalRes);
    }

    //Calculate full frametime (after swap)
    clock_gettime(CLOCK_MONOTONIC, &curTime);
    frameNs = ((curTime.tv_sec - s_SIM_lastFrameEnd.tv_sec) * 1000000000) +
              (curTime.tv_nsec - s_SIM_lastFrameEnd.tv_nsec);
    sFullFrameTime = frameNs;


    clock_gettime(CLOCK_MONOTONIC, &s_SIM_lastFrameEnd);
#ifdef SDK_TRACY_ENABLE
    TracyCFrameMark;
#endif

    s_numG3CommandsThisFrame = 0;
    s_curPolygonAttr.alpha = 1.0f;

    SIM_GameLock_Reacquire();
  } while (SIM::GUI::IsGameLogicPaused());
  // Bind 3D Framebuffer & Renderbuffer
  glBindFramebuffer(GL_FRAMEBUFFER, g3FrameBuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, g3RenderBufferId);

  // Widescreen for the 3D drawn until the next frame: the game asks, the top screen is on its
  // own display and shows the 3D engine
  {
    const bool wide = s_SIM_config.widescreen && sWideRequested && SIM_Display_SecondaryActive() &&
                      !s_SIM_config.swapScreens && (s_reg_GX_POWCNT >> 15) && sFboWidth > SIM_NDS_SCREEN_WIDTH;
    if (wide != sWideActive) {
      printf("Widescreen: %s\n", wide ? "on" : "off");
    }
    sWideActive = wide;
    sWide3dWidth = wide ? sFboWidth : SIM_NDS_SCREEN_WIDTH;
    sWideScale3D = wide && sWideScale3DRequested;
    sWideBgLayers = wide ? sWideBgRequested : 0;
  }

  // Setup 3D Viewport
  if (s_reg_GX_POWCNT >> 15) {
    glViewport(0, SIM_NDS_SCREEN_HEIGHT * sScale, sWide3dWidth * sScale, SIM_NDS_SCREEN_HEIGHT * sScale);
  } else {
    glViewport(0, 0, sWide3dWidth * sScale, SIM_NDS_SCREEN_HEIGHT * sScale);
  }

  // Activate 3D shader
  glUseProgram(g3shaderProgramID);
  // A wide scene whose projection the game left at 4:3: narrowed here, so shapes keep their
  // proportions and more is seen at the sides
  glProgramUniform1f(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "xScale"),
                     sWideScale3D ? (float)SIM_NDS_SCREEN_WIDTH / (float)sWide3dWidth : 1.0f);
  GpuTimerBegin(GPU_3D); // until the next frame's drawing starts

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);

  glCullFace(GL_FRONT);
  glFrontFace(GL_CW);

  return nullptr;
}

// Get the rendering time in nanoseconds
u64 SIM_GetRenderFrameTime() {
  return sRenderFrameTime;
}

// Get the full frame time (including vsync wait) in nanoseconds
u64 SIM_GetFullFrameTime() {
  return sFullFrameTime;
}

void SIM_PreRenderVBlank() { SDL_SemPost(vcountVblankSemaphore); }

void SIM_PostRenderVBlank() {
  if (G3SIM_SwapBuffersCalledThisFrame()) {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    G3SIM_ResetSwapBuffersCall();
  }
  // Reset Vcount
  s_reg_GX_VCOUNT = 0;
  SDL_SemPost(vcountResetSemaphore);
}

const char * SIM_GetLibntrGitHash() {
  return SIM_LIBNTR_GIT_HASH;
}

const char * SIM_GetProjectGitHash() {
  return SIM_PROJECT_GIT_HASH;
}

// The build's game and maker codes in the loaded ROM header. A header whose game code is one
// the build runs keeps it (HeartGold or SoulSilver, from one build); any other gets the
// build's first code.
static void SetBuildCodes(CARDRomHeader *cardHeader) {
#ifdef LIBNTR_CARD_GAME_CODE
  if (!SIM_Carts_RunsGameCode((const char *)&cardHeader->game_code)) {
    strncpy((char *)&cardHeader->game_code, LIBNTR_CARD_GAME_CODE, sizeof(u32));
  }
#endif
#ifdef LIBNTR_CARD_MAKER_CODE
  strncpy((char *)&cardHeader->maker_code, LIBNTR_CARD_MAKER_CODE, sizeof(u16));
#endif
  (void)cardHeader;
}

#ifdef _WIN32
int main(int argc, char *argv[]);

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, PSTR cmdline,
                     int cmdshow) {
  return main(__argc, __argv);
}
#endif

int main(int argc, char *argv[]) {
#ifdef SDK_BUILD_NX
  // Start up libnx sockets
  socketInitializeDefault();
#endif

#ifdef SDK_BUILD_ANDROID
  // Logging to logcat, SDL hints, and chdir into the app's files directory
  SIM_Android_Init();
#endif

#ifdef TRACY_ENABLE
  // Start up Tracy profiler
  ___tracy_startup_profiler();

  while (!TracyCIsStarted) {
    SDL_Delay(1);
  }
#endif

  // TODO: This broke the linux AppImage build
  //chdir(SDL_GetBasePath());

  // The data directory: settings, and the games' directories (sim_carts.h)
  SIM_Carts_SetArgs(argc, argv);
  SIM_Carts_Init();

  // Start up debug system
  SIM_Dbg_Init();

  // Load default config
  SIM_Config_LoadDefaults(&s_SIM_config);

  // Load configuration file
  if (!SIM_Config_LoadConfigFile(&s_SIM_config)) {
    // Config file does not exist. Save config file with defaults
    SIM_Config_SaveConfigFile(&s_SIM_config);
  }

  // Initialize Nitro registers
  s_reg_PAD_KEYINPUT = 0x3ff;
  *(vu16 *)HW_BUTTON_XY_BUF = 0b0111110000000000;

  memset(s_HW_OAM, 0, sizeof(s_HW_OAM));
  memset(s_HW_DB_OAM, 0, sizeof(s_HW_DB_OAM));

  s_reg_GX_VRAMCNT_A = 0;
  s_reg_GX_VRAMCNT_B = 0;
  s_reg_GX_VRAMCNT_C = 0;
  s_reg_GX_VRAMCNT_D = 0;
  s_reg_GX_VRAMCNT_E = 0;
  s_reg_GX_VRAMCNT_F = 0;
  s_reg_GX_VRAMCNT_G = 0;
  s_reg_GX_VRAMCNT_H = 0;
  s_reg_GX_VRAMCNT_I = 0;

  WIN_Init_sTexStartAddrTable();
  WIN_Init_sTexPlttStartAddrTable();

  // Read the first 0x160 bytes of the rom and put it in the HW_ROM_HEADER_BUF
  FILE *romFile;
  romFile = fopen("header.bin", "r");

  if (romFile != NULL) {
    // Read the fields of the struct
    fread((void *)HW_ROM_HEADER_BUF, 1, 0x160, romFile);
    fclose(romFile);
  }

  CARDRomHeader *cardHeader = (CARDRomHeader *)(HW_ROM_HEADER_BUF);
  SetBuildCodes(cardHeader);

  memset((void *)HW_MAIN_MEM_SHARED, 0, 0x160);

  SIM_pxiInit();

  pthread_t thread;
  SDL_Thread *pxiThread;
  SDL_Thread *pxi7to9Thread;
  int num;
  num = 123;

  // Initialize mosaic table
  u32 i;
  u32 j;
  for (i = 0; i < 16; i++) {
    for (j = 0; j < 256; j++) {
      int offset = j % (i + 1);
      s_mosaicTable[i][j] = offset;
    }
  }

  SND_ExChannelInit();
  SND_SeqInit();

  // Zero out some shared memory
  memset(s_HW_BG_PLTT, 0, sizeof(s_HW_BG_PLTT));
  memset(s_HW_OBJ_PLTT, 0, sizeof(s_HW_OBJ_PLTT));
  memset(s_HW_DB_BG_PLTT, 0, sizeof(s_HW_DB_BG_PLTT));
  memset(s_HW_DB_OBJ_PLTT, 0, sizeof(s_HW_DB_OBJ_PLTT));
  memset(s_HW_BG_VRAM, 0, sizeof(s_HW_BG_VRAM));
  memset(s_HW_DB_BG_VRAM, 0, sizeof(s_HW_DB_BG_VRAM));
  memset(s_HW_OBJ_VRAM, 0, sizeof(s_HW_OBJ_VRAM));
  memset(s_HW_DB_OBJ_VRAM, 0, sizeof(s_HW_DB_OBJ_VRAM));

  // Zero out some registers
  s_reg_G2_BG0CNT = 0;
  s_reg_G2_BG1CNT = 0;
  s_reg_G2_BG2CNT = 0;
  s_reg_G2_BG3CNT = 0;

  s_reg_G2S_DB_BG0CNT = 0;
  s_reg_G2S_DB_BG1CNT = 0;
  s_reg_G2S_DB_BG2CNT = 0;
  s_reg_G2S_DB_BG3CNT = 0;

  // Set up OS NVRAM Config
  SIM_Config_SetOSNVRAMConfig(&s_SIM_config);

  pxiThread = SDL_CreateThread(SIM_procPxiThread, "PXI", NULL);
  pxi7to9Thread = SDL_CreateThread(SIM_procPxi7to9Thread, "PXI7to9", NULL);

  WM_sp_SIM_init();

#ifdef SDK_BUILD_WIN64
  SetConsoleOutputCP(65001);
#endif

  SIM_RenderInit(NULL);
  G3SIM_DrawInit();

  // Which game to run (a picker when there are several), then enter its directory and
  // load its ROM header
  SIM_Carts_Choose();
  romFile = fopen("header.bin", "r");
  if (romFile != NULL) {
    fread((void *)HW_ROM_HEADER_BUF, 1, 0x160, romFile);
    fclose(romFile);
    SetBuildCodes(cardHeader);
  }

// Init Sockets (only needed on windows)
#ifdef SDK_BUILD_WIN64
  WSADATA wsa;

  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    printf("Failed to initialize winsock. Error Code : %d", WSAGetLastError());
    return 1;
  }
#endif

  SIM_Net_Init();
  SIM_Net_StartThread();

  if (argc >= 2) {
    if (strcmp(argv[1], "--net-host") == 0) {
      SIM_Net_StartHost();
      SDL_SetWindowTitle(window, "Host");
    }
    if (strcmp(argv[1], "--net-client") == 0) {
      SIM_Net_StartClient(s_SIM_config.netSettings.serverIp);
      SDL_SetWindowTitle(window, "Client");
    }
  }

  SIM_GameLock_Engage();
  NitroMain();
  return 0;
}

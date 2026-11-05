// Secondary screen output: see include/simulator/sim_display.h
#include <simulator/sim.h>
#include <simulator/sim_display.h>
#include <simulator/sim_opengl.h>

#include <simulator/sim_companion.h>

#include <atomic>
#include <deque>
#include <mutex>

#ifdef SDK_BUILD_ANDROID
#include <EGL/egl.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#endif

namespace {

std::atomic<bool> sTouchDown{false};
std::atomic<int> sTouchX{0};
std::atomic<int> sTouchY{0};

// Ordered touch events (Java UI thread or SDL events -> render thread)
std::mutex sTouchQueueLock;
std::deque<SIM_TouchEvent> sTouchQueue;

void QueueTouch(int type, int x, int y) {
  std::lock_guard<std::mutex> guard(sTouchQueueLock);
  if (sTouchQueue.size() >= 512) {
    sTouchQueue.pop_front();
  }
  sTouchQueue.push_back({type, x, y});
}

#ifdef SDK_BUILD_ANDROID

#define DISPLAY_LOG(...)                                                       \
  __android_log_print(ANDROID_LOG_INFO, "libntr-display", __VA_ARGS__)

// The Java Presentation's surface arrives on the UI thread while the renderer runs on
// the SDL main thread. sLock guards the window/surface pair: the render thread holds it
// for the whole time the secondary surface is current, so the UI thread can never pull
// the window out from under an in-flight frame.
std::mutex sLock;
ANativeWindow *sWindow = nullptr;
EGLSurface sSurface = EGL_NO_SURFACE;

// Captured from SDL's context on the render thread
EGLDisplay sEglDisplay = EGL_NO_DISPLAY;
EGLContext sEglContext = EGL_NO_CONTEXT;
EGLConfig sEglConfig = nullptr;
EGLSurface sMainDraw = EGL_NO_SURFACE;
EGLSurface sMainRead = EGL_NO_SURFACE;
bool sHoldingLock = false;

// Must be called with sLock held
void ReleaseWindowLocked() {
  if (sSurface != EGL_NO_SURFACE) {
    eglDestroySurface(sEglDisplay, sSurface);
    sSurface = EGL_NO_SURFACE;
  }
  if (sWindow != nullptr) {
    ANativeWindow_release(sWindow);
    sWindow = nullptr;
  }
  sTouchDown = false;
}

#else // Desktop: a second SDL window that shares the main GL context

SDL_Window *sMainWindow = nullptr;
SDL_GLContext sContext = nullptr;
SDL_Window *sSecondWindow = nullptr;
bool sUserClosedWindow = false;
int sMainSwapInterval = 1;

// Window placement changes are written to the config file once the window has stopped
// moving for a moment (a drag sends a stream of move events).
bool sPlacementDirty = false;
Uint32 sPlacementChangedAt = 0;

void MarkPlacementDirty() {
  sPlacementDirty = true;
  sPlacementChangedAt = SDL_GetTicks();
}

bool IsFullScreen(SDL_Window *window) {
  return window != nullptr &&
         (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

// TRUE if a window at (x, y, w, h) would show enough of itself (its top edge, where the
// title bar is) on one of the connected displays to be grabbed and moved. A position saved
// on a monitor that is gone falls back to the default.
bool PlacementVisible(int x, int y, int w, int h) {
  if (x == SIM_CONFIG_WINDOW_POS_UNSET || y == SIM_CONFIG_WINDOW_POS_UNSET) {
    return false;
  }
  const int displays = SDL_GetNumVideoDisplays();
  for (int i = 0; i < displays; i++) {
    SDL_Rect bounds;
    if (SDL_GetDisplayBounds(i, &bounds) != 0) {
      continue;
    }
    const int left = x > bounds.x ? x : bounds.x;
    const int right = (x + w) < (bounds.x + bounds.w) ? (x + w) : (bounds.x + bounds.w);
    const bool topInside = y >= bounds.y - 8 && y < bounds.y + bounds.h - 32;
    if (topInside && right - left >= 96) {
      return true;
    }
  }
  return false;
}

void SetFullScreen(SDL_Window *window, bool on) {
  if (window == nullptr || IsFullScreen(window) == on) {
    return;
  }
  SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

// The top window just went full screen: a bottom window on the same monitor would end up
// behind it, so with a second monitor it moves to the middle of that one
void MoveBottomOffTopsDisplay() {
  if (sSecondWindow == nullptr || sMainWindow == nullptr || IsFullScreen(sSecondWindow)) {
    return;
  }
  const int mainDisplay = SDL_GetWindowDisplayIndex(sMainWindow);
  const int displays = SDL_GetNumVideoDisplays();
  if (mainDisplay < 0 || displays < 2 || SDL_GetWindowDisplayIndex(sSecondWindow) != mainDisplay) {
    return;
  }
  for (int i = 0; i < displays; i++) {
    SDL_Rect bounds;
    if (i == mainDisplay || SDL_GetDisplayUsableBounds(i, &bounds) != 0) {
      continue;
    }
    int w, h;
    SDL_GetWindowSize(sSecondWindow, &w, &h);
    SDL_SetWindowPosition(sSecondWindow, bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2);
    return;
  }
}

void DefaultSecondSize(int *width, int *height) {
  // With a companion UI, mimic the AYN Thor's lower panel (1080x1240) at half size
  const bool companion = SIM_Prj_CompanionEnabled() != FALSE;
  *width = companion ? 540 : SIM_NDS_SCREEN_WIDTH * 2;
  *height = companion ? 620 : SIM_NDS_SCREEN_HEIGHT * 2;
}

// With the bottom screen in its own window, the top window only needs one screen's shape
// (4:3); without it, the stacked layout needs two screens' height. Only a window in the
// other layout's shape is changed, so a size the user picked stays.
void FitMainWindow(bool topOnly) {
  const SIM_config_type *config = SIM_GetConfigPtr();
  if (sMainWindow == nullptr || IsFullScreen(sMainWindow) ||
      (SDL_GetWindowFlags(sMainWindow) & SDL_WINDOW_MAXIMIZED) != 0 ||
      config->screenLayout != SIM_CONFIG_SCREEN_LAYOUT_VERTICAL) {
    return;
  }
  int w, h;
  SDL_GetWindowSize(sMainWindow, &w, &h);
  if (topOnly && h * 4 > w * 5) {
    SDL_SetWindowSize(sMainWindow, w, w * 3 / 4); // was two screens tall
  } else if (!topOnly && h * 5 < w * 4) {
    SDL_SetWindowSize(sMainWindow, w, w * 3 / 2); // was one screen's shape
  }
}

void UpdateSecondWindow() {
  SIM_config_type *config = SIM_GetConfigPtr();
  const bool wanted = config->secondaryScreen && !sUserClosedWindow;

  if (wanted && sSecondWindow == nullptr && sMainWindow != nullptr) {
    int width, height;
    DefaultSecondSize(&width, &height);
    if (config->bottomWindowWidth >= 128 && config->bottomWindowHeight >= 96) {
      width = (int)config->bottomWindowWidth;
      height = (int)config->bottomWindowHeight;
    }
    int x, y;
    const bool saved = PlacementVisible(config->bottomWindowX, config->bottomWindowY, width, height);
    if (saved) {
      x = config->bottomWindowX;
      y = config->bottomWindowY;
    } else {
      // Beside the main window (on the same display when it is full screen)
      int mainX, mainY, mainW, mainH;
      SDL_GetWindowPosition(sMainWindow, &mainX, &mainY);
      SDL_GetWindowSize(sMainWindow, &mainW, &mainH);
      x = IsFullScreen(sMainWindow) ? mainX + 32 : mainX + mainW + 16;
      y = IsFullScreen(sMainWindow) ? mainY + 32 : mainY;
      if (!PlacementVisible(x, y, width, height)) {
        x = SDL_WINDOWPOS_CENTERED;
        y = SDL_WINDOWPOS_CENTERED;
      }
    }
    sSecondWindow = SDL_CreateWindow(
        "Bottom Screen", x, y, width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (sSecondWindow == nullptr) {
      printf("Could not create the bottom screen window: %s\n", SDL_GetError());
      sUserClosedWindow = true; // don't retry every frame
    } else {
      if (config->bottomFullScreen) {
        SetFullScreen(sSecondWindow, true);
      } else if (!saved && IsFullScreen(sMainWindow)) {
        MoveBottomOffTopsDisplay();
      }
      FitMainWindow(true);
      // Back to the top window, so the keys keep going to the game window
      SDL_RaiseWindow(sMainWindow);
    }
  } else if (!config->secondaryScreen && sSecondWindow != nullptr) {
    SDL_DestroyWindow(sSecondWindow);
    sSecondWindow = nullptr;
    FitMainWindow(false);
  }

  if (!config->secondaryScreen) {
    // Turning the option back on reopens a window the user closed
    sUserClosedWindow = false;
  }
}

// Remembers where a window is while it isn't full screen (a full-screen window reports its
// display's corner, which isn't where it should come back to).
void NotePlacement(SDL_Window *window) {
  if (window == nullptr || IsFullScreen(window) ||
      (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) != 0) {
    return;
  }
  SIM_config_type *config = SIM_GetConfigPtr();
  int x, y, w, h;
  SDL_GetWindowPosition(window, &x, &y);
  SDL_GetWindowSize(window, &w, &h);
#ifndef _WIN32
  // SDL reports where the window's contents are, but on X11 a new window is placed by its
  // frame: store the frame's corner, or the window would creep down by its title bar on
  // every start (Windows places new windows by their contents, so nothing to do there)
  int top = 0;
  int left = 0;
  if (SDL_GetWindowBordersSize(window, &top, &left, nullptr, nullptr) == 0) {
    x -= left;
    y -= top;
  }
#endif
  if (window == sSecondWindow) {
    config->bottomWindowX = x;
    config->bottomWindowY = y;
    config->bottomWindowWidth = (u32)w;
    config->bottomWindowHeight = (u32)h;
  } else if (window == sMainWindow) {
    config->windowX = x;
    config->windowY = y;
    config->windowWidth = (u32)w;
    config->windowHeight = (u32)h;
  } else {
    return;
  }
  MarkPlacementDirty();
}

// The bottom window stays above a full-screen top window that covers the same display, or
// it would disappear behind it.
void UpdateStacking() {
  static bool sOnTop = false;
  bool onTop = false;
  if (sSecondWindow != nullptr && sMainWindow != nullptr && IsFullScreen(sMainWindow) &&
      !IsFullScreen(sSecondWindow)) {
    onTop = SDL_GetWindowDisplayIndex(sMainWindow) == SDL_GetWindowDisplayIndex(sSecondWindow);
  }
  if (onTop != sOnTop && sSecondWindow != nullptr) {
    SDL_SetWindowAlwaysOnTop(sSecondWindow, onTop ? SDL_TRUE : SDL_FALSE);
    sOnTop = onTop;
  }
  if (sSecondWindow == nullptr) {
    sOnTop = false;
  }
}

#endif

} // namespace

extern "C" void SIM_Display_Init(SDL_Window *mainWindow, SDL_GLContext context) {
#ifdef SDK_BUILD_ANDROID
  (void)mainWindow;
  (void)context;
  sEglDisplay = eglGetCurrentDisplay();
  sEglContext = eglGetCurrentContext();

  // Reuse the exact config SDL picked so the second surface is compatible with
  // the shared context.
  EGLint configId = 0;
  EGLint numConfigs = 0;
  eglQueryContext(sEglDisplay, sEglContext, EGL_CONFIG_ID, &configId);
  const EGLint attribs[] = {EGL_CONFIG_ID, configId, EGL_NONE};
  if (!eglChooseConfig(sEglDisplay, attribs, &sEglConfig, 1, &numConfigs) ||
      numConfigs < 1) {
    DISPLAY_LOG("eglChooseConfig failed for config %d", configId);
    sEglConfig = nullptr;
  }
#else
  sMainWindow = mainWindow;
  sContext = context;
#endif
}

extern "C" BOOL SIM_Display_SecondaryActive(void) {
  const SIM_config_type *config = SIM_GetConfigPtr();
#ifdef SDK_BUILD_ANDROID
  if (!config->secondaryScreen || sEglConfig == nullptr) {
    return FALSE;
  }
  std::lock_guard<std::mutex> guard(sLock);
  return sWindow != nullptr ? TRUE : FALSE;
#else
  UpdateSecondWindow();
  return (config->secondaryScreen && sSecondWindow != nullptr) ? TRUE : FALSE;
#endif
}

extern "C" BOOL SIM_Display_BeginSecondary(int *width, int *height) {
#ifdef SDK_BUILD_ANDROID
  if (sEglConfig == nullptr) {
    return FALSE;
  }

  sLock.lock();
  if (sWindow == nullptr) {
    sLock.unlock();
    return FALSE;
  }

  if (sSurface == EGL_NO_SURFACE) {
    EGLint format = 0;
    eglGetConfigAttrib(sEglDisplay, sEglConfig, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(sWindow, 0, 0, format);
    sSurface = eglCreateWindowSurface(sEglDisplay, sEglConfig, sWindow, nullptr);
    if (sSurface == EGL_NO_SURFACE) {
      DISPLAY_LOG("eglCreateWindowSurface failed: 0x%x", eglGetError());
      // Drop the window so we don't retry every frame; Java will hand over a new one
      ReleaseWindowLocked();
      sLock.unlock();
      return FALSE;
    }
    DISPLAY_LOG("Secondary surface ready (%dx%d)", ANativeWindow_getWidth(sWindow),
                ANativeWindow_getHeight(sWindow));
  }

  sMainDraw = eglGetCurrentSurface(EGL_DRAW);
  sMainRead = eglGetCurrentSurface(EGL_READ);
  if (!eglMakeCurrent(sEglDisplay, sSurface, sSurface, sEglContext)) {
    DISPLAY_LOG("eglMakeCurrent(secondary) failed: 0x%x", eglGetError());
    sLock.unlock();
    return FALSE;
  }
  // Never let the second panel's vsync stall the frame; the main window paces us.
  eglSwapInterval(sEglDisplay, 0);

  EGLint w = 0;
  EGLint h = 0;
  eglQuerySurface(sEglDisplay, sSurface, EGL_WIDTH, &w);
  eglQuerySurface(sEglDisplay, sSurface, EGL_HEIGHT, &h);
  *width = w;
  *height = h;

  sHoldingLock = true; // released in SIM_Display_EndSecondary
  return TRUE;
#else
  if (sSecondWindow == nullptr) {
    return FALSE;
  }
  sMainSwapInterval = SDL_GL_GetSwapInterval();
  if (SDL_GL_MakeCurrent(sSecondWindow, sContext) != 0) {
    return FALSE;
  }
  SDL_GL_SetSwapInterval(0);
  SDL_GL_GetDrawableSize(sSecondWindow, width, height);
  return TRUE;
#endif
}

extern "C" void SIM_Display_EndSecondary(void) {
#ifdef SDK_BUILD_ANDROID
  eglSwapBuffers(sEglDisplay, sSurface);
  eglMakeCurrent(sEglDisplay, sMainDraw, sMainRead, sEglContext);
  if (sHoldingLock) {
    sHoldingLock = false;
    sLock.unlock();
  }
#else
  SDL_GL_SwapWindow(sSecondWindow);
  SDL_GL_MakeCurrent(sMainWindow, sContext);
  SDL_GL_SetSwapInterval(sMainSwapInterval);
#endif
}

extern "C" BOOL SIM_Display_ProcessEvent(const SDL_Event *event) {
#ifdef SDK_BUILD_ANDROID
  // Secondary display input comes in through JNI, not SDL
  (void)event;
  return FALSE;
#else
  if (event->type == SDL_WINDOWEVENT && sMainWindow != nullptr &&
      event->window.windowID == SDL_GetWindowID(sMainWindow) &&
      (event->window.event == SDL_WINDOWEVENT_MOVED || event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
    NotePlacement(sMainWindow); // and let the main loop see it too
  }
  if (sSecondWindow == nullptr) {
    return FALSE;
  }
  const Uint32 secondId = SDL_GetWindowID(sSecondWindow);

  switch (event->type) {
  case SDL_WINDOWEVENT:
    if (event->window.windowID != secondId) {
      return FALSE;
    }
    if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
      // Closing the bottom window falls back to the single-window layout
      SDL_DestroyWindow(sSecondWindow);
      sSecondWindow = nullptr;
      sUserClosedWindow = true;
      sTouchDown = false;
    } else if (event->window.event == SDL_WINDOWEVENT_MOVED ||
               event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
      NotePlacement(sSecondWindow);
    }
    return TRUE;
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP:
    if (event->button.windowID != secondId) {
      return FALSE;
    }
    if (event->button.button == SDL_BUTTON_LEFT) {
      sTouchDown = (event->type == SDL_MOUSEBUTTONDOWN);
      sTouchX = event->button.x;
      sTouchY = event->button.y;
      int x, y;
      SIM_Display_GetSecondaryTouch(&x, &y);
      QueueTouch(event->type == SDL_MOUSEBUTTONDOWN ? SIM_TOUCH_DOWN : SIM_TOUCH_UP, x, y);
    }
    return TRUE;
  case SDL_MOUSEMOTION:
    if (event->motion.windowID != secondId) {
      return FALSE;
    }
    sTouchX = event->motion.x;
    sTouchY = event->motion.y;
    {
      int x, y;
      SIM_Display_GetSecondaryTouch(&x, &y);
      QueueTouch(SIM_TOUCH_MOVE, x, y);
    }
    return TRUE;
  default:
    return FALSE;
  }
#endif
}

extern "C" BOOL SIM_Display_PollSecondaryTouch(SIM_TouchEvent *event) {
  std::lock_guard<std::mutex> guard(sTouchQueueLock);
  if (sTouchQueue.empty()) {
    return FALSE;
  }
  *event = sTouchQueue.front();
  sTouchQueue.pop_front();
  return TRUE;
}

extern "C" BOOL SIM_Display_GetSecondaryTouch(int *x, int *y) {
  *x = sTouchX;
  *y = sTouchY;
#ifndef SDK_BUILD_ANDROID
  // Window coordinates -> drawable pixels (HiDPI windows)
  if (sSecondWindow != nullptr) {
    int winW, winH, drawW, drawH;
    SDL_GetWindowSize(sSecondWindow, &winW, &winH);
    SDL_GL_GetDrawableSize(sSecondWindow, &drawW, &drawH);
    if (winW > 0 && winH > 0) {
      *x = *x * drawW / winW;
      *y = *y * drawH / winH;
    }
  }
#endif
  return sTouchDown ? TRUE : FALSE;
}

extern "C" void SIM_Display_MainWindowPosition(int width, int height, int *x, int *y) {
#ifdef SDK_BUILD_ANDROID
  (void)width;
  (void)height;
  *x = 0;
  *y = 0;
#else
  const SIM_config_type *config = SIM_GetConfigPtr();
  if (PlacementVisible(config->windowX, config->windowY, width, height)) {
    *x = config->windowX;
    *y = config->windowY;
  } else {
    *x = SDL_WINDOWPOS_CENTERED;
    *y = SDL_WINDOWPOS_CENTERED;
  }
#endif
}

extern "C" BOOL SIM_Display_IsFullScreen(BOOL bottom) {
#ifdef SDK_BUILD_ANDROID
  (void)bottom;
  return TRUE;
#else
  return IsFullScreen(bottom ? sSecondWindow : sMainWindow) ? TRUE : FALSE;
#endif
}

extern "C" void SIM_Display_SetFullScreen(BOOL bottom, BOOL on) {
#ifdef SDK_BUILD_ANDROID
  (void)bottom;
  (void)on;
#else
  SIM_config_type *config = SIM_GetConfigPtr();
  if (bottom) {
    config->bottomFullScreen = on ? TRUE : FALSE;
    SetFullScreen(sSecondWindow, on != FALSE);
  } else {
    config->fullScreen = on ? TRUE : FALSE;
    SetFullScreen(sMainWindow, on != FALSE);
    if (on) {
      MoveBottomOffTopsDisplay();
    }
  }
  SIM_Config_SaveConfigFile(config);
#endif
}

extern "C" void SIM_Display_ToggleFullScreen(void) {
#ifndef SDK_BUILD_ANDROID
  // The window being typed into: F11 / Alt+Enter on the bottom window switches that one
  SDL_Window *focus = SDL_GetKeyboardFocus();
  const BOOL bottom = (focus != nullptr && focus == sSecondWindow) ? TRUE : FALSE;
  SIM_Display_SetFullScreen(bottom, SIM_Display_IsFullScreen(bottom) ? FALSE : TRUE);
#endif
}

extern "C" void SIM_Display_Tick(void) {
#ifndef SDK_BUILD_ANDROID
  UpdateStacking();
  if (sPlacementDirty && SDL_GetTicks() - sPlacementChangedAt > 1000) {
    sPlacementDirty = false;
    SIM_Config_SaveConfigFile(SIM_GetConfigPtr());
  }
#endif
}

extern "C" void SIM_Display_SavePlacement(void) {
#ifndef SDK_BUILD_ANDROID
  NotePlacement(sMainWindow);
  NotePlacement(sSecondWindow);
  if (sPlacementDirty) {
    sPlacementDirty = false;
    SIM_Config_SaveConfigFile(SIM_GetConfigPtr());
  }
#endif
}

#ifdef SDK_BUILD_ANDROID
// JNI entry points for org.pokeplatinum.port.SecondaryScreen (Java side of the Presentation)
extern "C" {

JNIEXPORT void JNICALL Java_org_pokeplatinum_port_SecondaryScreen_nativeSurfaceChanged(
    JNIEnv *env, jclass, jobject surface) {
  ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
  std::lock_guard<std::mutex> guard(sLock);
  if (window == sWindow) {
    // Same surface, new size: the EGL surface follows the window size by itself
    if (window != nullptr) {
      ANativeWindow_release(window);
    }
    return;
  }
  ReleaseWindowLocked();
  sWindow = window;
}

JNIEXPORT void JNICALL
Java_org_pokeplatinum_port_SecondaryScreen_nativeSurfaceDestroyed(JNIEnv *, jclass) {
  // Blocks until the render thread is done with the surface (at most one draw)
  std::lock_guard<std::mutex> guard(sLock);
  ReleaseWindowLocked();
}

JNIEXPORT void JNICALL Java_org_pokeplatinum_port_SecondaryScreen_nativeTouch(
    JNIEnv *, jclass, jboolean down, jfloat x, jfloat y) {
  const bool wasDown = sTouchDown;
  sTouchX = (int)x;
  sTouchY = (int)y;
  sTouchDown = down ? true : false;
  QueueTouch(down ? (wasDown ? SIM_TOUCH_MOVE : SIM_TOUCH_DOWN) : SIM_TOUCH_UP, (int)x, (int)y);
}

} // extern "C"
#endif

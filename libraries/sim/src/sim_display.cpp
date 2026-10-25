// Secondary screen output: see include/simulator/sim_display.h
#include <simulator/sim.h>
#include <simulator/sim_display.h>
#include <simulator/sim_opengl.h>

#include <atomic>
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

void UpdateSecondWindow() {
  const SIM_config_type *config = SIM_GetConfigPtr();
  const bool wanted = config->secondaryScreen && !sUserClosedWindow;

  if (wanted && sSecondWindow == nullptr && sMainWindow != nullptr) {
    int mainX, mainY, mainW, mainH;
    SDL_GetWindowPosition(sMainWindow, &mainX, &mainY);
    SDL_GetWindowSize(sMainWindow, &mainW, &mainH);
    sSecondWindow = SDL_CreateWindow(
        "Bottom Screen", mainX + mainW + 16, mainY, SIM_NDS_SCREEN_WIDTH * 2,
        SIM_NDS_SCREEN_HEIGHT * 2,
        SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (sSecondWindow == nullptr) {
      printf("Could not create the bottom screen window: %s\n", SDL_GetError());
      sUserClosedWindow = true; // don't retry every frame
    }
  } else if (!config->secondaryScreen && sSecondWindow != nullptr) {
    SDL_DestroyWindow(sSecondWindow);
    sSecondWindow = nullptr;
  }

  if (!config->secondaryScreen) {
    // Turning the option back on reopens a window the user closed
    sUserClosedWindow = false;
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
    }
    return TRUE;
  case SDL_MOUSEMOTION:
    if (event->motion.windowID != secondId) {
      return FALSE;
    }
    sTouchX = event->motion.x;
    sTouchY = event->motion.y;
    return TRUE;
  default:
    return FALSE;
  }
#endif
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
  sTouchX = (int)x;
  sTouchY = (int)y;
  sTouchDown = down ? true : false;
}

} // extern "C"
#endif

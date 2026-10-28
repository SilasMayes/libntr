// Companion UI on the secondary output. See include/simulator/sim_companion.h
#include <simulator/sim.h>
#include <simulator/sim_companion.h>
#include <simulator/sim_display.h>
#include <simulator/sim_opengl.h>

#include <SDL2/SDL.h>

#include <cfloat>
#include <vector>

#include "imgui_impl_opengl3.h"
#include "simulator/imgui/imgui.hpp"

namespace {

ImGuiContext *sContext = nullptr;
bool sFullScreen = false;
std::vector<SIM_TouchEvent> sPendingTouches;
bool sReleaseHover = false;
Uint64 sLastFrameTicks = 0;

// Below this share of the output height the companion strip would be too cramped to be
// useful; the companion is then only shown full screen.
constexpr float kMinCompanionShare = 0.15f;

} // namespace

extern "C" unsigned int SIM_UI_CreateImage(int width, int height, const void *rgba) {
  if (width <= 0 || height <= 0 || rgba == nullptr) {
    return 0;
  }
  GLint previous = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
  return texture;
}

extern "C" void SIM_UI_DeleteImage(unsigned int image) {
  if (image != 0) {
    GLuint texture = image;
    glDeleteTextures(1, &texture);
  }
}

extern "C" void SIM_Companion_SetFullScreen(BOOL fullScreen) {
  sFullScreen = fullScreen ? true : false;
}

extern "C" BOOL SIM_Companion_IsFullScreen(void) {
  return sFullScreen ? TRUE : FALSE;
}

extern "C" void SIM_Companion_ToggleFullScreen(void) {
  if (SIM_Prj_CompanionEnabled()) {
    sFullScreen = !sFullScreen;
  }
}

extern "C" void SIM_Companion_ComputeLayout(int outputWidth, int outputHeight,
                                            SIM_CompanionLayout *layout) {
  *layout = {};

  // The DS screen at full width (or full height if the output is wider than 4:3)
  int dsWidth = outputWidth;
  int dsHeight = outputWidth * SIM_NDS_SCREEN_HEIGHT / SIM_NDS_SCREEN_WIDTH;
  if (dsHeight > outputHeight) {
    dsHeight = outputHeight;
    dsWidth = outputHeight * SIM_NDS_SCREEN_WIDTH / SIM_NDS_SCREEN_HEIGHT;
  }

  const bool enabled = SIM_Prj_CompanionEnabled() != FALSE;
  const int spare = outputHeight - dsHeight;
  const bool canShare =
      enabled && spare >= (int)(outputHeight * kMinCompanionShare);

  layout->frame.outputWidth = (float)outputWidth;
  layout->frame.outputHeight = (float)outputHeight;
  layout->frame.canShareScreen = canShare ? TRUE : FALSE;

  if (enabled && sFullScreen) {
    layout->dsVisible = FALSE;
    layout->companionVisible = TRUE;
    layout->frame.x = 0;
    layout->frame.y = 0;
    layout->frame.width = (float)outputWidth;
    layout->frame.height = (float)outputHeight;
    layout->frame.fullScreen = TRUE;
    return;
  }

  layout->dsVisible = TRUE;
  layout->dsWidth = dsWidth;
  layout->dsHeight = dsHeight;
  layout->dsX = (outputWidth - dsWidth) / 2;

  if (canShare) {
    // DS screen against the top edge (next to the hinge on the Thor), companion below
    layout->dsY = 0;
    layout->companionVisible = TRUE;
    layout->frame.x = 0;
    layout->frame.y = (float)dsHeight;
    layout->frame.width = (float)outputWidth;
    layout->frame.height = (float)(outputHeight - dsHeight);
  } else {
    layout->dsY = (outputHeight - dsHeight) / 2;
  }
}

extern "C" void SIM_Companion_QueueTouch(int type, int x, int y) {
  if (sPendingTouches.size() < 256) {
    sPendingTouches.push_back({type, x, y});
  }
}

extern "C" void SIM_Companion_Render(const SIM_CompanionLayout *layout) {
  if (!layout->companionVisible) {
    sPendingTouches.clear();
    return;
  }

  ImGuiContext *previous = ImGui::GetCurrentContext();

  if (sContext == nullptr) {
    sContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(sContext);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing to remember between runs
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
#ifdef SIM_GLES
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 420");
#endif
    SIM_Prj_CompanionSetup(layout->frame.outputWidth);
  } else {
    ImGui::SetCurrentContext(sContext);
  }

  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(layout->frame.outputWidth, layout->frame.outputHeight);
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

  const Uint64 now = SDL_GetTicks64();
  io.DeltaTime = sLastFrameTicks != 0 ? (float)(now - sLastFrameTicks) / 1000.0f : 1.0f / 60.0f;
  if (io.DeltaTime <= 0.0f) {
    io.DeltaTime = 1.0f / 60.0f;
  }
  sLastFrameTicks = now;

  // A finger lifted last frame: stop "hovering" where it was
  if (sReleaseHover && sPendingTouches.empty()) {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    sReleaseHover = false;
  }
  for (const SIM_TouchEvent &touch : sPendingTouches) {
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent((float)touch.x, (float)touch.y);
    if (touch.type == SIM_TOUCH_DOWN) {
      io.AddMouseButtonEvent(0, true);
      sReleaseHover = false;
    } else if (touch.type == SIM_TOUCH_UP) {
      io.AddMouseButtonEvent(0, false);
      sReleaseHover = true;
    }
  }
  sPendingTouches.clear();

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  SIM_Prj_CompanionDraw(&layout->frame);
  ImGui::Render();

  glViewport(0, 0, (GLsizei)layout->frame.outputWidth, (GLsizei)layout->frame.outputHeight);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  ImGui::SetCurrentContext(previous);
}

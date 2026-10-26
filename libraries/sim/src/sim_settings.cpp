// The settings screen. See include/simulator/sim_settings.hpp
#include "simulator/sim_settings.hpp"

#include "simulator/config/sim_config.h"
#include "simulator/sim.h"
#include "simulator/sim_gui.hpp"
#include "simulator/sim_ui.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void __attribute__((weak)) SIM_Prj_SettingsPages() {
}

namespace SIM::Settings {

namespace {

using namespace SIM::UI;

struct Page {
  std::string name;
  std::string summary;
  std::vector<void (*)()> draw;
  int order;
};

std::vector<Page> sPages;
bool sPagesAdded = false;
bool sOpen = false;
int sPage = 0;          // the page shown
bool sShowList = true;  // narrow screens: the list of pages instead of a page
bool sFocusPage = false; // wide screens: the pad's cursor goes into the page

void EnsurePages();

// --- Display ------------------------------------------------------------------------------

void SaveConfig() {
  SIM_Config_SaveConfigFile(SIM_GetConfigPtr());
}

void DrawDisplay() {
  SIM_config_type *config = SIM_GetConfigPtr();

  int resolution = (int)config->internalResolutionScale;
  if (Stepper("3D resolution", "How sharp 3D scenes are. Higher looks better and uses more power.", &resolution, 1, 20,
              1, "%dx")) {
    config->internalResolutionScale = (u32)resolution;
    SIM_SetInternalResolutionAfterRender(resolution);
    SaveConfig();
  }

#ifdef SDK_BUILD_ANDROID
  const char *secondLabel = "Bottom screen on the second display";
  const char *secondHelp = "On dual-screen devices like the AYN Thor. Off puts both screens on this display.";
#else
  const char *secondLabel = "Bottom screen in its own window";
  const char *secondHelp = "Off puts both screens in this window.";
#endif
  bool second = config->secondaryScreen != FALSE;
  if (Toggle(secondLabel, secondHelp, &second)) {
    config->secondaryScreen = second ? TRUE : FALSE;
    SaveConfig();
  }

  static const char *const kLayouts[] = { "Stacked", "Side by side", "Large top" };
  int layout = (int)config->screenLayout;
  if (Choice("Screen layout", "How the screens share one display.", &layout, kLayouts, 3)) {
    config->screenLayout = (SIM_config_screen_layout_type)layout;
    SaveConfig();
  }

  bool swap = config->swapScreens != FALSE;
  if (Toggle("Swap screens", "Show the bottom screen where the top one goes, and the other way round.", &swap)) {
    config->swapScreens = swap ? TRUE : FALSE;
    SaveConfig();
  }

#ifndef SDK_BUILD_ANDROID
  bool fullScreen = config->fullScreen != 0;
  if (Toggle("Full screen", nullptr, &fullScreen)) {
    config->fullScreen = fullScreen ? 1 : 0;
    SDL_SetWindowFullscreen(SIM_GetSDLWindow(), fullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SaveConfig();
  }
#endif

  Section("Frame timing");
  static const char *const kVsync[] = { "Off", "On", "Half rate" };
  int vsync = config->vsyncInterval < 3 ? config->vsyncInterval : 1;
  if (Choice("Vertical sync", "On avoids tearing. Half rate shows every second refresh.", &vsync, kVsync, 3)) {
    config->vsyncInterval = (u8)vsync;
    SDL_GL_SetSwapInterval(vsync);
    SaveConfig();
  }
  bool cap = config->capFrameRate != 0;
  if (Toggle("Run at the game's speed", "Off runs as fast as the device can (fast forward).", &cap)) {
    config->capFrameRate = cap ? 1 : 0;
    SaveConfig();
  }
}

// --- Sound --------------------------------------------------------------------------------

void DrawSound() {
  SIM_config_type *config = SIM_GetConfigPtr();
  int percent = (config->masterVolume * 100 + 63) / 127;
  if (Stepper("Volume", "The game's music and sounds.", &percent, 0, 100, 5, "%d%%")) {
    config->masterVolume = (u8)((percent * 127 + 50) / 100);
    SaveConfig();
  }
}

// --- Controls -----------------------------------------------------------------------------

struct Control {
  const char *name;
  int SIM_config_pad_settings_type::*key;
  int SIM_config_pad_settings_type::*joyKey; // nullptr: keyboard only
};

const Control kControls[] = {
  { "A", &SIM_config_pad_settings_type::aKey, &SIM_config_pad_settings_type::aJoyKey },
  { "B", &SIM_config_pad_settings_type::bKey, &SIM_config_pad_settings_type::bJoyKey },
  { "X", &SIM_config_pad_settings_type::xKey, &SIM_config_pad_settings_type::xJoyKey },
  { "Y", &SIM_config_pad_settings_type::yKey, &SIM_config_pad_settings_type::yJoyKey },
  { "L", &SIM_config_pad_settings_type::lKey, &SIM_config_pad_settings_type::lJoyKey },
  { "R", &SIM_config_pad_settings_type::rKey, &SIM_config_pad_settings_type::rJoyKey },
  { "Start", &SIM_config_pad_settings_type::startKey, &SIM_config_pad_settings_type::startJoyKey },
  { "Select", &SIM_config_pad_settings_type::selectKey, &SIM_config_pad_settings_type::selectJoyKey },
  { "Up", &SIM_config_pad_settings_type::upKey, &SIM_config_pad_settings_type::upJoyKey },
  { "Down", &SIM_config_pad_settings_type::downKey, &SIM_config_pad_settings_type::downJoyKey },
  { "Left", &SIM_config_pad_settings_type::leftKey, &SIM_config_pad_settings_type::leftJoyKey },
  { "Right", &SIM_config_pad_settings_type::rightKey, &SIM_config_pad_settings_type::rightJoyKey },
  { "Settings menu", &SIM_config_pad_settings_type::guiKey, nullptr },
  { "Fast forward (hold)", &SIM_config_pad_settings_type::frameCapKey, nullptr },
  { "Fast forward (toggle)", &SIM_config_pad_settings_type::frameCapToggleKey, nullptr },
};

// The control waiting for a key or button
int *sCaptureTarget = nullptr;
bool sCaptureJoystick = false;
const char *sCaptureName = nullptr;
Uint64 sCaptureUntil = 0;

std::string JoystickName(int key) {
  char text[32];
  if (key == SIM_CONFIG_JOY_INVALID_KEY) {
    return "None";
  } else if (key & SIM_CONFIG_JOY_HAT_MASK) {
    int hat = key & ~SIM_CONFIG_JOY_HAT_MASK;
    const char *direction = hat == SDL_HAT_UP ? "up" : hat == SDL_HAT_DOWN ? "down" : hat == SDL_HAT_LEFT ? "left"
                          : hat == SDL_HAT_RIGHT ? "right" : "?";
    snprintf(text, sizeof(text), "D-pad %s", direction);
  } else if (key & SIM_CONFIG_JOY_AXIS_MINUS_MASK) {
    snprintf(text, sizeof(text), "Axis %d -", key & ~SIM_CONFIG_JOY_AXIS_MINUS_MASK);
  } else if (key & SIM_CONFIG_JOY_AXIS_PLUS_MASK) {
    snprintf(text, sizeof(text), "Axis %d +", key & ~SIM_CONFIG_JOY_AXIS_PLUS_MASK);
  } else {
    snprintf(text, sizeof(text), "Button %d", key);
  }
  return text;
}

void StartCapture(int *target, bool joystick, const char *name) {
  sCaptureTarget = target;
  sCaptureJoystick = joystick;
  sCaptureName = name;
  sCaptureUntil = SDL_GetTicks64() + 6000;
}

void FinishCapture(int value) {
  if (sCaptureTarget != nullptr) {
    *sCaptureTarget = value;
    SaveConfig();
  }
  sCaptureTarget = nullptr;
}

void ControlRow(const Control &control, bool joystick) {
  SIM_config_pad_settings_type &pad = SIM_GetConfigPtr()->padSettings;
  int *key = joystick ? &(pad.*control.joyKey) : &(pad.*control.key);
  std::string current = joystick ? JoystickName(*key) : std::string(SDL_GetKeyName((SDL_Keycode)*key));
  if (current.empty()) {
    current = "None";
  }
  std::string help = sCaptureTarget == key ? std::string(joystick ? "Press the button for it now (or wait to keep "
                                                                    "it)" : "Press the key for it now (or wait to keep it)")
                                           : current;
  ImGui::PushID(joystick ? 1 : 0);
  if (Button(control.name, help.c_str(), sCaptureTarget == key)) {
    StartCapture(key, joystick, control.name);
  }
  ImGui::PopID();
}

void DrawControls() {
  if (sCaptureTarget != nullptr && SDL_GetTicks64() > sCaptureUntil) {
    sCaptureTarget = nullptr;
  }

#ifdef SDK_BUILD_ANDROID
  Note("Back button or R3: this menu. L3: the Gear on the whole lower screen. The D-pad always works as the D-pad.");
  Section("Controller");
  for (const Control &control : kControls) {
    if (control.joyKey != nullptr && strcmp(control.name, "Up") != 0 && strcmp(control.name, "Down") != 0 &&
        strcmp(control.name, "Left") != 0 && strcmp(control.name, "Right") != 0) {
      ControlRow(control, true);
    }
  }
#else
  Note("Tap a control, then press the key or button for it.");
  Section("Keyboard");
  for (const Control &control : kControls) {
    ControlRow(control, false);
  }
  Section("Controller");
  for (const Control &control : kControls) {
    if (control.joyKey != nullptr) {
      ControlRow(control, true);
    }
  }
#endif
}

// --- Advanced -----------------------------------------------------------------------------

void DrawAdvanced() {
  float frameMs = (float)SIM_GetFullFrameTime() / 1000000.0f;
  SIM_FrameTimings timings;
  SIM_GetFrameTimings(&timings);
  char text[160];
  snprintf(text, sizeof(text), "%.0f frames a second. Game %.1f ms, 3D %.1f ms, drawing %.1f ms, waiting %.1f ms, "
           "swap %.1f ms.", frameMs > 0.0f ? 1000.0f / frameMs : 0.0f, timings.gameMs, timings.g3SubmitMs,
           timings.drawMs, timings.capWaitMs, timings.swapMs);
  Section("Performance");
  Note(text);

  Section("Developer");
  if (Button("Developer windows", "The original debug windows: libntr, multiplayer, the game's tools.")) {
    Close();
    SIM::GUI::ShowDeveloperWindows();
  }
  snprintf(text, sizeof(text), "libntr %s", SIM_GetLibntrGitHash());
  Note(text);
}

void SortPages() {
  std::stable_sort(sPages.begin(), sPages.end(), [](const Page &a, const Page &b) { return a.order < b.order; });
}

void EnsurePages() {
  if (sPagesAdded) {
    return;
  }
  sPagesAdded = true;
  AddPage("Display", "Resolution, screens, frame timing", DrawDisplay, 40);
  AddPage("Sound", "Volume", DrawSound, 50);
  AddPage("Controls", "Buttons and keys", DrawControls, 60);
  AddPage("Advanced", "Performance, developer windows", DrawAdvanced, 90);
  SIM_Prj_SettingsPages();
  SortPages();
}

void DrawPage(const Page &page) {
  for (void (*draw)() : page.draw) {
    draw();
  }
}

} // namespace

void AddPage(const char *name, const char *summary, void (*draw)(), int order) {
  for (Page &page : sPages) {
    if (page.name == name) {
      page.draw.push_back(draw);
      return;
    }
  }
  sPages.push_back({ name, summary != nullptr ? summary : "", { draw }, order });
  SortPages();
}

void Open(const char *page) {
  EnsurePages();
  sOpen = true;
  sShowList = true;
  if (page != nullptr) {
    for (size_t i = 0; i < sPages.size(); i++) {
      if (sPages[i].name == page) {
        sPage = (int)i;
        sShowList = false;
      }
    }
  }
}

void Close() {
  sOpen = false;
  sCaptureTarget = nullptr;
}

bool IsOpen() {
  return sOpen;
}

bool CaptureEvent(const void *sdlEvent) {
  if (!sOpen || sCaptureTarget == nullptr) {
    return false;
  }
  const SDL_Event *event = (const SDL_Event *)sdlEvent;
  if (!sCaptureJoystick) {
    if (event->type == SDL_KEYDOWN) {
      FinishCapture(event->key.keysym.sym);
      return true;
    }
    return false; // releases still release
  }
  switch (event->type) {
  case SDL_JOYBUTTONDOWN:
#ifdef SDK_BUILD_ANDROID
    if (event->jbutton.button >= 11 && event->jbutton.button <= 14) {
      return true; // the D-pad stays the D-pad
    }
#endif
    FinishCapture(event->jbutton.button);
    return true;
  case SDL_JOYHATMOTION:
    if (event->jhat.value == SDL_HAT_UP || event->jhat.value == SDL_HAT_DOWN || event->jhat.value == SDL_HAT_LEFT ||
        event->jhat.value == SDL_HAT_RIGHT) {
      FinishCapture(event->jhat.value | SIM_CONFIG_JOY_HAT_MASK);
      return true;
    }
    return false;
  case SDL_JOYAXISMOTION: {
    const int deadzone = SIM_GetConfigPtr()->padSettings.joyAxisDeadzone;
    if (event->jaxis.value > deadzone * 2) {
      FinishCapture(event->jaxis.axis | SIM_CONFIG_JOY_AXIS_PLUS_MASK);
      return true;
    } else if (event->jaxis.value < -deadzone * 2) {
      FinishCapture(event->jaxis.axis | SIM_CONFIG_JOY_AXIS_MINUS_MASK);
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

void Draw() {
  EnsurePages();
  if (sPages.empty()) {
    Close();
    return;
  }
  if (sPage >= (int)sPages.size()) {
    sPage = 0;
  }

  ImGuiIO &io = ImGui::GetIO();
  const float width = io.DisplaySize.x, height = io.DisplaySize.y;
  const bool wide = width >= height * 1.15f && width >= S(620.0f);
  const bool capturing = sCaptureTarget != nullptr;

  // L / R: previous / next page
  if (!capturing && PadPressed(9) && sPage > 0) {
    sPage--;
    sShowList = false;
  }
  if (!capturing && PadPressed(8) && sPage + 1 < (int)sPages.size()) {
    sPage++;
    sShowList = false;
  }

  // What's shown changed: the pad's cursor goes to its default item
  static int sLastView = -2;
  const int view = wide || !sShowList ? sPage : -1;
  if (view != sLastView) {
    RequestFocus();
    sLastView = view;
  }

  bool close = false;
  bool backToList = false;
  BeginWindow("Settings");
  if (wide) {
    const float sideWidth = std::min(S(280.0f), width * 0.34f);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(sideWidth, height), IM_COL32(18, 22, 30, 255));
    close = BeginPane("##pages", ImVec2(0, 0), ImVec2(sideWidth, height), "Settings", "Close");
    bool sidebarFocused = false;
    for (size_t i = 0; i < sPages.size(); i++) {
      ImGui::PushID((int)i);
      // With the pad, the page follows the cursor
      bool tapped = ListItem(sPages[i].name.c_str(), (int)i == sPage);
      if (LastItemFocused()) {
        sidebarFocused = true;
      }
      if (tapped || (UsingPad() && LastItemFocused())) {
        sPage = (int)i;
      }
      if ((int)i == sPage) {
        SetDefaultFocus();
      }
      ImGui::PopID();
    }
    Space(6.0f);
    Note("L / R: previous / next page. Right: into the page, B: back to this list.");
    EndPane();

    const Page &page = sPages[sPage];
    ImGui::PushID(page.name.c_str());
    BeginPane("##page", ImVec2(sideWidth, 0), ImVec2(width - sideWidth, height), page.name.c_str(), nullptr,
              S(720.0f));
    if (sFocusPage) {
      FocusNextItem();
      sFocusPage = false;
    }
    DrawPage(page);
    EndPane();
    ImGui::PopID();

    // The pad: right goes into the page, B comes back to the list (and closes from there)
    if (!capturing && sidebarFocused && PadPressed(4)) {
      sFocusPage = true;
    }
    if (!capturing && BackPressed()) {
      if (sidebarFocused) {
        close = true;
      } else {
        RequestFocus();
      }
    }
  } else if (sShowList) {
    close = BeginPane("##list", ImVec2(0, 0), io.DisplaySize, "Settings", "Close", S(640.0f));
    for (size_t i = 0; i < sPages.size(); i++) {
      ImGui::PushID((int)i);
      if (Button(sPages[i].name.c_str(), sPages[i].summary.c_str())) {
        sPage = (int)i;
        sShowList = false;
      }
      if ((int)i == sPage) {
        SetDefaultFocus();
      }
      ImGui::PopID();
    }
    EndPane();
    close = close || (!capturing && BackPressed());
  } else {
    const Page &page = sPages[sPage];
    ImGui::PushID(page.name.c_str());
    backToList = BeginPane("##page", ImVec2(0, 0), io.DisplaySize, page.name.c_str(), "Settings", S(640.0f));
    DrawPage(page);
    EndPane();
    ImGui::PopID();
    backToList = backToList || (!capturing && BackPressed());
  }
  EndWindow();

  if (backToList) {
    sShowList = true;
    sCaptureTarget = nullptr;
  }
  if (close) {
    Close();
  }
}

} // namespace SIM::Settings

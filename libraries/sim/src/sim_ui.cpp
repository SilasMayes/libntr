// Touch-friendly menu widgets. See include/simulator/sim_ui.hpp
#include "simulator/sim_ui.hpp"

#include <simulator/sim.h>
#include <simulator/simvariables.h>

#include "imgui_internal.h"

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>

extern "C" void __attribute__((weak)) SIM_Prj_UIFont(const void **regular, int *regularSize, const void **bold,
                                                      int *boldSize) {
  *regular = nullptr;
  *regularSize = 0;
  *bold = nullptr;
  *boldSize = 0;
}

namespace SIM::UI {

namespace {

constexpr ImU32 kBackground = IM_COL32(22, 27, 36, 255);
constexpr ImU32 kTitleBar = IM_COL32(30, 36, 48, 255);
constexpr ImU32 kPanel = IM_COL32(37, 45, 59, 255);
constexpr ImU32 kPanelHover = IM_COL32(44, 54, 71, 255);
constexpr ImU32 kPanelPressed = IM_COL32(58, 71, 93, 255);
constexpr ImU32 kAccent = IM_COL32(96, 172, 232, 255);
constexpr ImU32 kAccentPressed = IM_COL32(72, 144, 204, 255);
constexpr ImU32 kOnAccent = IM_COL32(12, 20, 30, 255);
constexpr ImU32 kText = IM_COL32(236, 240, 247, 255);
constexpr ImU32 kTextDim = IM_COL32(146, 158, 176, 255);
constexpr ImU32 kTextOff = IM_COL32(92, 102, 120, 255);
constexpr ImU32 kError = IM_COL32(242, 116, 104, 255);
constexpr ImU32 kGood = IM_COL32(112, 208, 142, 255);
constexpr ImU32 kSwitchOff = IM_COL32(72, 82, 100, 255);
constexpr ImU32 kFocus = IM_COL32(255, 214, 102, 255);

ImFont *sRegular = nullptr;
ImFont *sBold = nullptr;

bool sDragging = false;    // the current touch scrolls rather than taps
ImGuiID sDragPane = 0;     // the pane the current touch started in
bool sPadMode = false;     // the pad was used last (show where its cursor is)
bool sPadActive = false;   // a menu has the pad
u16 sPreviousKeys = 0x3FF; // REG_KEYINPUT: bits are low while pressed
u16 sPressed = 0;
bool sFocusRequest = false; // the pad's cursor goes to the next default item (SetDefaultFocus)
bool sFocusNext = false;    // FocusNextItem()
bool sInWindow = false;     // between BeginWindow and EndWindow
bool sFocusRequestLate = false; // requested inside the window: it waits for the next frame
ImGuiLastItemData sLastItem; // the last interactive item (rows end with a Dummy)
ImGuiWindow *sLastItemWindow = nullptr;

void RememberItem() {
  sLastItem = GImGui->LastItemData;
  sLastItemWindow = ImGui::GetCurrentWindow();
}

constexpr float kRounding = 12.0f;

bool Hovered() {
  return ImGui::IsItemHovered() && ImGui::GetIO().MouseSource != ImGuiMouseSource_TouchScreen;
}

ImVec2 Measure(ImFont *font, float px, const char *text, float wrap = 0.0f) {
  return font->CalcTextSizeA(px, FLT_MAX, wrap, text);
}

void Text(ImDrawList *draw, ImFont *font, float px, ImVec2 pos, ImU32 color, const char *text, float wrap = 0.0f) {
  draw->AddText(font, px, ImVec2(std::floor(pos.x), std::floor(pos.y)), color, text, nullptr, wrap);
}

ImU32 RowColor(bool held, bool hovered) {
  return held ? kPanelPressed : (hovered ? kPanelHover : kPanel);
}

// Where the pad's cursor is, while the pad is in use
void FocusRing(ImVec2 min, ImVec2 max) {
  if (sPadMode && ImGui::IsItemFocused()) {
    const float grow = S(2.0f);
    ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow), kFocus,
                                        S(kRounding) + grow, 0, S(3.0f));
  }
}

// Label and help text of a row; returns the block's height (measureOnly: just measures)
float RowText(ImDrawList *draw, ImVec2 pos, float width, const char *label, const char *help, bool enabled,
              bool measureOnly, bool onAccent = false) {
  const float labelPx = S(17.0f);
  const float helpPx = S(14.0f);
  ImVec2 labelSize = Measure(BoldFont(), labelPx, label, width);
  float height = labelSize.y;
  if (!measureOnly) {
    Text(draw, BoldFont(), labelPx, pos, onAccent ? kOnAccent : (enabled ? kText : kTextOff), label, width);
  }
  if (help != nullptr && help[0] != '\0') {
    ImVec2 helpSize = Measure(RegularFont(), helpPx, help, width);
    if (!measureOnly) {
      Text(draw, RegularFont(), helpPx, ImVec2(pos.x, pos.y + height + S(3.0f)),
           onAccent ? IM_COL32(12, 20, 30, 200) : (enabled ? kTextDim : kTextOff), help, width);
    }
    height += S(3.0f) + helpSize.y;
  }
  return height;
}

bool Tapped(bool pressed) {
  return pressed && !sDragging;
}

// Ends a row drawn by hand at pos with the given size: moves the layout past it
void EndRow(ImVec2 pos, float width, float height) {
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height));
  ImGui::Dummy(ImVec2(width, S(8.0f)));
}

bool InvisibleItem(const char *id, ImVec2 size, bool enabled) {
  ImGui::BeginDisabled(!enabled);
  bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
  ImGui::EndDisabled();
  RememberItem();
  if (sFocusNext && enabled) {
    ImGui::SetFocusID(GImGui->LastItemData.ID, ImGui::GetCurrentWindow());
    ImGui::ScrollToItem();
    sFocusNext = false;
  }
  return pressed;
}

} // namespace

float Scale() {
  float scale = ImGui::GetIO().DisplaySize.y / 540.0f;
  return scale < 1.0f ? 1.0f : scale;
}

float S(float value) {
  return value * Scale();
}

ImFont *RegularFont() {
  return sRegular != nullptr ? sRegular : ImGui::GetFont();
}

ImFont *BoldFont() {
  return sBold != nullptr ? sBold : RegularFont();
}

void Init() {
  const void *regular = nullptr;
  const void *bold = nullptr;
  int regularSize = 0;
  int boldSize = 0;
  SIM_Prj_UIFont(&regular, &regularSize, &bold, &boldSize);
  if (regular == nullptr || regularSize <= 0) {
    return;
  }
  ImGuiIO &io = ImGui::GetIO();
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false; // the data lives in the game's binary
  sRegular = io.Fonts->AddFontFromMemoryTTF((void *)regular, regularSize, 17.0f, &config);
  if (bold != nullptr && boldSize > 0) {
    sBold = io.Fonts->AddFontFromMemoryTTF((void *)bold, boldSize, 17.0f, &config);
  }
  // The developer windows use it too
  io.FontDefault = sRegular;
}

void FeedPad(bool active) {
  ImGuiIO &io = ImGui::GetIO();
  const u16 keys = s_reg_PAD_KEYINPUT;
  static const ImGuiKey kKeys[10] = {
    ImGuiKey_GamepadFaceDown,  // A
    ImGuiKey_None,             // B: the menus' own back (Dear ImGui's cancel would leave child windows)
    ImGuiKey_None,             // Select
    ImGuiKey_None,             // Start
    ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,
    ImGuiKey_None, ImGuiKey_None, // R, L
  };

  if (!active) {
    if (sPadActive) {
      for (ImGuiKey key : kKeys) {
        if (key != ImGuiKey_None) {
          io.AddKeyEvent(key, false);
        }
      }
      io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
      io.ConfigNavCursorVisibleAlways = false;
      sPadActive = false;
    }
    sPreviousKeys = keys;
    sPressed = 0;
    return;
  }

  // The DS buttons as the player mapped them drive Dear ImGui's gamepad navigation. The
  // cursor always exists (so A works straight away); it is only drawn while the pad is
  // in use.
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.ConfigNavCursorVisibleAlways = true;
  const u16 changed = sPadActive ? (u16)(keys ^ sPreviousKeys) : 0x3FF;
  for (int bit = 0; bit < 10; bit++) {
    if (kKeys[bit] != ImGuiKey_None && (changed & (1 << bit))) {
      io.AddKeyEvent(kKeys[bit], (keys & (1 << bit)) == 0);
    }
  }
  // Keys held when the menu opened don't count as presses
  sPressed = sPadActive ? (u16)(sPreviousKeys & ~keys & 0x3FF) : 0;
  if (sPressed & 0xF3) {
    sPadMode = true;
  }
  sPreviousKeys = keys;
  sPadActive = true;
}

bool PadPressed(int bit) {
  return (sPressed & (1 << bit)) != 0;
}

bool BackPressed() {
  return PadPressed(1);
}

bool IsDragging() {
  return sDragging;
}

bool UsingPad() {
  return sPadMode;
}

bool LastItemFocused() {
  return sLastItem.ID != 0 && GImGui->NavId == sLastItem.ID;
}

void SetDefaultFocus() {
  ImGuiContext &g = *GImGui;
  ImGuiWindow *window = sLastItemWindow;
  if (sFocusRequest && window == ImGui::GetCurrentWindow() && sLastItem.ID != 0 && g.NavWindow != nullptr &&
      g.NavWindow->RootWindowForNav == window->RootWindowForNav) {
    const ImGuiLastItemData current = g.LastItemData;
    g.LastItemData = sLastItem;
    ImGui::SetFocusID(sLastItem.ID, window);
    if (sPadMode) {
      ImGui::ScrollToItem(); // with touch, the screen starts at its top
    }
    g.LastItemData = current;
    g.NavInitRequest = false; // the window's own init would take it elsewhere
    g.NavInitResult.ID = 0;
    sFocusRequest = false;
  }
}

void RequestFocus() {
  sFocusRequest = true;
  sFocusRequestLate = sInWindow;
}

void FocusNextItem() {
  sFocusNext = true;
}

void BeginWindow(const char *id) {
  ImGuiIO &io = ImGui::GetIO();
  if (io.MouseClicked[0]) {
    sDragging = false;
    sPadMode = false;
    sDragPane = 0;
  }
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImColor(kBackground).Value);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0)); // FocusRing draws it
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
  ImGui::Begin(id, nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar(3);
  if (ImGui::IsWindowAppearing()) {
    sFocusRequest = true;
  }
  sInWindow = true;
}

void EndWindow() {
  ImGuiContext &g = *GImGui;
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (sFocusRequest && !sFocusRequestLate && g.NavWindow != nullptr && g.NavWindow->RootWindowForNav == window) {
    // No default item: the first one
    ImGui::NavInitWindow(g.NavWindow, true);
    sFocusRequest = false;
  }
  sFocusRequestLate = false;
  sInWindow = false;
  ImGui::End();
  ImGui::PopStyleColor(4);
}

bool BeginPane(const char *id, ImVec2 pos, ImVec2 size, const char *title, const char *backLabel,
               float maxColumnWidth) {
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetCursorScreenPos(pos);
  ImGui::BeginChild(id, size, ImGuiChildFlags_NavFlattened,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

  bool back = false;
  float barHeight = 0.0f;
  if (title != nullptr) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    barHeight = S(54.0f);
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + barHeight), kTitleBar);

    float titleX = pos.x + S(18.0f);
    if (backLabel != nullptr) {
      char label[64];
      snprintf(label, sizeof(label), "<  %s", backLabel);
      ImVec2 textSize = Measure(BoldFont(), S(16.0f), label);
      ImVec2 buttonSize(textSize.x + S(28.0f), barHeight - S(14.0f));
      ImVec2 buttonPos(pos.x + S(8.0f), pos.y + S(7.0f));
      ImGui::SetCursorScreenPos(buttonPos);
      ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true); // the pad starts on the content (B is back)
      bool pressed = ImGui::InvisibleButton("##back", buttonSize, ImGuiButtonFlags_EnableNav);
      ImGui::PopItemFlag();
      bool held = ImGui::IsItemActive();
      ImVec2 max(buttonPos.x + buttonSize.x, buttonPos.y + buttonSize.y);
      draw->AddRectFilled(buttonPos, max, held ? kPanelPressed : (Hovered() ? kPanelHover : kPanel), S(10.0f));
      Text(draw, BoldFont(), S(16.0f), ImVec2(buttonPos.x + S(14.0f), buttonPos.y + (buttonSize.y - textSize.y) * 0.5f),
           kText, label);
      FocusRing(buttonPos, max);
      back = pressed;
      titleX = max.x + S(14.0f);
    }
    float titleWidth = pos.x + size.x - titleX - S(12.0f);
    ImVec2 titleSize = Measure(BoldFont(), S(21.0f), title);
    draw->PushClipRect(ImVec2(titleX, pos.y), ImVec2(titleX + titleWidth, pos.y + barHeight), true);
    Text(draw, BoldFont(), S(21.0f), ImVec2(titleX, pos.y + (barHeight - titleSize.y) * 0.5f), kText, title);
    draw->PopClipRect();
  }

  // The scrolling column, centred
  const float sideMargin = S(16.0f);
  float column = size.x - sideMargin * 2;
  if (maxColumnWidth > 0.0f && column > maxColumnWidth) {
    column = maxColumnWidth;
  }
  const float margin = (size.x - column) * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + barHeight));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin, S(12.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(5.0f));
  ImGui::BeginChild("##scroll", ImVec2(size.x, size.y - barHeight),
                    ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding, 0);
  ImGui::PopStyleVar(2);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.0f), 0.0f));

  // Finger scrolling: a touch that moves becomes a scroll of the pane it started in
  const ImGuiID paneId = ImGui::GetID("##drag");
  if (io.MouseClicked[0] && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
    sDragPane = paneId;
  }
  if (sDragPane == paneId && ImGui::IsMouseDragging(0, S(10.0f))) {
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    sDragging = true;
  }
  return back && !sDragging;
}

void EndPane() {
  ImGui::Dummy(ImVec2(0, S(16.0f)));
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::EndChild();
}

bool BeginScreen(const char *id, const char *title, const char *backLabel, float maxColumnWidth) {
  BeginWindow(id);
  ImGuiIO &io = ImGui::GetIO();
  bool back = BeginPane("##pane", ImVec2(0, 0), io.DisplaySize, title, backLabel, maxColumnWidth);
  return back || (backLabel != nullptr && BackPressed());
}

void EndScreen() {
  EndPane();
  EndWindow();
}

void Space(float px) {
  ImGui::Dummy(ImVec2(0, S(px)));
}

void Section(const char *title) {
  Space(10.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float width = ImGui::GetContentRegionAvail().x;
  ImVec2 size = Measure(BoldFont(), S(15.0f), title, width);
  Text(ImGui::GetWindowDrawList(), BoldFont(), S(15.0f), ImVec2(pos.x + S(4.0f), pos.y), kAccent, title, width);
  ImGui::Dummy(ImVec2(width, size.y + S(8.0f)));
}

void Note(const char *text) {
  float width = ImGui::GetContentRegionAvail().x - S(8.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 size = Measure(RegularFont(), S(14.0f), text, width);
  Text(ImGui::GetWindowDrawList(), RegularFont(), S(14.0f), ImVec2(pos.x + S(4.0f), pos.y), kTextDim, text, width);
  ImGui::Dummy(ImVec2(width, size.y + S(8.0f)));
}

void Status(const char *text, bool error) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }
  float width = ImGui::GetContentRegionAvail().x - S(8.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 size = Measure(BoldFont(), S(15.0f), text, width);
  Text(ImGui::GetWindowDrawList(), BoldFont(), S(15.0f), ImVec2(pos.x + S(4.0f), pos.y), error ? kError : kGood, text,
       width);
  ImGui::Dummy(ImVec2(width, size.y + S(8.0f)));
}

void Progress(const char *text, float fraction) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float pad = S(14.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 textSize = Measure(BoldFont(), S(16.0f), text, width - pad * 2);
  const float barH = S(8.0f);
  const float height = pad + textSize.y + S(10.0f) + barH + pad;
  draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), kPanel, S(kRounding));
  Text(draw, BoldFont(), S(16.0f), ImVec2(pos.x + pad, pos.y + pad), kText, text, width - pad * 2);

  ImVec2 bar(pos.x + pad, pos.y + pad + textSize.y + S(10.0f));
  const float barW = width - pad * 2;
  draw->AddRectFilled(bar, ImVec2(bar.x + barW, bar.y + barH), kSwitchOff, barH * 0.5f);
  if (fraction >= 0.0f) {
    float w = barW * (fraction > 1.0f ? 1.0f : fraction);
    if (w > barH) {
      draw->AddRectFilled(bar, ImVec2(bar.x + w, bar.y + barH), kAccent, barH * 0.5f);
    }
  } else {
    // A segment sliding back and forth
    float t = (float)ImGui::GetTime() * 0.8f;
    float phase = t - std::floor(t);
    float segment = barW * 0.25f;
    float x = (barW - segment) * (phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f);
    draw->AddRectFilled(ImVec2(bar.x + x, bar.y), ImVec2(bar.x + x + segment, bar.y + barH), kAccent, barH * 0.5f);
  }
  EndRow(pos, width, height);
}

bool Toggle(const char *label, const char *help, bool *value, bool enabled) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float pad = S(14.0f);
  const float switchW = S(52.0f), switchH = S(30.0f);
  const float textWidth = width - pad * 3 - switchW;
  const float textHeight = RowText(draw, ImVec2(0, 0), textWidth, label, help, enabled, true);
  const float height = textHeight + pad * 2 > S(58.0f) ? textHeight + pad * 2 : S(58.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 max(pos.x + width, pos.y + height);

  ImGui::PushID(label);
  bool pressed = InvisibleItem("##toggle", ImVec2(width, height), enabled);
  ImGui::PopID();
  bool held = ImGui::IsItemActive() && !sDragging;

  draw->AddRectFilled(pos, max, RowColor(held, Hovered()), S(kRounding));
  RowText(draw, ImVec2(pos.x + pad, pos.y + (height - textHeight) * 0.5f), textWidth, label, help, enabled, false);

  ImVec2 sw(pos.x + width - pad - switchW, pos.y + (height - switchH) * 0.5f);
  ImU32 track = *value ? (enabled ? kAccent : kTextOff) : kSwitchOff;
  draw->AddRectFilled(sw, ImVec2(sw.x + switchW, sw.y + switchH), track, switchH * 0.5f);
  float knobX = *value ? sw.x + switchW - switchH * 0.5f : sw.x + switchH * 0.5f;
  draw->AddCircleFilled(ImVec2(knobX, sw.y + switchH * 0.5f), switchH * 0.5f - S(3.0f),
                        enabled ? IM_COL32(246, 248, 252, 255) : kTextDim);
  FocusRing(pos, max);
  EndRow(pos, width, height);

  if (Tapped(pressed) && enabled) {
    *value = !*value;
    return true;
  }
  return false;
}

bool Choice(const char *label, const char *help, int *value, const char *const *options, int count, bool enabled) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float pad = S(14.0f);
  const float gap = S(6.0f);
  const float inner = width - pad * 2;
  const float textHeight = RowText(draw, ImVec2(0, 0), inner, label, help, enabled, true);

  // Segments side by side, or stacked when they don't fit
  float widest = 0.0f;
  for (int i = 0; i < count; i++) {
    float w = Measure(BoldFont(), S(15.0f), options[i]).x + S(24.0f);
    widest = w > widest ? w : widest;
  }
  const bool stacked = count > 1 && widest * count + gap * (count - 1) > inner;
  const int perRow = stacked ? 1 : count;
  const int rows = (count + perRow - 1) / perRow;
  const float segmentH = S(44.0f);
  const float segmentW = (inner - gap * (perRow - 1)) / perRow;
  const float height = pad + textHeight + S(10.0f) + rows * segmentH + (rows - 1) * gap + pad;
  ImVec2 pos = ImGui::GetCursorScreenPos();

  draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), kPanel, S(kRounding));
  RowText(draw, ImVec2(pos.x + pad, pos.y + pad), inner, label, help, enabled, false);

  bool changed = false;
  ImGui::PushID(label);
  for (int i = 0; i < count; i++) {
    int row = i / perRow, column = i % perRow;
    ImVec2 segPos(pos.x + pad + column * (segmentW + gap), pos.y + pad + textHeight + S(10.0f) + row * (segmentH + gap));
    ImVec2 segMax(segPos.x + segmentW, segPos.y + segmentH);
    ImGui::SetCursorScreenPos(segPos);
    ImGui::PushID(i);
    bool pressed = InvisibleItem("##segment", ImVec2(segmentW, segmentH), enabled);
    ImGui::PopID();
    bool held = ImGui::IsItemActive() && !sDragging;
    bool selected = *value == i;
    ImU32 fill = selected ? (held ? kAccentPressed : (enabled ? kAccent : kTextOff))
                          : (held ? kPanelPressed : (Hovered() ? kPanelHover : kBackground));
    draw->AddRectFilled(segPos, segMax, fill, S(10.0f));
    ImVec2 size = Measure(BoldFont(), S(15.0f), options[i], segmentW - S(8.0f));
    Text(draw, BoldFont(), S(15.0f), ImVec2(segPos.x + (segmentW - size.x) * 0.5f, segPos.y + (segmentH - size.y) * 0.5f),
         selected ? kOnAccent : (enabled ? kText : kTextOff), options[i], segmentW - S(8.0f));
    FocusRing(segPos, segMax);
    if (Tapped(pressed) && enabled && *value != i) {
      *value = i;
      changed = true;
    }
  }
  ImGui::PopID();
  EndRow(pos, width, height);
  return changed;
}

bool Stepper(const char *label, const char *help, int *value, int min, int max, int step, const char *format,
             bool enabled) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float pad = S(14.0f);
  const float buttonW = S(50.0f), buttonH = S(42.0f), valueW = S(84.0f);
  const float controlsW = buttonW * 2 + valueW;
  const float textWidth = width - pad * 3 - controlsW;
  const float textHeight = RowText(draw, ImVec2(0, 0), textWidth, label, help, enabled, true);
  const float height = textHeight + pad * 2 > S(64.0f) ? textHeight + pad * 2 : S(64.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();

  draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), kPanel, S(kRounding));
  RowText(draw, ImVec2(pos.x + pad, pos.y + (height - textHeight) * 0.5f), textWidth, label, help, enabled, false);

  bool changed = false;
  ImVec2 controls(pos.x + width - pad - controlsW, pos.y + (height - buttonH) * 0.5f);
  ImGui::PushID(label);
  for (int side = 0; side < 2; side++) {
    ImVec2 bp(side == 0 ? controls.x : controls.x + buttonW + valueW, controls.y);
    ImVec2 bmax(bp.x + buttonW, bp.y + buttonH);
    ImGui::SetCursorScreenPos(bp);
    const bool can = enabled && (side == 0 ? *value > min : *value < max);
    ImGui::PushID(side);
    // Holding repeats
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    bool pressed = InvisibleItem("##step", ImVec2(buttonW, buttonH), can);
    ImGui::PopItemFlag();
    ImGui::PopID();
    bool held = ImGui::IsItemActive() && !sDragging;
    draw->AddRectFilled(bp, bmax, held ? kPanelPressed : (Hovered() ? kPanelHover : kBackground), S(10.0f));
    const char *sign = side == 0 ? "-" : "+";
    ImVec2 size = Measure(BoldFont(), S(24.0f), sign);
    Text(draw, BoldFont(), S(24.0f), ImVec2(bp.x + (buttonW - size.x) * 0.5f, bp.y + (buttonH - size.y) * 0.5f),
         can ? kText : kTextOff, sign);
    FocusRing(bp, bmax);
    if (Tapped(pressed) && can) {
      *value += side == 0 ? -step : step;
      *value = *value < min ? min : (*value > max ? max : *value);
      changed = true;
    }
  }
  ImGui::PopID();
  char text[48];
  snprintf(text, sizeof(text), format, *value);
  ImVec2 size = Measure(BoldFont(), S(17.0f), text);
  Text(draw, BoldFont(), S(17.0f),
       ImVec2(controls.x + buttonW + (valueW - size.x) * 0.5f, controls.y + (buttonH - size.y) * 0.5f),
       enabled ? kText : kTextOff, text);

  EndRow(pos, width, height);
  return changed;
}

namespace {

bool ButtonAt(const char *label, const char *help, bool primary, bool enabled, float width, ImVec2 pos) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float pad = S(14.0f);
  const bool centred = primary && (help == nullptr || help[0] == '\0');
  const float textWidth = centred ? width - pad * 2 : width - pad * 3 - S(10.0f);
  const float textHeight = RowText(draw, ImVec2(0, 0), textWidth, label, help, enabled, true);
  const float height = textHeight + pad * 2 > S(54.0f) ? textHeight + pad * 2 : S(54.0f);
  ImVec2 max(pos.x + width, pos.y + height);

  ImGui::SetCursorScreenPos(pos);
  ImGui::PushID(label);
  bool pressed = InvisibleItem("##button", ImVec2(width, height), enabled);
  ImGui::PopID();
  bool held = ImGui::IsItemActive() && !sDragging;
  ImU32 fill = primary && enabled ? (held ? kAccentPressed : kAccent) : RowColor(held, Hovered());
  draw->AddRectFilled(pos, max, fill, S(kRounding));

  if (centred) {
    ImVec2 size = Measure(BoldFont(), S(17.0f), label, textWidth);
    Text(draw, BoldFont(), S(17.0f), ImVec2(pos.x + (width - size.x) * 0.5f, pos.y + (height - size.y) * 0.5f),
         enabled ? kOnAccent : kTextOff, label, textWidth);
  } else {
    RowText(draw, ImVec2(pos.x + pad, pos.y + (height - textHeight) * 0.5f), textWidth, label, help, enabled, false,
            primary && enabled);
    if (!primary) {
      // Reads like a menu entry: a chevron on the right
      const float cx = pos.x + width - pad - S(4.0f), cy = pos.y + height * 0.5f, r = S(6.0f);
      std::array<ImVec2, 3> points = { ImVec2(cx - r, cy - r), ImVec2(cx, cy), ImVec2(cx - r, cy + r) };
      draw->AddPolyline(points.data(), 3, enabled ? kTextDim : kTextOff, 0, S(2.5f));
    }
  }
  FocusRing(pos, max);
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height));
  return Tapped(pressed) && enabled;
}

} // namespace

bool Button(const char *label, const char *help, bool primary, bool enabled) {
  const float width = ImGui::GetContentRegionAvail().x;
  ImVec2 pos = ImGui::GetCursorScreenPos();
  bool tapped = ButtonAt(label, help, primary, enabled, width, pos);
  ImGui::Dummy(ImVec2(width, S(8.0f)));
  return tapped;
}

int ButtonPair(const char *left, const char *right, bool leftPrimary, bool leftEnabled, bool rightEnabled) {
  const float width = ImGui::GetContentRegionAvail().x;
  const float gap = S(8.0f);
  const float half = (width - gap) * 0.5f;
  ImVec2 pos = ImGui::GetCursorScreenPos();
  int result = 0;
  if (ButtonAt(left, nullptr, leftPrimary, leftEnabled, half, pos)) {
    result = 1;
  }
  float bottom = ImGui::GetCursorScreenPos().y;
  if (ButtonAt(right, nullptr, false, rightEnabled, half, ImVec2(pos.x + half + gap, pos.y))) {
    result = 2;
  }
  float bottom2 = ImGui::GetCursorScreenPos().y;
  ImGui::SetCursorScreenPos(ImVec2(pos.x, bottom > bottom2 ? bottom : bottom2));
  ImGui::Dummy(ImVec2(width, S(8.0f)));
  return result;
}

bool Card(const char *title, const char *subtitle, bool selected, bool enabled) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float pad = S(14.0f);
  const float indent = S(8.0f);
  const float textWidth = width - pad * 2 - indent;
  const float textHeight = RowText(draw, ImVec2(0, 0), textWidth, title, subtitle, enabled, true);
  const float height = textHeight + pad * 2 > S(58.0f) ? textHeight + pad * 2 : S(58.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 max(pos.x + width, pos.y + height);

  ImGui::PushID(title);
  bool pressed = InvisibleItem("##card", ImVec2(width, height), enabled);
  ImGui::PopID();
  bool held = ImGui::IsItemActive() && !sDragging;
  draw->AddRectFilled(pos, max, selected && !held ? kPanelPressed : RowColor(held, Hovered()), S(kRounding));
  if (selected) {
    draw->AddRectFilled(ImVec2(pos.x + S(6.0f), pos.y + S(12.0f)), ImVec2(pos.x + S(10.0f), max.y - S(12.0f)), kAccent,
                        S(2.0f));
  }
  RowText(draw, ImVec2(pos.x + pad + indent, pos.y + (height - textHeight) * 0.5f), textWidth, title, subtitle, enabled,
          false);
  FocusRing(pos, max);
  EndRow(pos, width, height);
  return Tapped(pressed) && enabled;
}

bool ListItem(const char *label, bool selected) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float height = S(46.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 max(pos.x + width, pos.y + height);

  ImGui::PushID(label);
  bool pressed = InvisibleItem("##item", ImVec2(width, height), true);
  ImGui::PopID();
  bool held = ImGui::IsItemActive() && !sDragging;
  if (selected || held || Hovered()) {
    draw->AddRectFilled(pos, max, held ? kPanelPressed : (selected ? kPanel : kPanelHover), S(10.0f));
  }
  if (selected) {
    draw->AddRectFilled(ImVec2(pos.x + S(6.0f), pos.y + S(11.0f)), ImVec2(pos.x + S(10.0f), max.y - S(11.0f)), kAccent,
                        S(2.0f));
  }
  ImVec2 size = Measure(BoldFont(), S(17.0f), label);
  Text(draw, BoldFont(), S(17.0f), ImVec2(pos.x + S(20.0f), pos.y + (height - size.y) * 0.5f),
       selected ? kText : kTextDim, label);
  FocusRing(pos, max);
  ImGui::SetCursorScreenPos(ImVec2(pos.x, max.y));
  ImGui::Dummy(ImVec2(width, S(4.0f)));
  return Tapped(pressed);
}

} // namespace SIM::UI

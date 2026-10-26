#ifndef LIBNTR_SIM_UI_HPP
#define LIBNTR_SIM_UI_HPP

// Touch-friendly building blocks for libntr's full-screen menus (settings, game picker,
// adding a game) and the game's own settings pages: big rows that span the width,
// switches, segmented choices, -/+ steppers, finger scrolling, and the game pad (D-pad to
// move, A to choose, B to go back) through Dear ImGui's gamepad navigation.
//
// A screen is one full-window ImGui window (BeginWindow) holding one or more panes
// (BeginPane): a title bar over a scrolling column the widgets go in.

#include "simulator/imgui/imgui.hpp"

namespace SIM::UI {

// Size unit: the main window's height / 540 (a 540p layout scaled up)
float Scale();
float S(float value);

ImFont *RegularFont(); // the game's UI font when it provides one (SIM_Prj_UIFont)
ImFont *BoldFont();

void BeginWindow(const char *id);
void EndWindow();

// A pane at pos (screen coordinates) of the given size: a title bar (with a back button
// when backLabel != nullptr, none at all when title == nullptr) and a scrolling column at
// most maxColumnWidth wide (0: the pane's width). Returns TRUE when back was tapped.
bool BeginPane(const char *id, ImVec2 pos, ImVec2 size, const char *title, const char *backLabel = nullptr,
               float maxColumnWidth = 0.0f);
void EndPane();

// A window with a single pane. Also returns TRUE when B was pressed.
bool BeginScreen(const char *id, const char *title, const char *backLabel = "Back", float maxColumnWidth = 0.0f);
void EndScreen();

void Section(const char *title);
void Note(const char *text); // dim, wrapped
void Status(const char *text, bool error = false);
void Progress(const char *text, float fraction); // fraction < 0: no known progress (animated)
bool Toggle(const char *label, const char *help, bool *value, bool enabled = true);
bool Choice(const char *label, const char *help, int *value, const char *const *options, int count,
            bool enabled = true);
bool Stepper(const char *label, const char *help, int *value, int min, int max, int step, const char *format,
             bool enabled = true);
bool Button(const char *label, const char *help = nullptr, bool primary = false, bool enabled = true);
// A two-line card (title, subtitle); selected draws it highlighted
bool Card(const char *title, const char *subtitle, bool selected = false, bool enabled = true);
// A compact entry of a list of pages; selected draws it highlighted
bool ListItem(const char *label, bool selected);
// Two buttons side by side; returns 1 or 2 for the one pressed, 0 for none
int ButtonPair(const char *left, const char *right, bool leftPrimary = false, bool leftEnabled = true,
               bool rightEnabled = true);
void Space(float px = 8.0f);

// The item just drawn is where the pad's cursor goes when the screen opens, or after
// RequestFocus() (when what the screen shows changes)
void SetDefaultFocus();
void RequestFocus();
// The pad's cursor goes to the next row or button drawn (that can be used)
void FocusNextItem();

// A finger is scrolling the screen: releases should not count as taps
bool IsDragging();
// The pad was used last (rather than touch or the mouse)
bool UsingPad();
// The pad's cursor is on the last row or button drawn
bool LastItemFocused();

// The DS buttons while a menu is up (bit numbers of REG_KEYINPUT: 0 A, 1 B, 2 Select,
// 3 Start, 4 Right, 5 Left, 6 Up, 7 Down, 8 R, 9 L)
bool PadPressed(int bit);
bool BackPressed(); // B

// libntr internals
void Init();               // with the main ImGui context current, after the GUI's own init
void FeedPad(bool active); // between the platform backend's NewFrame and ImGui::NewFrame

} // namespace SIM::UI

extern "C" {
// Game hook (weak, none by default): TrueType data of the font for libntr's menus
void SIM_Prj_UIFont(const void **regular, int *regularSize, const void **bold, int *boldSize);
}

#endif // LIBNTR_SIM_UI_HPP

#ifndef LIBNTR_SIM_SETTINGS_HPP
#define LIBNTR_SIM_SETTINGS_HPP

// The settings screen: a full-window, touch- and pad-friendly menu (SIM::UI widgets) with
// a list of pages. It opens with the settings key (Android: back button / R3; desktop:
// the GUI key), and the game waits while it is open.
//
// libntr has the Display, Sound, Controls and Advanced pages; the game adds its own from
// SIM_Prj_SettingsPages().

namespace SIM::Settings {

// Adds a page drawn by draw() (SIM::UI widgets) at the given place (lower first; libntr's
// pages: Display 40, Sound 50, Controls 60, Advanced 90). Adding to an existing page's
// name appends draw() to it instead.
void AddPage(const char *name, const char *summary, void (*draw)(), int order);

// For a page with views of its own (an entry opened from a list, a picker): B was pressed
// and goes back a view in the page (rather than out of it). Call from the page's draw().
bool PageBack();

void Open(const char *page = nullptr);
void Close();
bool IsOpen();
void Draw(); // libntr internal: every frame while open

// A key or button for a control is being chosen: SDL input goes to it (libntr internal,
// from the event loop). Returns TRUE when the event was used.
bool CaptureEvent(const void *sdlEvent);

} // namespace SIM::Settings

// Game hook (weak, none by default): adds the game's pages. Called once, the first time
// the settings open.
void SIM_Prj_SettingsPages();

#endif // LIBNTR_SIM_SETTINGS_HPP

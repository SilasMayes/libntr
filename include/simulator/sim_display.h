#ifndef SIM_DISPLAY_H
#define SIM_DISPLAY_H

// Secondary screen output.
//
// When enabled (SIM_config_type.secondaryScreen), the DS bottom screen is drawn on its own
// output and the main window only shows the top screen:
//   * Android: an android.app.Presentation on a second physical display (e.g. the AYN Thor's
//     lower panel). Java hands its Surface over through JNI and the GL context renders to it
//     through a second EGL window surface.
//   * Desktop: a second SDL window sharing the main GL context.
//
// All functions except the JNI entry points must be called from the render thread.

#include <nitro/types.h>
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once after the main window and GL context exist.
void SIM_Display_Init(SDL_Window *mainWindow, SDL_GLContext context);

// TRUE if a secondary output exists right now and the config asks for it.
BOOL SIM_Display_SecondaryActive(void);

// Makes the secondary output current. Returns FALSE (and leaves the main window current)
// if it went away. On success, *width/*height receive its drawable size in pixels.
BOOL SIM_Display_BeginSecondary(int *width, int *height);

// Presents the secondary output and makes the main window current again.
void SIM_Display_EndSecondary(void);

// Gives the display code a look at SDL events (desktop second window input/close).
// Returns TRUE if the event belonged to the secondary output and must not be handled
// as main-window input.
BOOL SIM_Display_ProcessEvent(const SDL_Event *event);

// Current touch on the secondary output, in pixels of its drawable. Returns TRUE while a
// finger/mouse button is down.
BOOL SIM_Display_GetSecondaryTouch(int *x, int *y);

// Touch events on the secondary output, in order, in pixels of its drawable (top-left
// origin). Quick taps that start and end between two frames are not lost.
enum {
    SIM_TOUCH_DOWN = 0,
    SIM_TOUCH_MOVE = 1,
    SIM_TOUCH_UP = 2,
};
typedef struct SIM_TouchEvent {
    int type;
    int x;
    int y;
} SIM_TouchEvent;

// Where the main window goes when it's created: the saved position if that's still on a
// connected display, else centred. Needs SDL's video subsystem.
void SIM_Display_MainWindowPosition(int width, int height, int *x, int *y);

// Desktop full screen for the top (main) window or the bottom screen's own window. The
// window keeps the display it's on; leaving full screen puts it back where it was. The
// choice is saved in the config (FullScreen / BottomFullScreen). Android: always full screen.
BOOL SIM_Display_IsFullScreen(BOOL bottom);
void SIM_Display_SetFullScreen(BOOL bottom, BOOL on);

// F11 / Alt+Enter: full screen on or off for the window that has the keyboard focus.
void SIM_Display_ToggleFullScreen(void);

// Once per frame: keeps the bottom window above a full-screen top window on the same
// display, and writes moved/resized window positions to the config after a moment.
void SIM_Display_Tick(void);

// Writes the windows' current positions and sizes to the config now (on quit).
void SIM_Display_SavePlacement(void);

// Pops the oldest pending touch event. FALSE when there is none.
BOOL SIM_Display_PollSecondaryTouch(SIM_TouchEvent *event);

#ifdef __cplusplus
}
#endif

#endif

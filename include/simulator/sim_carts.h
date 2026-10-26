#ifndef LIBNTR_SIM_CARTS_H
#define LIBNTR_SIM_CARTS_H

// Several games side by side, like cartridges: the data directory holds the first game
// (its extracted ROM and saves) and the settings shared by all; every other game lives
// in carts/<id>/ with its own extracted ROM and saves. Before the game starts, the chosen
// game's directory becomes the working directory, so the game's files and saves resolve
// there, while settings go through SIM_SettingsPath.
//
// With more than one game a picker comes up at launch (unless turned off); games are
// added from it or from the settings GUI by importing another ROM.

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_CARTS_MAX 8
#define SIM_CART_NAME_LEN 48

typedef struct SIM_Cart {
    char id[32];   // "" for the game in the data directory itself
    char name[SIM_CART_NAME_LEN];
    BOOL hasFiles; // its ROM has been extracted
} SIM_Cart;

// Game hook (weak default "Pokemon"): a display name for the ROM extracted in dir
void SIM_Prj_CartName(const char *dir, char *name, int nameSize);

// Settings file in the data directory, whichever game is running
const char *SIM_SettingsPath(const char *fileName);

int SIM_Carts_List(SIM_Cart carts[SIM_CARTS_MAX]);
const SIM_Cart *SIM_Carts_Current(void);
BOOL SIM_Carts_AskAtLaunch(void);
void SIM_Carts_SetAskAtLaunch(BOOL ask);

// Imports a ROM as a new game (file picker); calls done(TRUE) with the new game's id
// once it is extracted, done(FALSE) if cancelled or the file was not a valid ROM
void SIM_Carts_Add(void (*done)(BOOL ok, const char *id));

// Makes id the game to start and restarts the app (Android) or quits (desktop), letting
// the game save first (SIM_LIFECYCLE_QUIT)
void SIM_Carts_SwitchTo(const char *id);

// libntr internals
void SIM_Carts_Init(void);      // first thing in main(): remembers the data directory
void SIM_Carts_Choose(void);    // once the window exists: picker, then enter the game's directory
BOOL SIM_Carts_PickerActive(void);
void SIM_Carts_DrawPicker(void);

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_CARTS_H

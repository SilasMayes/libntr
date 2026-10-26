#ifndef LIBNTR_SIM_FILES_H
#define LIBNTR_SIM_FILES_H

// Letting the player pick files outside the game's own folder: an ImGui file dialog on
// desktop, the system document picker on Android (where the app can't browse other
// apps' files directly). One request at a time; results are delivered on the game
// thread from SIM_Files_Update(), which libntr's GUI calls every frame.

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// path: a local file the game can read (on Android, a copy in the app's folder that the
// game may delete afterwards), or NULL if the player cancelled
typedef void (*SIM_FileOpenedFn)(const char *path, void *user);
typedef void (*SIM_FileSavedFn)(BOOL saved, void *user);

// filters: comma-separated extensions for the desktop dialog, e.g. ".sav,.dsv"
void SIM_Files_PickToOpen(const char *title, const char *filters, SIM_FileOpenedFn done, void *user);

// Lets the player choose where to save a copy of sourcePath
void SIM_Files_SaveCopy(const char *title, const char *sourcePath, const char *suggestedName,
                        SIM_FileSavedFn done, void *user);

BOOL SIM_Files_IsBusy(void);
void SIM_Files_Update(void);

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_FILES_H

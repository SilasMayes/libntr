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

// Lets the player pick a folder (e.g. a ROM hack's download with its patches). On Android
// its patches (.xdelta .vcdiff .delta) and ROMs (.nds .srl; only those whose game code is
// gameCode unless it is NULL), also from one folder level down, are copied into a folder
// of the app's own, whose path is passed; archives and unsupported patch formats it holds
// are listed in .skipped there. On desktop the folder's own path. NULL if cancelled.
void SIM_Files_PickFolder(const char *title, const char *gameCode, SIM_FileOpenedFn done, void *user);

// Lets the player pick a folder whose files with one of the extensions (".png,.dds"), from
// it and every folder in it, are copied into targetDir/NAME (NAME: the picked folder's),
// keeping its folders. done gets targetDir/NAME, or NULL if the player cancelled or
// nothing could be copied.
void SIM_Files_ImportFolder(const char *title, const char *extensions, const char *targetDir, SIM_FileOpenedFn done,
                            void *user);

// Lets the player choose where to save a copy of sourcePath
void SIM_Files_SaveCopy(const char *title, const char *sourcePath, const char *suggestedName,
                        SIM_FileSavedFn done, void *user);

BOOL SIM_Files_IsBusy(void);
void SIM_Files_Update(void);

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_FILES_H

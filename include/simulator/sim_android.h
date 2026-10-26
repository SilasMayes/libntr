#ifndef SIM_ANDROID_H
#define SIM_ANDROID_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// First thing called from SDL_main: logcat logging, SDL hints, and chdir into the
// app's files directory (where the extracted ROM, saves and configs live).
void SIM_Android_Init(void);

// Opens the system file picker so the player can choose their .nds file. The Java side
// copies it into the files directory and reports back.
void SIM_Android_RequestRomImport(void);

// 0 while the picker is open, 1 when a file was copied (its path is written to pathOut),
// -1 if the player backed out.
int SIM_Android_PollRomImport(char *pathOut, size_t pathOutSize);

// Restarts the app (a fresh process): used to switch to another game
void SIM_Android_RestartApp(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef LIBNTR_SIM_BACKUP_H
#define LIBNTR_SIM_BACKUP_H

// Cartridge backup memory (the DS save chip) on the port: a plain file holding the raw
// flash image, the same layout emulators use for .sav files.
//
// The file defaults to "save.bin" in the working directory. A game can point it at
// another file (save slots) with SIM_Backup_SetPath(); it must reload its save data
// afterwards, since nothing is cached here.

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_BACKUP_DEFAULT_PATH "save.bin"
#define SIM_BACKUP_PATH_MAX 512

void SIM_Backup_SetPath(const char *path);
const char *SIM_Backup_GetPath(void);

// Reads len bytes at offset src. Bytes past the end of the file (or a missing file) read
// as 0x00, like the zero-filled file the port has always created on first access.
BOOL SIM_Backup_Read(u32 src, void *dst, u32 len);

// Writes len bytes at offset dst and flushes them to storage before returning.
BOOL SIM_Backup_Write(u32 dst, const void *src, u32 len);

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_BACKUP_H

// Backup memory (save chip) as a file. See include/simulator/sim_backup.h
#include <simulator/sim_backup.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define SIM_FSYNC(fd) _commit(fd)
#else
#include <unistd.h>
#define SIM_FSYNC(fd) fsync(fd)
#endif

static char sPath[SIM_BACKUP_PATH_MAX] = SIM_BACKUP_DEFAULT_PATH;

void SIM_Backup_SetPath(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        path = SIM_BACKUP_DEFAULT_PATH;
    }
    strncpy(sPath, path, sizeof(sPath) - 1);
    sPath[sizeof(sPath) - 1] = '\0';
}

const char *SIM_Backup_GetPath(void)
{
    return sPath;
}

BOOL SIM_Backup_Read(u32 src, void *dst, u32 len)
{
    memset(dst, 0, len);

    FILE *file = fopen(sPath, "rb");
    if (file == NULL) {
        // A blank chip. The file is created by the first write.
        return TRUE;
    }

    if (fseek(file, (long)src, SEEK_SET) == 0) {
        size_t got = fread(dst, 1, len, file);
        (void)got; // a short read leaves the rest zeroed
    }
    fclose(file);
    return TRUE;
}

BOOL SIM_Backup_Write(u32 dst, const void *src, u32 len)
{
    FILE *file = fopen(sPath, "rb+");
    if (file == NULL) {
        file = fopen(sPath, "wb+");
        if (file == NULL) {
            printf("SIM_Backup_Write: could not open %s\n", sPath);
            return FALSE;
        }
    }

    BOOL ok = FALSE;
    if (fseek(file, (long)dst, SEEK_SET) == 0 && fwrite(src, 1, len, file) == len) {
        // Saves must survive the app being killed or the device losing power right after
        ok = (fflush(file) == 0);
        if (ok) {
            SIM_FSYNC(fileno(file));
        }
    }
    if (fclose(file) != 0) {
        ok = FALSE;
    }
    if (!ok) {
        printf("SIM_Backup_Write: write to %s failed\n", sPath);
    }
    return ok;
}

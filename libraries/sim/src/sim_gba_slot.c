// A GBA cartridge in the GBA slot, from a save file. See include/simulator/sim_gba_slot.h
#include <simulator/sim_gba_slot.h>

#include <nitro.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define SIM_FSYNC(fd) _commit(fd)
#else
#include <unistd.h>
#define SIM_FSYNC(fd) fsync(fd)
#endif

#define GBA_ROM_START 0x08000000u
#define GBA_ROM_END 0x0A000000u // the ROM's 32 MB, then the backup chip
#define GBA_HEADER_SIZE 0xC0
#define GBA_SECTOR_SIZE 0x1000
#define GBA_SECTOR_COUNT (SIM_GBA_FLASH_SIZE / GBA_SECTOR_SIZE)

static BOOL sInserted = FALSE;
static u8 sHeader[GBA_HEADER_SIZE];
static u8 sFlash[SIM_GBA_FLASH_SIZE];
static BOOL sKeepsSave = FALSE;
static char sSavePath[512];
static u32 sGameCode;
static u16 sMakerCode;

static void SetError(char *err, int errSize, const char *text)
{
    if (err != NULL && errSize > 0) {
        snprintf(err, (size_t)errSize, "%s", text);
    }
}

static void CopyField(u8 *dst, const char *src, int size)
{
    memset(dst, 0, (size_t)size);
    for (int i = 0; src != NULL && src[i] != '\0' && i < size; i++) {
        dst[i] = (u8)src[i];
    }
}

// The cartridge header the DS checks: the logo (the DS game's header has the same one at
// 0xC0), title, game code, maker code and the header checksum
static void BuildHeader(const SIM_GBASlotCart *cart)
{
    memset(sHeader, 0, sizeof(sHeader));
    sHeader[0] = 0x2E; // "b" over the header to the entry point, as on a real cartridge
    sHeader[3] = 0xEA;
    memcpy(&sHeader[0x04], (const u8 *)HW_ROM_HEADER_BUF + 0xC0, 0x9C);
    CopyField(&sHeader[0xA0], cart->title, 12);
    CopyField(&sHeader[0xAC], cart->gameCode, 4);
    CopyField(&sHeader[0xB0], cart->makerCode, 2);
    sHeader[0xB2] = 0x96;

    u8 sum = 0;
    for (int i = 0xA0; i <= 0xBC; i++) {
        sum = (u8)(sum + sHeader[i]);
    }
    sHeader[0xBD] = (u8)(-(sum + 0x19));

    sGameCode = (u32)sHeader[0xAC] | (u32)sHeader[0xAD] << 8 | (u32)sHeader[0xAE] << 16 | (u32)sHeader[0xAF] << 24;
    sMakerCode = (u16)(sHeader[0xB0] | sHeader[0xB1] << 8);
}

BOOL SIM_GBASlot_Insert(const SIM_GBASlotCart *cart, char *err, int errSize)
{
    SIM_GBASlot_Eject();
    SetError(err, errSize, "");
    if (cart == NULL || cart->gameCode == NULL || strlen(cart->gameCode) != 4) {
        SetError(err, errSize, "No game chosen.");
        return FALSE;
    }

    memset(sFlash, 0xFF, sizeof(sFlash));
    sKeepsSave = FALSE;
    sSavePath[0] = '\0';
    if (cart->savePath != NULL && cart->savePath[0] != '\0') {
        FILE *file = fopen(cart->savePath, "rb");
        if (file == NULL) {
            SetError(err, errSize, "The save file can't be opened.");
            return FALSE;
        }
        size_t got = fread(sFlash, 1, sizeof(sFlash), file);
        fclose(file);
        if (got != sizeof(sFlash)) {
            if (got == 0x10000) {
                SetError(err, errSize, "That save is 64 KB. Emerald, FireRed and LeafGreen saves are 128 KB: set the "
                                       "emulator's save type to Flash 128K (1 Mbit) and load the game once.");
            } else if (got == 0x8000) {
                SetError(err, errSize, "That save is 32 KB (SRAM). Pokemon GBA games use a 128 KB flash save.");
            } else {
                SetError(err, errSize, "That isn't a 128 KB GBA save.");
            }
            memset(sFlash, 0xFF, sizeof(sFlash));
            return FALSE;
        }
        snprintf(sSavePath, sizeof(sSavePath), "%s", cart->savePath);
        sKeepsSave = TRUE;
    }

    BuildHeader(cart);
    sInserted = TRUE;
    return TRUE;
}

void SIM_GBASlot_Eject(void)
{
    sInserted = FALSE;
    sKeepsSave = FALSE;
    sSavePath[0] = '\0';
}

BOOL SIM_GBASlot_IsInserted(void)
{
    return sInserted;
}

const u8 *SIM_GBASlot_Flash(void)
{
    return sInserted ? sFlash : NULL;
}

u32 SIM_GBASlot_GameCode(void)
{
    return sInserted ? sGameCode : 0;
}

u16 SIM_GBASlot_MakerCode(void)
{
    return sInserted ? sMakerCode : 0;
}

BOOL SIM_GBASlot_ReadRom(const void *address, void *dst, u32 size)
{
    if (!sInserted) {
        return FALSE;
    }
    uintptr_t at = (uintptr_t)address;
    uintptr_t buffer = (uintptr_t)HW_CTRDG_ROM;
    u32 offset;
    if (at >= GBA_ROM_START && at < GBA_ROM_END) {
        offset = (u32)(at - GBA_ROM_START);
    } else if (at >= buffer && at < (uintptr_t)HW_CTRDG_ROM_END) {
        offset = (u32)(at - buffer);
    } else {
        return FALSE;
    }

    u8 *out = (u8 *)dst;
    for (u32 i = 0; i < size; i++) {
        u32 pos = offset + i;
        if (pos < GBA_HEADER_SIZE) {
            out[i] = sHeader[pos];
        } else {
            // Past the header: the GBA's open bus over an absent ROM (each halfword reads as
            // its own address / 2)
            u32 half = (pos >> 1) & 0xFFFF;
            out[i] = (u8)((pos & 1) ? half >> 8 : half);
        }
    }
    return TRUE;
}

BOOL SIM_GBASlot_HasFlash1M(void)
{
    return sInserted;
}

void SIM_GBASlot_ReadFlash(u16 sector, u32 offset, u8 *dst, u32 size)
{
    u32 start = (u32)sector * GBA_SECTOR_SIZE + offset;
    for (u32 i = 0; i < size; i++) {
        dst[i] = start + i < SIM_GBA_FLASH_SIZE ? sFlash[start + i] : 0xFF;
    }
}

// The sectors from first, count of them, into the save file
static BOOL Persist(u32 first, u32 count)
{
    if (!sKeepsSave) {
        return TRUE;
    }
    FILE *file = fopen(sSavePath, "rb+");
    if (file == NULL) {
        printf("GBA slot: can't open %s to save\n", sSavePath);
        return FALSE;
    }
    BOOL ok = FALSE;
    u32 start = first * GBA_SECTOR_SIZE;
    u32 size = count * GBA_SECTOR_SIZE;
    if (fseek(file, (long)start, SEEK_SET) == 0 && fwrite(&sFlash[start], 1, size, file) == size) {
        ok = fflush(file) == 0;
        if (ok) {
            SIM_FSYNC(fileno(file));
        }
    }
    if (fclose(file) != 0) {
        ok = FALSE;
    }
    if (!ok) {
        printf("GBA slot: writing %s failed\n", sSavePath);
    }
    return ok;
}

u32 SIM_GBASlot_WriteFlashSector(u16 sector, const u8 *src)
{
    if (!sInserted) {
        return CTRDG_BACKUP_RESULT_PULLEDOUT;
    }
    if (sector >= GBA_SECTOR_COUNT) {
        return CTRDG_BACKUP_RESULT_ERROR;
    }
    memcpy(&sFlash[(u32)sector * GBA_SECTOR_SIZE], src, GBA_SECTOR_SIZE);
    return Persist(sector, 1) ? 0 : CTRDG_BACKUP_RESULT_ERROR;
}

u32 SIM_GBASlot_EraseFlash(int sector)
{
    if (!sInserted) {
        return CTRDG_BACKUP_RESULT_PULLEDOUT;
    }
    if (sector < 0) {
        memset(sFlash, 0xFF, sizeof(sFlash));
        return Persist(0, GBA_SECTOR_COUNT) ? 0 : CTRDG_BACKUP_RESULT_ERROR;
    }
    if (sector >= GBA_SECTOR_COUNT) {
        return CTRDG_BACKUP_RESULT_ERROR;
    }
    memset(&sFlash[(u32)sector * GBA_SECTOR_SIZE], 0xFF, GBA_SECTOR_SIZE);
    return Persist((u32)sector, 1) ? 0 : CTRDG_BACKUP_RESULT_ERROR;
}

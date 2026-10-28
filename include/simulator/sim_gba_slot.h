#ifndef LIBNTR_SIM_GBA_SLOT_H
#define LIBNTR_SIM_GBA_SLOT_H

// A GBA cartridge in the DS's GBA slot, without the cartridge: the game reads a ROM header
// (title, game code, maker code, the logo from the DS game's own header) and a 128 KB flash
// save, which is a save file from an emulator. libntr's CTRDG library answers from here
// while a cartridge is "inserted"; with none, it behaves as before (an empty slot).
//
// Writes the DS game makes to the cartridge's save go to the save file straight away (it is
// replaced as a whole, so it is never left half-written).

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_GBA_FLASH_SIZE 0x20000 // 1 Mbit flash: 32 sectors of 4 KB

typedef struct SIM_GBASlotCart {
    const char *title;     // the ROM header's title, up to 12 characters ("POKEMON EMER")
    const char *gameCode;  // 4 characters ("BPEE")
    const char *makerCode; // 2 characters ("01")
    // Its flash save: a .sav of at least 128 KB, as GBA emulators write it (anything after
    // the first 128 KB, such as an emulator's clock data, is kept as it is). NULL: a blank
    // flash chip that isn't kept anywhere.
    const char *savePath;
} SIM_GBASlotCart;

// FALSE (with a reason in err) if the save can't be used; the slot is then empty
BOOL SIM_GBASlot_Insert(const SIM_GBASlotCart *cart, char *err, int errSize);
void SIM_GBASlot_Eject(void);
BOOL SIM_GBASlot_IsInserted(void);
// The save as the game sees it now (NULL when none is inserted)
const u8 *SIM_GBASlot_Flash(void);

// For libntr's CTRDG library (SDK_PORT)
u32 SIM_GBASlot_GameCode(void);
u16 SIM_GBASlot_MakerCode(void);
// address: where the DS sees the GBA ROM (0x08000000 up, or libntr's HW_CTRDG_ROM buffer).
// FALSE if it isn't there.
BOOL SIM_GBASlot_ReadRom(const void *address, void *dst, u32 size);
BOOL SIM_GBASlot_HasFlash1M(void);
void SIM_GBASlot_ReadFlash(u16 sector, u32 offset, u8 *dst, u32 size);
// 0 on success (CTRDG's result codes otherwise)
u32 SIM_GBASlot_WriteFlashSector(u16 sector, const u8 *src);
u32 SIM_GBASlot_EraseFlash(int sector); // -1: the whole chip

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_GBA_SLOT_H

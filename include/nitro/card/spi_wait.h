#ifndef NITRO_CARD_SPI_WAIT_H_
#define NITRO_CARD_SPI_WAIT_H_

// Byte-wise transfers on the card's SPI bus with a wait before every byte: how HeartGold and
// SoulSilver talk to the infrared chip on their cards (the Pokéwalker). Only those games' SDK
// had these; nitro.h declares just CARD_SpiWaitGetStatus (in card/backup.h), since the games
// declare the others themselves.
//
// SDK_PORT: there is no SPI bus. The IR chip is simulated (simulator/sim_ir.h): it answers its
// ID and, until a device is attached, nothing is ever in range.

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void CARD_SpiWaitInit(void);
// Waits before each byte, in microseconds (CARD_SpiWaitInit sets 50)
void CARD_SetSpiReadWaitCycles(u32 cycles);
void CARD_SetSpiWriteWaitCycles(u32 cycles);
// Reads a block whose first byte is its length, undoes the XOR 0xAA of its bytes, returns the
// length (at most 185; 0 when nothing was received)
int CARD_SpiWaitReadRange(u8 *buffer);
void CARD_SpiWaitWriteRange(const void *data, int size);
// The IR chip's status byte: 0xAA when it is there
int CARD_SpiWaitGetStatus(void);

#ifdef __cplusplus
}
#endif

#endif

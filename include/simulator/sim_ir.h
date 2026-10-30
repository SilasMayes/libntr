#ifndef LIBNTR_SIM_IR_H
#define LIBNTR_SIM_IR_H

// The infrared chip on HeartGold/SoulSilver cards (the Pokéwalker link), which the game reaches
// through CARD_SpiWait* (libntr's CARD library, SDK_PORT). With no device attached the chip is
// there but nothing is in range: it answers its ID (0xAA) and never receives anything, so the
// game behaves as a DS with no Pokéwalker pointed at it ("could not connect").
//
// A device (a virtual Pokéwalker) sees the bytes as the IR chip moves them: what the game sends,
// and what it reads back, are wire bytes (the Pokéwalker protocol's XOR 0xAA encoding is applied
// by the game on the way out and undone by CARD_SpiWaitReadRange on the way in, as on the DS).

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_IR_CHIP_ID      0xAA // the status byte the card's IR chip answers with
#define SIM_IR_PACKET_MAX   185  // the most bytes one CARD_SpiWaitReadRange returns

typedef struct SIM_IrDevice {
    void *user;
    // A packet the game sends (wire bytes).
    void (*send)(void *user, const u8 *data, int size);
    // A packet received since the last call: up to max wire bytes into data, returns how many
    // (0: nothing). Called from the game's IR thread every few milliseconds while it listens.
    int (*receive)(void *user, u8 *data, int max);
} SIM_IrDevice;

// Attaches a device (it must stay valid until replaced); NULL detaches it. Any thread.
void SIM_Ir_SetDevice(const SIM_IrDevice *device);
BOOL SIM_Ir_HasDevice(void);

// For libntr's CARD library
void SIM_Ir_Send(const u8 *data, int size);
int SIM_Ir_Receive(u8 *data, int max);

#ifdef __cplusplus
}
#endif

#endif

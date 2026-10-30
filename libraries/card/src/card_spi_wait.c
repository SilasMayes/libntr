// CARD_SpiWait*: the card IR chip of HeartGold/SoulSilver (the Pokéwalker link), simulated.
// See include/nitro/card/spi_wait.h and include/simulator/sim_ir.h.
//
// On the DS these clock bytes through the card's SPI bus with a busy-wait before each one. Here
// the chip always answers its ID, so the games' checks for a genuine card pass (HeartGold's
// field map starts its anti-piracy tasks when CARD_SpiWaitGetStatus isn't 0xAA), and packets go
// to and come from the attached IR device, if any. Without one nothing is in range: reads come
// back empty and the game's own Pokéwalker screens time out with "could not connect".

#include <nitro.h>
#include <nitro/card/spi_wait.h>
#include <simulator/sim_ir.h>

// The simulated chip needs no lock and no waits between bytes.
void CARD_SpiWaitInit(void)
{
}

void CARD_SetSpiReadWaitCycles(u32 cycles)
{
    (void)cycles;
}

void CARD_SetSpiWriteWaitCycles(u32 cycles)
{
    (void)cycles;
}

int CARD_SpiWaitReadRange(u8 *buffer)
{
    int size;
    int i;

    if (buffer == NULL) {
        return 0;
    }
    buffer[0] = 0;
    size = SIM_Ir_Receive(buffer, SIM_IR_PACKET_MAX);
    for (i = 0; i < size; i++) {
        buffer[i] ^= 0xAA;
    }
    return size;
}

void CARD_SpiWaitWriteRange(const void *data, int size)
{
    SIM_Ir_Send((const u8 *)data, size);
}

int CARD_SpiWaitGetStatus(void)
{
    return SIM_IR_CHIP_ID;
}

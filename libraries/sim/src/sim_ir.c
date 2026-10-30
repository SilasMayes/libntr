// The card's infrared chip and what is in range of it. See include/simulator/sim_ir.h
#include <simulator/sim_ir.h>

#include <SDL2/SDL_atomic.h>

// Held only around reading the device pointer and calling it, so a device can be swapped (or
// detached) while the game's IR thread is polling.
static SDL_SpinLock sLock = 0;
static const SIM_IrDevice *sDevice = NULL;

void SIM_Ir_SetDevice(const SIM_IrDevice *device)
{
    SDL_AtomicLock(&sLock);
    sDevice = device;
    SDL_AtomicUnlock(&sLock);
}

BOOL SIM_Ir_HasDevice(void)
{
    SDL_AtomicLock(&sLock);
    const BOOL has = sDevice != NULL;
    SDL_AtomicUnlock(&sLock);
    return has;
}

void SIM_Ir_Send(const u8 *data, int size)
{
    SDL_AtomicLock(&sLock);
    if (sDevice != NULL && sDevice->send != NULL && data != NULL && size > 0) {
        sDevice->send(sDevice->user, data, size);
    }
    SDL_AtomicUnlock(&sLock);
}

int SIM_Ir_Receive(u8 *data, int max)
{
    int got = 0;
    SDL_AtomicLock(&sLock);
    if (sDevice != NULL && sDevice->receive != NULL && data != NULL && max > 0) {
        got = sDevice->receive(sDevice->user, data, max);
    }
    SDL_AtomicUnlock(&sLock);
    if (got < 0) {
        return 0;
    }
    return got > max ? max : got;
}

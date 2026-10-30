// The BIOS calls the ports need that libsyscall (DS only) would otherwise provide.
#include <nitro.h>

#ifdef SDK_PORT

// SWI 0x0E: CRC-16 with the reflected polynomial 0xA001 from start, over size bytes (the BIOS
// reads halfwords, low byte first: the same result for the even sizes it takes).
u16 SVC_GetCRC16(u32 start, const void *datap, u32 size)
{
    const u8 *data = (const u8 *)datap;
    u32 crc = start & 0xFFFF;
    u32 i;
    int bit;

    for (i = 0; i < size; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return (u16)crc;
}

#endif

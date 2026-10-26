// VCDIFF (RFC 3284) decoder for ROM hack patches. See include/simulator/sim_vcdiff.h
#include <simulator/sim_vcdiff.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    VCD_SECONDARY = 1 << 0,
    VCD_CODETABLE = 1 << 1,
    VCD_APPHEADER = 1 << 2,

    VCD_SOURCE = 1 << 0,
    VCD_TARGET = 1 << 1,
    VCD_ADLER32 = 1 << 2, // xdelta3's extension: Adler-32 of the target window
};

enum { INST_NOOP, INST_ADD, INST_RUN, INST_COPY };

typedef struct Instruction {
    uint8_t type, size, mode;
} Instruction;

typedef struct CodeEntry {
    Instruction inst[2];
} CodeEntry;

#define NEAR_SLOTS 4
#define SAME_SLOTS 3

static CodeEntry sCodeTable[256];

// The default code table (RFC 3284, 5.6)
static void BuildCodeTable(void)
{
    if (sCodeTable[1].inst[0].type == INST_ADD) {
        return;
    }
    int i = 0;
    sCodeTable[i++].inst[0] = (Instruction) { INST_RUN, 0, 0 };
    for (int size = 0; size <= 17; size++) {
        sCodeTable[i++].inst[0] = (Instruction) { INST_ADD, (uint8_t)size, 0 };
    }
    for (int mode = 0; mode <= 8; mode++) {
        sCodeTable[i++].inst[0] = (Instruction) { INST_COPY, 0, (uint8_t)mode };
        for (int size = 4; size <= 18; size++) {
            sCodeTable[i++].inst[0] = (Instruction) { INST_COPY, (uint8_t)size, (uint8_t)mode };
        }
    }
    for (int mode = 0; mode <= 5; mode++) {
        for (int add = 1; add <= 4; add++) {
            for (int copy = 4; copy <= 6; copy++) {
                sCodeTable[i].inst[0] = (Instruction) { INST_ADD, (uint8_t)add, 0 };
                sCodeTable[i++].inst[1] = (Instruction) { INST_COPY, (uint8_t)copy, (uint8_t)mode };
            }
        }
    }
    for (int mode = 6; mode <= 8; mode++) {
        for (int add = 1; add <= 4; add++) {
            sCodeTable[i].inst[0] = (Instruction) { INST_ADD, (uint8_t)add, 0 };
            sCodeTable[i++].inst[1] = (Instruction) { INST_COPY, 4, (uint8_t)mode };
        }
    }
    for (int mode = 0; mode <= 8; mode++) {
        sCodeTable[i].inst[0] = (Instruction) { INST_COPY, 4, (uint8_t)mode };
        sCodeTable[i++].inst[1] = (Instruction) { INST_ADD, 1, 0 };
    }
}

typedef struct Reader {
    const uint8_t *pos;
    const uint8_t *end;
    int failed;
} Reader;

static uint8_t ReadByte(Reader *r)
{
    if (r->pos >= r->end) {
        r->failed = 1;
        return 0;
    }
    return *r->pos++;
}

// Base-128, most significant group first
static uint64_t ReadInt(Reader *r)
{
    uint64_t value = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t byte = ReadByte(r);
        value = (value << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    r->failed = 1;
    return 0;
}

static uint32_t Adler32(const uint8_t *data, size_t size)
{
    uint32_t a = 1, b = 0;
    while (size > 0) {
        size_t chunk = size < 5552 ? size : 5552;
        size -= chunk;
        while (chunk-- > 0) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

static void Fail(SIM_VCDiffResult *result, uint8_t *output, const char *message)
{
    free(output);
    result->data = NULL;
    result->size = 0;
    snprintf(result->error, sizeof(result->error), "%s", message);
}

void SIM_VCDiff_Apply(const uint8_t *source, size_t sourceSize, const uint8_t *patch, size_t patchSize,
                      SIM_VCDiffResult *result)
{
    memset(result, 0, sizeof(*result));
    BuildCodeTable();

    Reader r = { patch, patch + patchSize, 0 };
    if (patchSize < 5 || patch[0] != 0xD6 || patch[1] != 0xC3 || patch[2] != 0xC4 || patch[3] != 0x00) {
        Fail(result, NULL, "Not an xdelta/VCDIFF patch.");
        return;
    }
    r.pos += 4;
    uint8_t header = ReadByte(&r);
    if (header & VCD_SECONDARY) {
        Fail(result, NULL, "The patch uses xdelta's extra compression, which isn't supported. "
                           "Make it without -S, or apply it with xdelta/Delta Patcher.");
        return;
    }
    if (header & VCD_CODETABLE) {
        Fail(result, NULL, "The patch uses a custom code table, which isn't supported.");
        return;
    }
    if (header & VCD_APPHEADER) {
        uint64_t length = ReadInt(&r);
        if (r.failed || length > (uint64_t)(r.end - r.pos)) {
            Fail(result, NULL, "The patch is damaged (header).");
            return;
        }
        r.pos += length;
    }

    size_t capacity = sourceSize > 0 ? sourceSize : 1 << 20;
    uint8_t *output = malloc(capacity);
    size_t outputSize = 0;
    if (output == NULL) {
        Fail(result, NULL, "Not enough memory.");
        return;
    }

    while (r.pos < r.end) {
        uint8_t window = ReadByte(&r);
        uint64_t segmentLength = 0, segmentOffset = 0;
        const uint8_t *segment = NULL;

        if (window & ~7) {
            Fail(result, output, "The patch is damaged (window).");
            return;
        }
        if (window & (VCD_SOURCE | VCD_TARGET)) {
            segmentLength = ReadInt(&r);
            segmentOffset = ReadInt(&r);
        }
        ReadInt(&r); // length of the delta encoding
        uint64_t targetLength = ReadInt(&r);
        uint8_t deltaIndicator = ReadByte(&r);
        uint64_t dataLength = ReadInt(&r);
        uint64_t instLength = ReadInt(&r);
        uint64_t addrLength = ReadInt(&r);
        uint32_t checksum = 0;
        if (window & VCD_ADLER32) {
            for (int i = 0; i < 4; i++) {
                checksum = (checksum << 8) | ReadByte(&r);
            }
        }
        if (r.failed || deltaIndicator != 0 || dataLength + instLength + addrLength > (uint64_t)(r.end - r.pos)
            || targetLength > (1u << 30)) {
            Fail(result, output, "The patch is damaged (window header).");
            return;
        }

        if (window & VCD_SOURCE) {
            if (segmentOffset + segmentLength > sourceSize) {
                Fail(result, output, "The patch doesn't fit this file (it needs a bigger file). Is it the right ROM?");
                return;
            }
            segment = source + segmentOffset;
        } else if (window & VCD_TARGET) {
            if (segmentOffset + segmentLength > outputSize) {
                Fail(result, output, "The patch is damaged (target copy).");
                return;
            }
            segment = output + segmentOffset;
        }

        if (outputSize + targetLength > capacity) {
            while (outputSize + targetLength > capacity) {
                capacity *= 2;
            }
            uint8_t *grown = realloc(output, capacity);
            if (grown == NULL) {
                Fail(result, output, "Not enough memory.");
                return;
            }
            output = grown;
            if (window & VCD_TARGET) {
                segment = output + segmentOffset;
            }
        }

        Reader data = { r.pos, r.pos + dataLength, 0 };
        Reader inst = { data.end, data.end + instLength, 0 };
        Reader addr = { inst.end, inst.end + addrLength, 0 };
        r.pos = addr.end;

        uint8_t *target = output + outputSize;
        uint64_t produced = 0;
        uint64_t nearCache[NEAR_SLOTS] = { 0 };
        uint64_t sameCache[SAME_SLOTS * 256] = { 0 };
        int nextNear = 0;

        while (inst.pos < inst.end && !inst.failed) {
            const CodeEntry *entry = &sCodeTable[ReadByte(&inst)];
            for (int half = 0; half < 2; half++) {
                const Instruction *in = &entry->inst[half];
                if (in->type == INST_NOOP) {
                    continue;
                }
                uint64_t size = in->size != 0 ? in->size : ReadInt(&inst);
                if (inst.failed || produced + size > targetLength) {
                    Fail(result, output, "The patch is damaged (instruction).");
                    return;
                }

                if (in->type == INST_ADD) {
                    if (size > (uint64_t)(data.end - data.pos)) {
                        Fail(result, output, "The patch is damaged (data).");
                        return;
                    }
                    memcpy(target + produced, data.pos, size);
                    data.pos += size;
                } else if (in->type == INST_RUN) {
                    memset(target + produced, ReadByte(&data), size);
                } else {
                    uint64_t here = segmentLength + produced;
                    uint64_t address;
                    if (in->mode == 0) {
                        address = ReadInt(&addr);
                    } else if (in->mode == 1) {
                        address = here - ReadInt(&addr);
                    } else if (in->mode < 2 + NEAR_SLOTS) {
                        address = nearCache[in->mode - 2] + ReadInt(&addr);
                    } else {
                        address = sameCache[(in->mode - 2 - NEAR_SLOTS) * 256 + ReadByte(&addr)];
                    }
                    nearCache[nextNear] = address;
                    nextNear = (nextNear + 1) % NEAR_SLOTS;
                    sameCache[address % (SAME_SLOTS * 256)] = address;

                    if (addr.failed || address >= here) {
                        Fail(result, output, "The patch is damaged (address).");
                        return;
                    }
                    // Byte by byte: a copy may overlap what it is producing
                    for (uint64_t i = 0; i < size; i++, address++) {
                        target[produced + i] = address < segmentLength ? segment[address] : target[address - segmentLength];
                    }
                }
                produced += size;
            }
        }

        if (inst.failed || data.failed || produced != targetLength) {
            Fail(result, output, "The patch is damaged (short window).");
            return;
        }
        if ((window & VCD_ADLER32) && Adler32(target, targetLength) != checksum) {
            result->checksumMismatches++;
        }
        outputSize += targetLength;
    }

    result->data = output;
    result->size = outputSize;
}

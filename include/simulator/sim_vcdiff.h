#ifndef LIBNTR_SIM_VCDIFF_H
#define LIBNTR_SIM_VCDIFF_H

// Applies VCDIFF patches (RFC 3284) as made by xdelta3 and Delta Patcher, the usual
// format for ROM hacks (.xdelta). Secondary compression (xdelta3 -S) and custom code
// tables are not supported; ROM hack patches use neither.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SIM_VCDiffResult {
    uint8_t *data;          // the patched file (malloc'd; the caller frees it)
    size_t size;
    int checksumMismatches; // target windows whose Adler-32 didn't match
    char error[160];        // set when the patch could not be applied (data is NULL)
} SIM_VCDiffResult;

void SIM_VCDiff_Apply(const uint8_t *source, size_t sourceSize, const uint8_t *patch, size_t patchSize,
                      SIM_VCDiffResult *result);

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_VCDIFF_H

#ifndef SIM_G2_H
#define SIM_G2_H
#include <nitro/types.h>
void G2SIM_DrawBG( u8 bgNum, u8 bgMode, u8 bg03D, u8 isSub );
void G2SIM_DrawOBJ( u32 line, u8 isSub, int bgnum );
void G2SIM_PlotPixel(u8 * screenBuf, u8 r, u8 g, u8 b, u32 x, u32 y, BOOL isSub);
// Widescreen: a main-engine text-mode BG layer drawn width DS pixels wide (centred on the
// screen) into its own texture, bound to the layer's texture unit. false: not text mode
bool G2SIM_DrawBGWide( u8 bgNum, u8 bgMode, int width );
#endif
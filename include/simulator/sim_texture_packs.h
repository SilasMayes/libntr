#ifndef LIBNTR_SIM_TEXTURE_PACKS_H
#define LIBNTR_SIM_TEXTURE_PACKS_H

// Texture dumping and replacement packs for the 3D scene, as Dolphin does them: every 3D
// texture the game uses is named by a hash of its data and palette, it can be saved as a
// PNG under that name, and a PNG with that name in a pack replaces it, at any size (HD
// textures, fixes, recolours).
//
//   textures/dump/GAMECODE/   dumped textures (Settings > Display > Save the game's textures)
//   textures/load/GAMECODE/   packs: PNGs anywhere under it, in folders of any names
//
// both in the folder with the app's settings (SIM_SettingsPath). A name looks like
// tex_64x32_1a2b3c4d_5e6f7a8b_3.png: the DS texture's size, the hashes of its texture
// data and of the palette colours it uses, and its format (1 A3I5, 2 4-colour, 3 16-colour,
// 4 256-colour, 6 A5I3, 7 direct colour; "t" after it when colour 0 is transparent).
//
// Only the 3D engine's textures: the overworld, NPCs and battle scenes. Menus and text
// are the 2D engines'.

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Rescans the packs (and redoes the textures in use) at the end of the frame
void SIM_TexturePacks_Reload(void);
int SIM_TexturePacks_Count(void);         // textures the packs have
int SIM_TexturePacks_InUseCount(void);    // of them, loaded so far
int SIM_TexturePacks_DumpedCount(void);   // saved this session
const char *SIM_TexturePacks_LoadDir(void);
const char *SIM_TexturePacks_DumpDir(void);

#ifdef __cplusplus
}

#include <memory>
#include <string>

#include "simulator/sim_g3_Texture.hpp"

namespace SIM::TexturePacks {

bool Replacing(); // packs are on
bool Dumping();
// The file name (without .png) of a DS texture
std::string Name(int format, int width, int height, u32 textureHash, const u8 *palette, bool color0Transparent);
// A pack's texture for that name, or nullptr
std::shared_ptr<G3::Texture> Replacement(const std::string &name);
// Saves a decoded texture (RGBA8, rows top to bottom) as name.png, once
void Dump(const std::string &name, const u8 *rgba, int width, int height);
// Changes since the last call: the texture cache must be emptied (outside drawing)
bool TakeChanged();

} // namespace SIM::TexturePacks

#endif

#endif // LIBNTR_SIM_TEXTURE_PACKS_H

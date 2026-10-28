// Texture dumping and replacement packs. See include/simulator/sim_texture_packs.h
#include "simulator/sim_texture_packs.h"

#include <nitro.h>
#include <nitro/gx/g3.h>

#include "simulator/config/sim_config.h"
#include "simulator/sim.h"
#include "simulator/sim_carts.h"
#include "simulator/sim_crc32.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_FAILURE_STRINGS
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;

namespace {

bool sScanned = false;
bool sChanged = false;
std::unordered_map<std::string, std::string> sPackFiles; // name (lower case) -> path
std::unordered_map<std::string, std::shared_ptr<SIM::G3::Texture>> sLoaded; // nullptr: failed
std::unordered_set<std::string> sDumped;
int sDumpedCount = 0;
bool sDumpDirMade = false;
std::string sLoadDir;
std::string sDumpDir;

std::string GameCode() {
  char code[5];
  memcpy(code, (const char *)HW_ROM_HEADER_BUF + 0x0C, 4);
  code[4] = '\0';
  for (int i = 0; i < 4; i++) {
    if (!isalnum((unsigned char)code[i])) {
      code[i] = '_';
    }
  }
  return code;
}

void Dirs() {
  if (sLoadDir.empty()) {
    sLoadDir = std::string(SIM_SettingsPath("textures/load/")) + GameCode();
    sDumpDir = std::string(SIM_SettingsPath("textures/dump/")) + GameCode();
  }
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)tolower(c); });
  return text;
}

void Scan() {
  sScanned = true;
  sPackFiles.clear();
  sLoaded.clear();
  Dirs();
  std::error_code error;
  if (!fs::is_directory(sLoadDir, error)) {
    return;
  }
  for (fs::recursive_directory_iterator it(sLoadDir, fs::directory_options::follow_directory_symlink, error), end;
       !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error)) {
      continue;
    }
    const fs::path &path = it->path();
    if (Lower(path.extension().string()) != ".png") {
      continue;
    }
    // A pack's later file wins over an earlier one with the same name
    sPackFiles[Lower(path.stem().string())] = path.string();
  }
  printf("Texture packs: %d textures in %s\n", (int)sPackFiles.size(), sLoadDir.c_str());
}

} // namespace

namespace SIM::TexturePacks {

bool Replacing() {
  return SIM_GetConfigPtr()->texturePacks != 0;
}

bool Dumping() {
  return SIM_GetConfigPtr()->dumpTextures != 0;
}

std::string Name(int format, int width, int height, u32 textureHash, const u8 *palette, bool color0Transparent) {
  int colors = 0;
  switch (format) {
  case GX_TEXFMT_PLTT4:
    colors = 4;
    break;
  case GX_TEXFMT_PLTT16:
    colors = 16;
    break;
  case GX_TEXFMT_PLTT256:
    colors = 256;
    break;
  case GX_TEXFMT_A3I5:
    colors = 32;
    break;
  case GX_TEXFMT_A5I3:
    colors = 8;
    break;
  default:
    break;
  }
  u32 paletteHash = colors > 0 && palette != nullptr ? SIM_crc32buf((u8 *)palette, (size_t)colors * 2) : 0;
  const bool paletted = format == GX_TEXFMT_PLTT4 || format == GX_TEXFMT_PLTT16 || format == GX_TEXFMT_PLTT256;
  char name[64];
  snprintf(name, sizeof(name), "tex_%dx%d_%08x_%08x_%d%s", width, height, (unsigned)textureHash, (unsigned)paletteHash,
           format, paletted && color0Transparent ? "t" : "");
  return name;
}

std::shared_ptr<G3::Texture> Replacement(const std::string &name) {
  if (!sScanned) {
    Scan();
  }
  if (sPackFiles.empty()) {
    return nullptr;
  }
  auto loaded = sLoaded.find(name);
  if (loaded != sLoaded.end()) {
    return loaded->second;
  }
  auto file = sPackFiles.find(name);
  if (file == sPackFiles.end()) {
    return nullptr;
  }
  int width = 0, height = 0, channels = 0;
  unsigned char *pixels = stbi_load(file->second.c_str(), &width, &height, &channels, 4);
  std::shared_ptr<G3::Texture> texture;
  if (pixels != nullptr && width > 0 && height > 0 && width <= 8192 && height <= 8192) {
    texture = std::make_shared<G3::Texture>((u16)height, (u16)width, pixels);
  } else {
    printf("Texture packs: can't read %s\n", file->second.c_str());
  }
  stbi_image_free(pixels);
  sLoaded[name] = texture;
  return texture;
}

void Dump(const std::string &name, const u8 *rgba, int width, int height) {
  if (!sDumped.insert(name).second) {
    return;
  }
  Dirs();
  std::error_code error;
  if (!sDumpDirMade) {
    fs::create_directories(sDumpDir, error);
    sDumpDirMade = true;
  }
  std::string path = sDumpDir + "/" + name + ".png";
  if (fs::exists(path, error)) {
    return;
  }
  if (stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4)) {
    sDumpedCount++;
  }
}

bool TakeChanged() {
  bool changed = sChanged;
  sChanged = false;
  return changed;
}

} // namespace SIM::TexturePacks

extern "C" void SIM_TexturePacks_Reload(void) {
  sScanned = false;
  sPackFiles.clear();
  sLoaded.clear();
  sDumped.clear();
  sChanged = true;
}

extern "C" int SIM_TexturePacks_Count(void) {
  if (!sScanned) {
    Scan();
  }
  return (int)sPackFiles.size();
}

extern "C" int SIM_TexturePacks_InUseCount(void) {
  int count = 0;
  for (const auto &entry : sLoaded) {
    count += entry.second != nullptr ? 1 : 0;
  }
  return count;
}

extern "C" int SIM_TexturePacks_DumpedCount(void) {
  return sDumpedCount;
}

extern "C" const char *SIM_TexturePacks_LoadDir(void) {
  Dirs();
  return sLoadDir.c_str();
}

extern "C" const char *SIM_TexturePacks_DumpDir(void) {
  Dirs();
  return sDumpDir.c_str();
}

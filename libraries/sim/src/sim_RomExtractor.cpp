#include "simulator/sim.h"
#include "simulator/imgui/imgui.hpp"
#include "simulator/sim_RomExtractor.h"
#include "simulator/sim_gui.hpp"
#include "nds-extract.hpp"

#include <SDL2/SDL.h>
#include <string>

#ifdef SDK_BUILD_ANDROID
#include "simulator/sim_android.h"

#include <cstdio>
#include <cstring>

// Checks the 4-letter game code in the ROM header (offset 0x0C) against the one this
// port was built for, so a wrong file gets a clear message instead of a crash later.
static bool RomMatchesGameCode(const char *path) {
  CARDRomHeader *expected = (CARDRomHeader *)(HW_ROM_HEADER_BUF);
  char expectedCode[5] = {0};
  memcpy(expectedCode, &expected->game_code, 4);
  if (expectedCode[0] == '\0') {
    return true; // nothing to compare against
  }

  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  char code[5] = {0};
  bool ok = fseek(file, 0x0C, SEEK_SET) == 0 && fread(code, 1, 4, file) == 4;
  fclose(file);
  return ok && memcmp(code, expectedCode, 4) == 0;
}

void SIM_ShowRomExtractionDialog() {
  for (;;) {
    SIM_Android_RequestRomImport();

    char path[1024] = {0};
    int result;
    while ((result = SIM_Android_PollRomImport(path, sizeof(path))) == 0) {
      // Keep the window alive while the picker is up
      SIM_Render(nullptr);
    }

    if (result > 0) {
      bool valid = RomMatchesGameCode(path);
      int extractResult = valid ? SIM_NDSExtract(path) : -1;
      remove(path); // the extracted files are all we keep
      if (valid && extractResult == 0) {
        return;
      }
      SDL_ShowSimpleMessageBox(
          SDL_MESSAGEBOX_ERROR, "Wrong file",
          valid ? "That ROM could not be read. Please choose your own dump of "
                  "Pokemon Platinum (USA)."
                : "That file is not a Pokemon Platinum (USA) ROM. Please choose "
                  "your own dump of the game.",
          nullptr);
    } else {
      SDL_ShowSimpleMessageBox(
          SDL_MESSAGEBOX_INFORMATION, "ROM required",
          "The game files come from your own Pokemon Platinum (USA) ROM. "
          "Choose the .nds file to continue.",
          nullptr);
    }
  }
}

#else

void SIM_ShowRomExtractionDialog() {
    bool fileSelected = false;
    SIM::GUI::Enable();
    SIM::GUI::OpenFileDialog([&fileSelected](std::string fileName) {
        SIM_NDSExtract(fileName);
        fileSelected = true;
    });
    while(!fileSelected) {
        SIM_Render(nullptr);
    }


    int foo = 1;
    foo++;
    printf("%d", foo);
}

#endif

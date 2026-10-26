// Games side by side ("carts"). See include/simulator/sim_carts.h
#include <simulator/sim.h>
#include <simulator/sim_carts.h>
#include <simulator/sim_gui.hpp>
#include <simulator/sim_lifecycle.h>
#include <simulator/simvariables.h>

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "nds-extract.hpp"
#include "simulator/imgui/imgui.hpp"

#ifdef SDK_BUILD_ANDROID
#include <simulator/sim_android.h>
#endif

namespace fs = std::filesystem;

extern "C" void __attribute__((weak)) SIM_Prj_CartName(const char *dir, char *name, int nameSize) {
  (void)dir;
  snprintf(name, nameSize, "Pokemon");
}

namespace {

std::string sBaseDir;           // the data directory (settings, first game)
std::string sCurrentId;         // game to start: "" for the data directory itself
bool sAskAtLaunch = true;
bool sPickerActive = false;
int sSelection = 0;
u16 sPreviousKeys = 0x3FF;
SIM_Cart sCurrent;

// Adding a game
enum AddState { ADD_IDLE, ADD_PICKING, ADD_EXTRACTING };
AddState sAddState = ADD_IDLE;
void (*sAddDone)(BOOL ok, const char *id) = nullptr;
std::string sAddPath;
std::string sAddMessage;

constexpr const char *kStateFile = "carts.ini";
constexpr const char *kNameFile = "cart.name";

std::string CartDir(const std::string &id) {
  return id.empty() ? sBaseDir : sBaseDir + "/carts/" + id;
}

void LoadState() {
  FILE *file = fopen(SIM_SettingsPath(kStateFile), "r");
  if (file == nullptr) {
    return;
  }
  char line[256];
  while (fgets(line, sizeof(line), file) != nullptr) {
    line[strcspn(line, "\r\n")] = '\0';
    if (strncmp(line, "Current=", 8) == 0) {
      sCurrentId = line + 8;
    } else if (strncmp(line, "AskAtLaunch=", 12) == 0) {
      sAskAtLaunch = atoi(line + 12) != 0;
    }
  }
  fclose(file);
}

void SaveState() {
  FILE *file = fopen(SIM_SettingsPath(kStateFile), "w");
  if (file == nullptr) {
    return;
  }
  fprintf(file, "[Carts]\nCurrent=%s\nAskAtLaunch=%d\n", sCurrentId.c_str(), sAskAtLaunch ? 1 : 0);
  fclose(file);
}

// A game's ROM has been extracted there (desktop builds may run from the build output,
// which has the game files and header.bin but no arm9.bin)
bool HasFiles(const std::string &dir) {
  std::error_code error;
  return fs::exists(dir + "/header.bin", error);
}

void ReadName(const std::string &id, SIM_Cart *cart) {
  std::string dir = CartDir(id);
  FILE *file = fopen((dir + "/" + kNameFile).c_str(), "r");
  if (file != nullptr) {
    if (fgets(cart->name, sizeof(cart->name), file) != nullptr) {
      cart->name[strcspn(cart->name, "\r\n")] = '\0';
    }
    fclose(file);
  }
  if (cart->name[0] == '\0' && cart->hasFiles) {
    SIM_Prj_CartName(dir.c_str(), cart->name, sizeof(cart->name));
    FILE *out = fopen((dir + "/" + kNameFile).c_str(), "w");
    if (out != nullptr) {
      fprintf(out, "%s\n", cart->name);
      fclose(out);
    }
  }
  if (cart->name[0] == '\0') {
    snprintf(cart->name, sizeof(cart->name), "%s", id.empty() ? "Pokemon" : id.c_str());
  }
}

bool RomMatchesGameCode(const char *path) {
  char expected[5] = {0};
#ifdef LIBNTR_CARD_GAME_CODE
  memcpy(expected, LIBNTR_CARD_GAME_CODE, 4);
#else
  memcpy(expected, (const char *)HW_ROM_HEADER_BUF + 0x0C, 4);
#endif
  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  char code[5] = {0};
  bool ok = fseek(file, 0x0C, SEEK_SET) == 0 && fread(code, 1, 4, file) == 4;
  fclose(file);
  return ok && (expected[0] == '\0' || memcmp(code, expected, 4) == 0);
}

void FinishAdd(bool ok, const std::string &id) {
  sAddState = ADD_IDLE;
  if (sAddDone != nullptr) {
    void (*done)(BOOL, const char *) = sAddDone;
    sAddDone = nullptr;
    done(ok ? TRUE : FALSE, id.c_str());
  }
}

// Extracts the picked ROM into a new carts/<id> directory
void ExtractAddedRom(const std::string &path) {
  if (!RomMatchesGameCode(path.c_str())) {
    sAddMessage = "That file is not a ROM of this game.";
    FinishAdd(false, "");
    return;
  }

  std::string id;
  std::error_code error;
  for (int n = 2; n < 100; n++) {
    id = "cart" + std::to_string(n);
    if (!fs::exists(CartDir(id), error)) {
      break;
    }
  }
  fs::create_directories(CartDir(id), error);
  int result = error ? -1 : SIM_NDSExtractTo(path, CartDir(id));
#ifdef SDK_BUILD_ANDROID
  remove(path.c_str()); // the app's own copy of the ROM; the extracted files are all we keep
#endif
  if (result != 0 || !HasFiles(CartDir(id))) {
    fs::remove_all(CartDir(id), error);
    sAddMessage = "That ROM could not be read.";
    FinishAdd(false, "");
    return;
  }

  SIM_Cart cart = {};
  cart.hasFiles = TRUE;
  ReadName(id, &cart);
  sAddMessage = std::string("Added ") + cart.name + ".";
  FinishAdd(true, id);
}

void UpdateAdd() {
#ifdef SDK_BUILD_ANDROID
  if (sAddState == ADD_PICKING) {
    char path[1024] = {0};
    int result = SIM_Android_PollRomImport(path, sizeof(path));
    if (result > 0) {
      sAddState = ADD_EXTRACTING;
      sAddPath = path;
    } else if (result < 0) {
      sAddMessage.clear();
      FinishAdd(false, "");
    }
  }
#endif
  if (sAddState == ADD_EXTRACTING && !sAddPath.empty()) {
    std::string path = sAddPath;
    sAddPath.clear();
    ExtractAddedRom(path);
  }
}

void EnterCart(const std::string &id) {
  std::string dir = CartDir(id);
  if (chdir(dir.c_str()) == 0) {
    printf("Game: %s (%s)\n", id.empty() ? "data directory" : id.c_str(), dir.c_str());
  } else {
    printf("Could not enter %s, staying in the data directory\n", dir.c_str());
    sCurrentId.clear();
    chdir(sBaseDir.c_str());
  }
  sCurrent = {};
  snprintf(sCurrent.id, sizeof(sCurrent.id), "%s", sCurrentId.c_str());
  sCurrent.hasFiles = HasFiles(CartDir(sCurrentId)) ? TRUE : FALSE;
  ReadName(sCurrentId, &sCurrent);
}

void PickerAdded(BOOL ok, const char *id) {
  if (ok) {
    SIM_Cart carts[SIM_CARTS_MAX];
    int count = SIM_Carts_List(carts);
    for (int i = 0; i < count; i++) {
      if (strcmp(carts[i].id, id) == 0) {
        sSelection = i;
      }
    }
  }
}

} // namespace

extern "C" const char *SIM_SettingsPath(const char *fileName) {
  // A few paths are in use at once (config + its temp file...); keep a small ring
  static std::string sPaths[8];
  static int sNext = 0;
  std::string &path = sPaths[sNext];
  sNext = (sNext + 1) % 8;
  path = sBaseDir.empty() ? std::string(fileName) : sBaseDir + "/" + fileName;
  return path.c_str();
}

extern "C" void SIM_Carts_Init(void) {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != nullptr) {
    sBaseDir = cwd;
  }
  LoadState();
}

extern "C" int SIM_Carts_List(SIM_Cart carts[SIM_CARTS_MAX]) {
  int count = 0;

  // The data directory's own game, unless it was never set up and others exist
  SIM_Cart base = {};
  base.hasFiles = HasFiles(sBaseDir) ? TRUE : FALSE;
  ReadName("", &base);
  carts[count++] = base;

  std::error_code error;
  std::string root = sBaseDir + "/carts";
  if (fs::is_directory(root, error)) {
    std::vector<std::string> ids;
    for (const auto &entry : fs::directory_iterator(root, error)) {
      if (entry.is_directory(error) && HasFiles(entry.path().string())) {
        ids.push_back(entry.path().filename().string());
      }
    }
    std::sort(ids.begin(), ids.end());
    for (const std::string &id : ids) {
      if (count == SIM_CARTS_MAX) {
        break;
      }
      SIM_Cart cart = {};
      snprintf(cart.id, sizeof(cart.id), "%s", id.c_str());
      cart.hasFiles = TRUE;
      ReadName(id, &cart);
      carts[count++] = cart;
    }
  }

  if (count > 1 && !base.hasFiles) {
    // Only added games: don't offer the empty data directory
    memmove(&carts[0], &carts[1], sizeof(SIM_Cart) * (count - 1));
    count--;
  }
  return count;
}

extern "C" const SIM_Cart *SIM_Carts_Current(void) {
  return &sCurrent;
}

extern "C" BOOL SIM_Carts_AskAtLaunch(void) {
  return sAskAtLaunch ? TRUE : FALSE;
}

extern "C" void SIM_Carts_SetAskAtLaunch(BOOL ask) {
  sAskAtLaunch = ask != FALSE;
  SaveState();
}

extern "C" void SIM_Carts_Add(void (*done)(BOOL ok, const char *id)) {
  if (sAddState != ADD_IDLE) {
    return;
  }
  sAddDone = done;
  sAddMessage.clear();
#ifdef SDK_BUILD_ANDROID
  sAddState = ADD_PICKING;
  SIM_Android_RequestRomImport();
#else
  sAddState = ADD_PICKING;
  SIM::GUI::Enable();
  SIM::GUI::OpenFileDialog([](std::string path) {
    sAddState = ADD_EXTRACTING;
    sAddPath = path;
  });
#endif
}

extern "C" void SIM_Carts_SwitchTo(const char *id) {
  sCurrentId = id;
  SaveState();
  // Let the game save, as when the app is closed
  SIM_Prj_OnLifecycleEvent(SIM_LIFECYCLE_QUIT);
#ifdef SDK_BUILD_ANDROID
  SIM_Android_RestartApp();
#else
  exit(0);
#endif
}

extern "C" void SIM_Carts_Choose(void) {
  SIM_Cart carts[SIM_CARTS_MAX];
  int count = SIM_Carts_List(carts);

  sSelection = 0;
  for (int i = 0; i < count; i++) {
    if (sCurrentId == carts[i].id) {
      sSelection = i;
    }
  }

  if (count > 1 && sAskAtLaunch) {
    sPickerActive = true;
    while (sPickerActive) {
      SIM_Render(nullptr);
    }
    count = SIM_Carts_List(carts);
    if (sSelection >= count) {
      sSelection = 0;
    }
  }

  sCurrentId = carts[sSelection].id;
  SaveState();
  EnterCart(sCurrentId);
}

extern "C" BOOL SIM_Carts_PickerActive(void) {
  return sPickerActive ? TRUE : FALSE;
}

// Full-window list of the games. Touch, mouse, or the D-pad and A
extern "C" void SIM_Carts_DrawPicker(void) {
  UpdateAdd();
  if (!sPickerActive) {
    return;
  }

  SIM_Cart carts[SIM_CARTS_MAX];
  int count = SIM_Carts_List(carts);
  if (sSelection >= count) {
    sSelection = count - 1;
  }

  // The pad: up/down to choose, A or Start to play (KEYINPUT bits are low when pressed)
  const u16 keys = s_reg_PAD_KEYINPUT;
  const u16 pressed = (u16)(sPreviousKeys & ~keys);
  sPreviousKeys = keys;
  if (sAddState == ADD_IDLE) {
    // Entries 0..count-1 are the games, count is "add a game"
    if ((pressed & (1 << 6)) && sSelection > 0) {
      sSelection--;
    }
    if ((pressed & (1 << 7)) && sSelection < count) {
      sSelection++;
    }
  }

  ImGuiIO &io = ImGui::GetIO();
  const float scale = io.FontGlobalScale;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("Choose a game", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  const float width = ImGui::GetContentRegionAvail().x;
  const float buttonWidth = width * 0.6f;
  const float buttonHeight = 44.0f * scale;
  const float x = (width - buttonWidth) * 0.5f;

  ImGui::Dummy(ImVec2(0, 20.0f * scale));
  ImGui::SetWindowFontScale(1.6f);
  ImVec2 titleSize = ImGui::CalcTextSize("Choose a game");
  ImGui::SetCursorPosX((width - titleSize.x) * 0.5f);
  ImGui::TextUnformatted("Choose a game");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Dummy(ImVec2(0, 14.0f * scale));

  bool start = false;
  for (int i = 0; i <= count; i++) {
    const bool add = i == count;
    const bool selected = sSelection == i;
    ImGui::SetCursorPosX(x);
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.26f, 0.50f, 0.80f, 1.0f) : ImVec4(0.18f, 0.22f, 0.30f, 1.0f));
    ImGui::SetWindowFontScale(1.25f);
    bool clicked = ImGui::Button(add ? (sAddState == ADD_IDLE ? "+  Add a game (choose a ROM)" : "Waiting for the ROM...")
                                     : carts[i].name,
                                 ImVec2(buttonWidth, buttonHeight));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::PopID();
    if (clicked && sAddState == ADD_IDLE) {
      sSelection = i;
      start = !add;
      if (add) {
        SIM_Carts_Add(PickerAdded);
      }
    }
    ImGui::Dummy(ImVec2(0, 4.0f * scale));
  }

  if (sAddState == ADD_IDLE && (pressed & ((1 << 0) | (1 << 3)))) {
    if (sSelection == count) {
      SIM_Carts_Add(PickerAdded);
    } else {
      start = true;
    }
  }

  if (!sAddMessage.empty()) {
    ImGui::SetCursorPosX(x);
    ImGui::TextUnformatted(sAddMessage.c_str());
  }

  ImGui::Dummy(ImVec2(0, 8.0f * scale));
  ImGui::SetCursorPosX(x);
  bool ask = sAskAtLaunch;
  if (ImGui::Checkbox("Ask every time the app starts", &ask)) {
    SIM_Carts_SetAskAtLaunch(ask ? TRUE : FALSE);
  }
  ImGui::SetCursorPosX(x);
  ImGui::PushTextWrapPos(x + buttonWidth);
  ImGui::TextDisabled("Each game keeps its own saves. Switch later in the settings: Application > Games.");
  ImGui::PopTextWrapPos();
  ImGui::End();

  if (start && sSelection < count) {
    sPickerActive = false;
  }
}

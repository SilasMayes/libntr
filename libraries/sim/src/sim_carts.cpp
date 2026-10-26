// Games side by side ("carts"). See include/simulator/sim_carts.h
#include <simulator/sim.h>
#include <simulator/sim_carts.h>
#include <simulator/sim_gui.hpp>
#include <simulator/sim_files.h>
#include <simulator/sim_lifecycle.h>
#include <simulator/sim_vcdiff.h>
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

bool sSkipPickerOnce = false; // switching games: go straight into the chosen one

// Adding a game: the "Add a game" window (a ROM, and patches to apply to it)
struct PickedFile {
  std::string path;
  std::string name;
};
enum AddStep { ADD_IDLE, ADD_PICKING_ROM, ADD_PICKING_PATCH, ADD_WORK_PENDING, ADD_WORKING };
bool sAddOpen = false;
AddStep sAddStep = ADD_IDLE;
PickedFile sAddRom;
std::vector<PickedFile> sAddPatches;
std::string sAddMessage;
bool sAddFailed = false;
std::string sAddedId;   // the game just added, offered to play
std::string sAddedName;
void (*sAddDone)(BOOL ok, const char *id) = nullptr;

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
    } else if (strncmp(line, "SkipPickerOnce=", 15) == 0) {
      sSkipPickerOnce = atoi(line + 15) != 0;
    }
  }
  fclose(file);
}

void SaveState() {
  FILE *file = fopen(SIM_SettingsPath(kStateFile), "w");
  if (file == nullptr) {
    return;
  }
  fprintf(file, "[Carts]\nCurrent=%s\nAskAtLaunch=%d\nSkipPickerOnce=%d\n", sCurrentId.c_str(), sAskAtLaunch ? 1 : 0,
          sSkipPickerOnce ? 1 : 0);
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

// The game code (4 letters at 0x0C) this build runs
void ExpectedGameCode(char expected[5]) {
  memset(expected, 0, 5);
#ifdef LIBNTR_CARD_GAME_CODE
  memcpy(expected, LIBNTR_CARD_GAME_CODE, 4);
#else
  memcpy(expected, (const char *)HW_ROM_HEADER_BUF + 0x0C, 4);
#endif
}

bool GameCodeMatches(const u8 *header, size_t size) {
  char expected[5];
  ExpectedGameCode(expected);
  return size >= 0x10 && (expected[0] == '\0' || memcmp(header + 0x0C, expected, 4) == 0);
}

bool ReadWholeFile(const std::string &path, std::vector<u8> &out) {
  FILE *file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  bool ok = fseek(file, 0, SEEK_END) == 0;
  long size = ok ? ftell(file) : -1;
  ok = ok && size > 0 && fseek(file, 0, SEEK_SET) == 0;
  if (ok) {
    out.resize((size_t)size);
    ok = fread(out.data(), 1, out.size(), file) == out.size();
  }
  fclose(file);
  return ok;
}

bool WriteWholeFile(const std::string &path, const u8 *data, size_t size) {
  FILE *file = fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  bool ok = fwrite(data, 1, size, file) == size;
  ok = fclose(file) == 0 && ok;
  return ok;
}

std::string FileName(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The document picker's copies (Android) live in the data directory's imports/
void RemoveIfImported(const std::string &path) {
  std::string imports = sBaseDir + "/imports/";
  if (path.compare(0, imports.size(), imports) == 0) {
    remove(path.c_str());
  }
}

void FilePicked(const char *path, void *user) {
  const bool isPatch = user != nullptr;
  sAddStep = ADD_IDLE;
  if (path == nullptr) {
    return;
  }
  PickedFile file = { path, FileName(path) };
  if (isPatch) {
    sAddPatches.push_back(file);
  } else {
    if (!sAddRom.path.empty()) {
      RemoveIfImported(sAddRom.path);
    }
    sAddRom = file;
  }
  sAddMessage.clear();
  sAddFailed = false;
}

void AddFailed(const std::string &message) {
  sAddMessage = message;
  sAddFailed = true;
}

std::string NewCartId() {
  std::error_code error;
  for (int n = 2; n < 100; n++) {
    std::string id = "cart" + std::to_string(n);
    if (!fs::exists(CartDir(id), error)) {
      return id;
    }
  }
  return "cart100";
}

// Builds the game from the picked ROM and patches and extracts it into carts/<id>
void DoAdd() {
  std::string romPath = sAddRom.path;
  std::string patchedPath;

  if (!sAddPatches.empty()) {
    std::vector<u8> rom;
    if (!ReadWholeFile(sAddRom.path, rom)) {
      AddFailed("The ROM could not be read.");
      return;
    }
    if (!GameCodeMatches(rom.data(), rom.size())) {
      AddFailed("That ROM is not this game. Choose your Pokemon Platinum (USA) ROM.");
      return;
    }

    std::vector<u8> current = std::move(rom);
    int stackedMismatches = 0;
    for (size_t i = 0; i < sAddPatches.size(); i++) {
      std::vector<u8> patch;
      if (!ReadWholeFile(sAddPatches[i].path, patch)) {
        AddFailed(sAddPatches[i].name + " could not be read.");
        return;
      }
      SIM_VCDiffResult result;
      SIM_VCDiff_Apply(current.data(), current.size(), patch.data(), patch.size(), &result);
      if (result.data == nullptr) {
        AddFailed(sAddPatches[i].name + ": " + result.error);
        return;
      }
      if (i == 0 && result.checksumMismatches > 0) {
        free(result.data);
        AddFailed(sAddPatches[i].name + " was made for a different version of the ROM. "
                  "(Renegade Platinum: 4997 for later carts/dumps, 3541 for early ones.)");
        return;
      }
      stackedMismatches += result.checksumMismatches;
      current.assign(result.data, result.data + result.size);
      free(result.data);
    }
    printf("Add a game: %zu patch(es) applied%s\n", sAddPatches.size(),
           stackedMismatches > 0 ? " (later ones on top of earlier ones)" : "");
    if (!GameCodeMatches(current.data(), current.size())) {
      AddFailed("The patched ROM is not this game.");
      return;
    }

    std::error_code error;
    fs::create_directories(sBaseDir + "/imports", error);
    patchedPath = sBaseDir + "/imports/patched.nds";
    if (!WriteWholeFile(patchedPath, current.data(), current.size())) {
      remove(patchedPath.c_str());
      AddFailed("There is not enough space to build the game.");
      return;
    }
    romPath = patchedPath;
  } else {
    FILE *file = fopen(romPath.c_str(), "rb");
    u8 bytes[0x10] = {0};
    bool ok = file != nullptr && fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    if (file != nullptr) {
      fclose(file);
    }
    if (!ok || !GameCodeMatches(bytes, sizeof(bytes))) {
      AddFailed("That file is not a ROM of this game.");
      return;
    }
  }

  std::string id = NewCartId();
  std::error_code error;
  fs::create_directories(CartDir(id), error);
  int result = error ? -1 : SIM_NDSExtractTo(romPath, CartDir(id));
  if (!patchedPath.empty()) {
    remove(patchedPath.c_str());
  }
  if (result != 0 || !HasFiles(CartDir(id))) {
    fs::remove_all(CartDir(id), error);
    AddFailed("The game could not be extracted (is there enough space?).");
    return;
  }

  // The picked files have served their purpose
  RemoveIfImported(sAddRom.path);
  for (const PickedFile &patch : sAddPatches) {
    RemoveIfImported(patch.path);
  }
  sAddRom = {};
  sAddPatches.clear();

  SIM_Cart cart = {};
  cart.hasFiles = TRUE;
  ReadName(id, &cart);
  sAddedId = id;
  sAddedName = cart.name;
  sAddMessage = std::string("Added ") + cart.name + ".";
  sAddFailed = false;
  if (sAddDone != nullptr) {
    sAddDone(TRUE, id.c_str());
  }
}

void CloseAddWindow() {
  RemoveIfImported(sAddRom.path);
  for (const PickedFile &patch : sAddPatches) {
    RemoveIfImported(patch.path);
  }
  sAddRom = {};
  sAddPatches.clear();
  sAddOpen = false;
  sAddDone = nullptr;
}

void DrawAddWindow();

void EnterCart(const std::string &id) {
  std::string dir = CartDir(id);

  // Some libraries (DWC's backup) open firmware.bin by a relative path: give every game
  // a copy of the shared one
  std::error_code error;
  if (!id.empty() && !fs::exists(dir + "/firmware.bin", error) && fs::exists(SIM_SettingsPath("firmware.bin"), error)) {
    fs::copy_file(SIM_SettingsPath("firmware.bin"), dir + "/firmware.bin", error);
  }

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
  if (sAddOpen) {
    return;
  }
  sAddOpen = true;
  sAddStep = ADD_IDLE;
  sAddMessage.clear();
  sAddFailed = false;
  sAddedId.clear();
  sAddDone = done;
}

extern "C" BOOL SIM_Carts_UIActive(void) {
  return (sPickerActive || sAddOpen) ? TRUE : FALSE;
}

extern "C" void SIM_Carts_SwitchTo(const char *id) {
  sCurrentId = id;
  sSkipPickerOnce = true;
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

  const bool skip = sSkipPickerOnce;
  sSkipPickerOnce = false;
  if (count > 1 && sAskAtLaunch && !skip) {
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

namespace {

ImVec4 ButtonColor(bool selected) {
  return selected ? ImVec4(0.26f, 0.50f, 0.80f, 1.0f) : ImVec4(0.18f, 0.22f, 0.30f, 1.0f);
}

bool BigButton(const char *label, float width, float height, bool selected = false) {
  ImGui::PushStyleColor(ImGuiCol_Button, ButtonColor(selected));
  ImGui::SetWindowFontScale(1.2f);
  bool clicked = ImGui::Button(label, ImVec2(width, height));
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  return clicked;
}

// "Add a game": the ROM, then any patches in order (Renegade Platinum: its patch, then
// the Classic patch)
void DrawAddWindow() {
  if (sAddStep == ADD_WORKING) {
    sAddStep = ADD_IDLE;
    DoAdd();
  } else if (sAddStep == ADD_WORK_PENDING) {
    sAddStep = ADD_WORKING; // let "Building..." show for a frame first
  }

  ImGuiIO &io = ImGui::GetIO();
  const float scale = io.FontGlobalScale;
  const bool busy = sAddStep != ADD_IDLE;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::SetNextWindowFocus();
  ImGui::Begin("Add a game", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  const float width = ImGui::GetContentRegionAvail().x;
  const float columnWidth = width * 0.7f;
  const float x = (width - columnWidth) * 0.5f;
  const float buttonHeight = 36.0f * scale;
  ImGui::PushTextWrapPos(x + columnWidth);

  ImGui::Dummy(ImVec2(0, 10.0f * scale));
  ImGui::SetWindowFontScale(1.5f);
  ImGui::SetCursorPosX((width - ImGui::CalcTextSize("Add a game").x) * 0.5f);
  ImGui::TextUnformatted("Add a game");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Dummy(ImVec2(0, 6.0f * scale));

  if (!sAddedId.empty()) {
    ImGui::SetCursorPosX(x);
    ImGui::TextWrapped("%s", sAddMessage.c_str());
    ImGui::Dummy(ImVec2(0, 6.0f * scale));
    ImGui::SetCursorPosX(x);
    std::string play = "Play " + sAddedName;
    if (BigButton(play.c_str(), columnWidth, buttonHeight, true)) {
      std::string id = sAddedId;
      CloseAddWindow();
      if (sPickerActive) {
        // From the game picker: start it
        SIM_Cart carts[SIM_CARTS_MAX];
        int count = SIM_Carts_List(carts);
        for (int i = 0; i < count; i++) {
          if (id == carts[i].id) {
            sSelection = i;
            sPickerActive = false;
          }
        }
      } else {
        SIM_Carts_SwitchTo(id.c_str());
      }
    }
    ImGui::SetCursorPosX(x);
    if (BigButton(sPickerActive ? "Back to the list" : "Later", columnWidth, buttonHeight)) {
      CloseAddWindow();
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
    return;
  }

  ImGui::SetCursorPosX(x);
  ImGui::TextWrapped("1. The game's ROM (.nds)");
  ImGui::SetCursorPosX(x);
  ImGui::TextDisabled("%s", sAddRom.path.empty() ? "None chosen yet" : sAddRom.name.c_str());
  ImGui::SetCursorPosX(x);
  if (BigButton(sAddStep == ADD_PICKING_ROM ? "Waiting for the file..." : "Choose the ROM...", columnWidth, buttonHeight)
      && !busy) {
    sAddStep = ADD_PICKING_ROM;
    SIM_Files_PickToOpen("Choose the game ROM", ".nds,.srl", FilePicked, nullptr);
  }

  ImGui::Dummy(ImVec2(0, 8.0f * scale));
  ImGui::SetCursorPosX(x);
  ImGui::TextWrapped("2. Patches to apply to it, in order (optional, .xdelta)");
  for (size_t i = 0; i < sAddPatches.size(); i++) {
    ImGui::SetCursorPosX(x);
    ImGui::TextDisabled("%zu. %s", i + 1, sAddPatches[i].name.c_str());
  }
  ImGui::SetCursorPosX(x);
  const float half = (columnWidth - 8.0f * scale) * 0.5f;
  if (BigButton(sAddStep == ADD_PICKING_PATCH ? "Waiting for the file..." : "Add a patch...",
                sAddPatches.empty() ? columnWidth : half, buttonHeight)
      && !busy) {
    sAddStep = ADD_PICKING_PATCH;
    SIM_Files_PickToOpen("Choose a patch", ".xdelta,.vcdiff", FilePicked, (void *)1);
  }
  if (!sAddPatches.empty()) {
    ImGui::SameLine(0, 8.0f * scale);
    if (BigButton("Remove the last", half, buttonHeight) && !busy) {
      RemoveIfImported(sAddPatches.back().path);
      sAddPatches.pop_back();
    }
  }
  ImGui::SetCursorPosX(x);
  ImGui::TextDisabled("Renegade Platinum: your Platinum (USA) ROM, then RenegadePlatinum4997.xdelta "
                      "(RenegadePlatinum3541 for early carts), then ClassicVersion.xdelta for Classic.");

  ImGui::Dummy(ImVec2(0, 8.0f * scale));
  if (!sAddMessage.empty() || busy) {
    ImGui::SetCursorPosX(x);
    if (sAddStep == ADD_WORK_PENDING || sAddStep == ADD_WORKING) {
      ImGui::TextWrapped("Building the game, this takes a little while...");
    } else if (sAddFailed) {
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", sAddMessage.c_str());
    } else {
      ImGui::TextWrapped("%s", sAddMessage.c_str());
    }
  }

  ImGui::SetCursorPosX(x);
  const bool ready = !sAddRom.path.empty() && !busy;
  if (!ready) {
    ImGui::BeginDisabled();
  }
  if (BigButton(sAddPatches.empty() ? "Add the game" : "Patch and add", half, buttonHeight, ready)) {
    sAddStep = ADD_WORK_PENDING;
    sAddMessage.clear();
  }
  if (!ready) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine(0, 8.0f * scale);
  if (BigButton("Cancel", half, buttonHeight) && !busy) {
    CloseAddWindow();
  }

  ImGui::PopTextWrapPos();
  ImGui::End();
}

} // namespace

// The game picker (a full-window list; touch, mouse, or the D-pad and A) and the
// "Add a game" window
extern "C" void SIM_Carts_DrawUI(void) {
  if (sAddOpen) {
    DrawAddWindow();
    return;
  }
  if (!sPickerActive) {
    return;
  }

  SIM_Cart carts[SIM_CARTS_MAX];
  int count = SIM_Carts_List(carts);
  if (sSelection > count) {
    sSelection = count;
  }

  // The pad: up/down to choose, A or Start to play (KEYINPUT bits are low when pressed)
  const u16 keys = s_reg_PAD_KEYINPUT;
  const u16 pressed = (u16)(sPreviousKeys & ~keys);
  sPreviousKeys = keys;
  // Entries 0..count-1 are the games, count is "add a game"
  if ((pressed & (1 << 6)) && sSelection > 0) {
    sSelection--;
  }
  if ((pressed & (1 << 7)) && sSelection < count) {
    sSelection++;
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
  bool add = false;
  for (int i = 0; i <= count; i++) {
    ImGui::SetCursorPosX(x);
    ImGui::PushID(i);
    bool clicked = BigButton(i == count ? "+  Add a game..." : carts[i].name, buttonWidth, buttonHeight, sSelection == i);
    ImGui::PopID();
    if (clicked) {
      sSelection = i;
      add = i == count;
      start = !add;
    }
    ImGui::Dummy(ImVec2(0, 4.0f * scale));
  }

  if (pressed & ((1 << 0) | (1 << 3))) {
    add = sSelection == count;
    start = !add;
  }

  ImGui::Dummy(ImVec2(0, 8.0f * scale));
  ImGui::SetCursorPosX(x);
  bool ask = sAskAtLaunch;
  if (ImGui::Checkbox("Ask every time the app starts", &ask)) {
    SIM_Carts_SetAskAtLaunch(ask ? TRUE : FALSE);
  }
  ImGui::SetCursorPosX(x);
  ImGui::PushTextWrapPos(x + buttonWidth);
  ImGui::TextDisabled("Each game keeps its own saves. Switch any time from the Gear's Settings, or the settings "
                      "GUI: Application > Games.");
  ImGui::PopTextWrapPos();
  ImGui::End();

  if (add) {
    SIM_Carts_Add(nullptr);
  } else if (start && sSelection < count) {
    sPickerActive = false;
  }
}

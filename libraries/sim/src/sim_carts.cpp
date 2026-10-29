// Games side by side ("carts"). See include/simulator/sim_carts.h
#include <simulator/sim.h>
#include <simulator/sim_carts.h>
#include <simulator/sim_lifecycle.h>
#include <simulator/sim_ui.hpp>
#include <simulator/simvariables.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "sim_carts_internal.hpp"

#ifdef SDK_BUILD_ANDROID
#include <simulator/sim_android.h>
#endif

namespace fs = std::filesystem;

extern "C" void __attribute__((weak)) SIM_Prj_CartName(const char *dir, char *name, int nameSize) {
  (void)dir;
  snprintf(name, nameSize, "Pokemon");
}

extern "C" int __attribute__((weak)) SIM_Prj_RandomizerOptions(const SIM_RandomizerOption **options) {
  *options = nullptr;
  return 0;
}

extern "C" BOOL __attribute__((weak)) SIM_Prj_Randomize(const char *dir, u32 seed, u32 options, char *summary,
                                                        int summarySize, char *error, int errorSize) {
  (void)dir;
  (void)seed;
  (void)options;
  (void)summary;
  (void)summarySize;
  snprintf(error, errorSize, "This game has no randomizer.");
  return FALSE;
}

namespace SIM::Carts {

namespace {

std::string sBaseDir;   // the data directory (settings, first game)
std::string sCurrentId; // game to start: "" for the data directory itself
bool sAskAtLaunch = true;
bool sPickerActive = false;
int sSelection = 0;
SIM_Cart sCurrent;

bool sSkipPickerOnce = false; // switching games: go straight into the chosen one
bool sSoftReset = false;      // this process was started by a soft reset
bool sSoftResetNext = false;  // the next one will be
int sArgc = 0;
char **sArgv = nullptr;

constexpr const char *kStateFile = "carts.ini";
constexpr const char *kNameFile = "cart.name";
constexpr const char *kInfoFile = "cart.info";

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
    } else if (strncmp(line, "SoftReset=", 10) == 0) {
      sSoftReset = atoi(line + 10) != 0;
    }
  }
  fclose(file);
}

void SaveState() {
  FILE *file = fopen(SIM_SettingsPath(kStateFile), "w");
  if (file == nullptr) {
    return;
  }
  fprintf(file, "[Carts]\nCurrent=%s\nAskAtLaunch=%d\nSkipPickerOnce=%d\nSoftReset=%d\n", sCurrentId.c_str(),
          sAskAtLaunch ? 1 : 0, sSkipPickerOnce ? 1 : 0, sSoftResetNext ? 1 : 0);
  fclose(file);
}

// Starts the app again (Android: a new process through the activity; desktop: exec)
[[noreturn]] void RestartProcess() {
#ifdef SDK_BUILD_ANDROID
  SIM_Android_RestartApp();
#elif defined(__linux__) || defined(__APPLE__)
  if (sArgv != nullptr && chdir(sBaseDir.c_str()) == 0) {
    fflush(stdout);
    fflush(stderr);
    char self[4096];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
      self[length] = '\0';
      execv(self, sArgv);
    }
    execv(sArgv[0], sArgv);
  }
#endif
  exit(0);
}

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
  LoadVersions();
}

// The game picker at launch: a card per game (touch, or the D-pad and A)
void DrawPicker() {
  using namespace SIM::UI;
  SIM_Cart carts[SIM_CARTS_MAX];
  int count = SIM_Carts_List(carts);
  if (sSelection >= count) {
    sSelection = 0;
  }

  BeginScreen("Choose a game", "Choose a game", nullptr, S(640.0f));
  int start = -1;
  for (int i = 0; i < count; i++) {
    ImGui::PushID(i);
    std::string subtitle = carts[i].info;
    if (i == sSelection) {
      subtitle = subtitle.empty() ? "Played last" : "Played last. " + subtitle;
    }
    if (Card(carts[i].name, subtitle.c_str(), i == sSelection)) {
      start = i;
    }
    if (i == sSelection) {
      SetDefaultFocus();
    }
    ImGui::PopID();
  }
  Space(6.0f);
  bool add = Button("Add a game", "Another ROM, or a ROM hack like Renegade Platinum: choose a folder with the ROM and "
                                  "its patches.");
  bool ask = sAskAtLaunch;
  if (Toggle("Ask every time the app starts", "Turn off to go straight into the game played last.", &ask)) {
    SIM_Carts_SetAskAtLaunch(ask ? TRUE : FALSE);
  }
  Note("Each game keeps its own saves. Switch any time in Settings > Games.");
  EndScreen();

  if (add) {
    OpenAdd(nullptr);
  } else if (start >= 0) {
    sSelection = start;
    sPickerActive = false;
  }
}

} // namespace

std::string BaseDir() {
  return sBaseDir;
}

std::string CartDir(const std::string &id) {
  return id.empty() ? sBaseDir : sBaseDir + "/carts/" + id;
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
  file = fopen((dir + "/" + kInfoFile).c_str(), "r");
  if (file != nullptr) {
    if (fgets(cart->info, sizeof(cart->info), file) != nullptr) {
      cart->info[strcspn(cart->info, "\r\n")] = '\0';
    }
    fclose(file);
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
  std::string trimmed = path;
  while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\')) {
    trimmed.pop_back();
  }
  size_t slash = trimmed.find_last_of("/\\");
  return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

// The document and folder pickers' copies (Android) live in the data directory's imports/
void RemoveIfImported(const std::string &path) {
  std::string imports = sBaseDir + "/imports/";
  if (!path.empty() && path.compare(0, imports.size(), imports) == 0) {
    std::error_code error;
    fs::remove_all(path, error);
  }
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

bool PickerActive() {
  return sPickerActive;
}

void PickerPlay(const std::string &id) {
  SIM_Cart carts[SIM_CARTS_MAX];
  int count = SIM_Carts_List(carts);
  for (int i = 0; i < count; i++) {
    if (id == carts[i].id) {
      sSelection = i;
      sPickerActive = false;
    }
  }
}

} // namespace SIM::Carts

using namespace SIM::Carts;

extern "C" const char *SIM_SettingsPath(const char *fileName) {
  // A few paths are in use at once (config + its temp file...); keep a small ring
  static std::string sPaths[8];
  static int sNext = 0;
  std::string &path = sPaths[sNext];
  sNext = (sNext + 1) % 8;
  path = sBaseDir.empty() ? std::string(fileName) : sBaseDir + "/" + fileName;
  return path.c_str();
}

extern "C" void SIM_Carts_SetArgs(int argc, char **argv) {
  sArgc = argc;
  sArgv = argv;
}

extern "C" void SIM_Carts_Init(void) {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != nullptr) {
    sBaseDir = cwd;
  }
  LoadState();
  if (sSoftReset) {
    // Only this start
    SaveState();
    printf("Started by a soft reset\n");
  }
}

extern "C" BOOL SIM_Carts_StartedBySoftReset(void) {
  return sSoftReset ? TRUE : FALSE;
}

extern "C" void SIM_Carts_SoftReset(void) {
  printf("Soft reset\n");
  sSkipPickerOnce = true;
  sSoftResetNext = true;
  SaveState();
  RestartProcess();
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
    // cart2, cart3... cart10 in the order they were added
    std::sort(ids.begin(), ids.end(), [](const std::string &a, const std::string &b) {
      return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
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
  OpenAdd(done);
}

extern "C" BOOL SIM_Carts_UIActive(void) {
  return (sPickerActive || AddOpen() || AskActive()) ? TRUE : FALSE;
}

extern "C" void SIM_Carts_Back(void) {
  if (AddOpen()) {
    AddBack();
  } else if (AskActive()) {
    AskBack();
  }
}

extern "C" void SIM_Carts_SwitchTo(const char *id) {
  sCurrentId = id;
  sSkipPickerOnce = true;
  SaveState();
  // Let the game save, as when the app is closed
  SIM_Prj_OnLifecycleEvent(SIM_LIFECYCLE_QUIT);
  RestartProcess();
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

// The game picker and the "Add a game" screen
extern "C" void SIM_Carts_DrawUI(void) {
  if (AddOpen()) {
    DrawAdd();
  } else if (sPickerActive) {
    DrawPicker();
  } else if (AskActive()) {
    DrawAsk();
  }
}

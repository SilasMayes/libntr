// A game's versions (e.g. Renegade Platinum and its Classic patch): the files that differ
// per version, which one the game uses, and the screen that asks which one a new game
// gets. See include/simulator/sim_carts.h
#include <simulator/sim.h>
#include <simulator/sim_carts.h>
#include <simulator/sim_ui.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "sim_carts_internal.hpp"

namespace fs = std::filesystem;

extern "C" int __attribute__((weak)) SIM_Prj_VersionFiles(const char *const **paths) {
  *paths = nullptr;
  return 0;
}

extern "C" void __attribute__((weak)) SIM_Prj_VersionName(const char *patchName, char *name, int nameSize) {
  if (patchName == nullptr) {
    snprintf(name, nameSize, "Standard");
    return;
  }
  // "ClassicVersion" -> "Classic"
  std::string text = patchName;
  for (const char *suffix : { "Version", "_version", "-version", " version" }) {
    const size_t length = strlen(suffix);
    if (text.size() > length && text.compare(text.size() - length, length, suffix) == 0) {
      text.resize(text.size() - length);
      break;
    }
  }
  std::replace(text.begin(), text.end(), '_', ' ');
  snprintf(name, nameSize, "%s", text.c_str());
}

namespace SIM::Carts {

namespace {

constexpr const char *kVersionsFile = "versions.ini";
constexpr const char *kVersionsDir = "versions";

// The current game's versions
std::vector<std::string> sNames;
int sInstalled = 0;

// The version in use when it isn't the installed one: its files (relative paths)
std::string sInUse;
std::set<std::string> sOverride;

// The question
struct Ask {
  bool active = false;
  std::string title;
  std::string note;
  std::vector<std::string> names;
  std::vector<std::string> descriptions;
  bool canCancel = false;
  int answer = -2;
};
Ask sAsk;

std::string Normalize(const char *path) {
  std::string text = path;
  std::replace(text.begin(), text.end(), '\\', '/');
  while (text.compare(0, 2, "./") == 0) {
    text.erase(0, 2);
  }
  while (!text.empty() && text[0] == '/') {
    text.erase(0, 1);
  }
  for (char &c : text) {
    c = (char)tolower((unsigned char)c);
  }
  return text;
}

bool ReadVersions(const std::string &dir, std::vector<std::string> &names, int *installed) {
  names.clear();
  *installed = 0;
  FILE *file = fopen((dir + "/" + kVersionsFile).c_str(), "r");
  if (file == nullptr) {
    return false;
  }
  std::string installedName;
  char line[512];
  while (fgets(line, sizeof(line), file) != nullptr) {
    line[strcspn(line, "\r\n")] = '\0';
    if (strncmp(line, "Names=", 6) == 0) {
      std::string list = line + 6;
      size_t start = 0;
      while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!name.empty() && (int)names.size() < SIM_CART_VERSIONS_MAX) {
          names.push_back(name);
        }
        if (comma == std::string::npos) {
          break;
        }
        start = comma + 1;
      }
    } else if (strncmp(line, "Installed=", 10) == 0) {
      installedName = line + 10;
    }
  }
  fclose(file);
  for (size_t i = 0; i < names.size(); i++) {
    if (names[i] == installedName) {
      *installed = (int)i;
    }
  }
  return names.size() > 1;
}

} // namespace

std::vector<std::string> VersionPaths() {
  const char *const *paths = nullptr;
  const int count = SIM_Prj_VersionFiles(&paths);
  std::vector<std::string> out;
  for (int i = 0; i < count && paths != nullptr; i++) {
    out.push_back(paths[i]);
  }
  return out;
}

std::string VersionNameFor(const std::string &patchName) {
  char name[SIM_CART_VERSION_NAME_LEN];
  SIM_Prj_VersionName(patchName.empty() ? nullptr : patchName.c_str(), name, sizeof(name));
  // Names go in a comma-separated list and a folder name
  std::string text = name;
  for (char &c : text) {
    if (c == ',' || c == '/' || c == '\\' || c == ':' || c == '=') {
      c = ' ';
    }
  }
  return text.empty() ? std::string("Version") : text;
}

bool HasVersions(const std::string &dir) {
  std::vector<std::string> names;
  int installed;
  return ReadVersions(dir, names, &installed);
}

bool WriteVersions(const std::string &dir, const std::vector<VersionFiles> &versions, int installed) {
  std::error_code error;
  fs::remove_all(dir + "/" + kVersionsDir, error);
  std::string names;
  for (size_t v = 0; v < versions.size(); v++) {
    names += (v > 0 ? "," : "") + versions[v].name;
    if ((int)v == installed) {
      continue; // the game's own files
    }
    for (const auto &file : versions[v].files) {
      const fs::path path = fs::path(dir) / kVersionsDir / versions[v].name / file.first;
      fs::create_directories(path.parent_path(), error);
      if (error || !WriteWholeFile(path.string(), file.second.data(), file.second.size())) {
        return false;
      }
    }
  }
  std::string text = "[Versions]\nInstalled=" + versions[installed].name + "\nNames=" + names + "\n";
  return WriteWholeFile(dir + "/" + kVersionsFile, (const u8 *)text.data(), text.size());
}

void LoadVersions() {
  ReadVersions(".", sNames, &sInstalled);
  if (sNames.size() < 2) {
    sNames.clear();
    sInstalled = 0;
  }
  SIM_Carts_UseVersion(nullptr);
}

bool AskActive() {
  return sAsk.active;
}

void AskBack() {
  if (sAsk.active && sAsk.canCancel) {
    sAsk.active = false;
    sAsk.answer = -1;
  }
}

// One card per version: touch it, or the D-pad and A
void DrawAsk() {
  using namespace SIM::UI;
  static bool sWasActive = false;
  if (!sWasActive) {
    RequestFocus();
  }
  sWasActive = sAsk.active;
  const bool back = BeginScreen("Choose a version", sAsk.title.c_str(), sAsk.canCancel ? "Back" : nullptr, S(640.0f));
  if (!sAsk.note.empty()) {
    Note(sAsk.note.c_str());
  }
  int chosen = -1;
  for (size_t i = 0; i < sAsk.names.size(); i++) {
    ImGui::PushID((int)i);
    if (Card(sAsk.names[i].c_str(), sAsk.descriptions[i].c_str(), false)) {
      chosen = (int)i;
    }
    if (i == 0) {
      SetDefaultFocus();
    }
    ImGui::PopID();
  }
  EndScreen();
  if (chosen >= 0) {
    sAsk.active = false;
    sAsk.answer = chosen;
    sWasActive = false;
  } else if (back) {
    AskBack();
    sWasActive = false;
  }
}

} // namespace SIM::Carts

using namespace SIM::Carts;

extern "C" int SIM_Carts_Versions(char names[SIM_CART_VERSIONS_MAX][SIM_CART_VERSION_NAME_LEN], int *installed) {
  for (size_t i = 0; i < sNames.size() && i < SIM_CART_VERSIONS_MAX; i++) {
    snprintf(names[i], SIM_CART_VERSION_NAME_LEN, "%s", sNames[i].c_str());
  }
  if (installed != nullptr) {
    *installed = sInstalled;
  }
  return (int)std::min<size_t>(sNames.size(), SIM_CART_VERSIONS_MAX);
}

extern "C" void SIM_Carts_UseVersion(const char *name) {
  sInUse.clear();
  sOverride.clear();
  if (name == nullptr || name[0] == '\0' || sNames.empty() || sNames[sInstalled] == name) {
    return;
  }
  if (std::find(sNames.begin(), sNames.end(), std::string(name)) == sNames.end()) {
    printf("Version %s: not one of this game's\n", name);
    return;
  }
  const fs::path root = fs::path(kVersionsDir) / name;
  std::error_code error;
  for (auto it = fs::recursive_directory_iterator(root, error); !error && it != fs::recursive_directory_iterator();
       it.increment(error)) {
    if (it->is_regular_file(error)) {
      sOverride.insert(Normalize(fs::relative(it->path(), root, error).generic_string().c_str()));
    }
  }
  sInUse = name;
  printf("Version: %s (%d file%s of its own)\n", name, (int)sOverride.size(), sOverride.size() == 1 ? "" : "s");
}

extern "C" const char *SIM_Carts_VersionInUse(void) {
  return sInUse.empty() ? (sNames.empty() ? "" : sNames[sInstalled].c_str()) : sInUse.c_str();
}

extern "C" const char *SIM_Carts_VersionFile(const char *path) {
  if (sOverride.empty() || path == nullptr) {
    return path;
  }
  std::string key = Normalize(path);
  if (sOverride.find(key) == sOverride.end()) {
    return path;
  }
  thread_local std::string sPath;
  sPath = std::string(kVersionsDir) + "/" + sInUse + "/" + key;
  return sPath.c_str();
}

extern "C" void SIM_Carts_AskVersion(const char *title, const char *note, const char *const *names,
                                     const char *const *descriptions, int count, BOOL canCancel) {
  sAsk = Ask();
  sAsk.title = title != nullptr ? title : "Choose a version";
  sAsk.note = note != nullptr ? note : "";
  for (int i = 0; i < count; i++) {
    sAsk.names.push_back(names[i]);
    sAsk.descriptions.push_back(descriptions != nullptr && descriptions[i] != nullptr ? descriptions[i] : "");
  }
  sAsk.canCancel = canCancel != FALSE;
  sAsk.active = count > 0;
  sAsk.answer = count > 0 ? -2 : -1;
}

extern "C" int SIM_Carts_TakeVersionAnswer(void) {
  if (sAsk.active) {
    return -2;
  }
  const int answer = sAsk.answer;
  sAsk.answer = -1;
  return answer;
}

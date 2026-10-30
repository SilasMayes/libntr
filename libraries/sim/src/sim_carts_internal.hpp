// Shared by sim_carts.cpp (the games, the picker) and sim_carts_add.cpp (adding a game)
#ifndef LIBNTR_SIM_CARTS_INTERNAL_HPP
#define LIBNTR_SIM_CARTS_INTERNAL_HPP

#include <simulator/sim_carts.h>

#include <string>
#include <utility>
#include <vector>

// The game's name where the screens talk about its ROM (libntr's card_game_title option)
#ifndef LIBNTR_CARD_GAME_TITLE
#define LIBNTR_CARD_GAME_TITLE "Pokemon Platinum (USA)"
#endif

namespace SIM::Carts {

std::string BaseDir();
std::string CartDir(const std::string &id);
bool HasFiles(const std::string &dir);
void ReadName(const std::string &id, SIM_Cart *cart);
std::string GameCodeList(); // "CPUE", "IPKE,IPGE"; "" for any game (see SIM_Carts_RunsGameCode)
bool GameCodeMatches(const u8 *header, size_t size);
bool ReadWholeFile(const std::string &path, std::vector<u8> &out);
bool WriteWholeFile(const std::string &path, const u8 *data, size_t size);
std::string FileName(const std::string &path);
// Removes a file or folder the pickers copied into the data directory's imports/
void RemoveIfImported(const std::string &path);
std::string NewCartId();

// sim_carts_versions.cpp
struct VersionFiles {
  std::string name;  // "Classic"
  std::string patch; // the patch that makes it ("" for the game without one)
  std::vector<std::pair<std::string, std::vector<u8>>> files; // ROM path, contents
};
std::vector<std::string> VersionPaths(); // SIM_Prj_VersionFiles
std::string VersionNameFor(const std::string &patchName); // "" for the game without a patch
bool HasVersions(const std::string &dir);
// versions/<name>/... for every version but installed (whose files are the game's own), and versions.ini
bool WriteVersions(const std::string &dir, const std::vector<VersionFiles> &versions, int installed);
void LoadVersions(); // the game just entered (working directory)
bool AskActive();
void DrawAsk();
void AskBack();

bool PickerActive();
void PickerPlay(const std::string &id); // the picker starts this game

// sim_carts_add.cpp
bool AddOpen();
void OpenAdd(void (*done)(BOOL ok, const char *id));
void DrawAdd();
void AddBack();

} // namespace SIM::Carts

#endif // LIBNTR_SIM_CARTS_INTERNAL_HPP

// Shared by sim_carts.cpp (the games, the picker) and sim_carts_add.cpp (adding a game)
#ifndef LIBNTR_SIM_CARTS_INTERNAL_HPP
#define LIBNTR_SIM_CARTS_INTERNAL_HPP

#include <simulator/sim_carts.h>

#include <string>
#include <vector>

namespace SIM::Carts {

std::string BaseDir();
std::string CartDir(const std::string &id);
bool HasFiles(const std::string &dir);
void ReadName(const std::string &id, SIM_Cart *cart);
void ExpectedGameCode(char expected[5]);
bool GameCodeMatches(const u8 *header, size_t size);
bool ReadWholeFile(const std::string &path, std::vector<u8> &out);
bool WriteWholeFile(const std::string &path, const u8 *data, size_t size);
std::string FileName(const std::string &path);
// Removes a file or folder the pickers copied into the data directory's imports/
void RemoveIfImported(const std::string &path);
std::string NewCartId();

bool PickerActive();
void PickerPlay(const std::string &id); // the picker starts this game

// sim_carts_add.cpp
bool AddOpen();
void OpenAdd(void (*done)(BOOL ok, const char *id));
void DrawAdd();
void AddBack();

} // namespace SIM::Carts

#endif // LIBNTR_SIM_CARTS_INTERNAL_HPP

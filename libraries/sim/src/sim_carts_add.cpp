// Adding a game: from a folder holding a ROM and patches (which patches go on the ROM, on
// each other, or together is worked out by applying them), or from a ROM and patches
// chosen one by one. See include/simulator/sim_carts.h
//
// A folder is worked out like this: every patch is applied to the ROM; the ones whose
// result checks out (xdelta's checksums, sim_vcdiff.h) apply to it. Those that rebuild
// the ROM (e.g. Renegade Platinum) are the games to choose from; every other patch is
// then applied to each of them, and the ones that check out are its extras. Extras that
// change bytes in place are kept as those changes, so any of them can go together unless
// they change the same bytes (e.g. two shiny-rate patches: pick one).
#include <simulator/sim.h>
#include <simulator/sim_carts.h>
#include <simulator/sim_files.h>
#include <simulator/sim_ui.hpp>
#include <simulator/sim_vcdiff.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "nds-extract.hpp"
#include "sim_carts_internal.hpp"

namespace fs = std::filesystem;

namespace SIM::Carts {

namespace {

using namespace SIM::UI;

// --- work on a thread -----------------------------------------------------------------------

struct Job {
  std::thread thread;
  std::atomic<bool> running{ false };
  std::atomic<bool> cancel{ false };
  std::mutex lock;
  std::string step;
  float fraction = -1.0f;
  std::function<void()> finish; // set by the work: runs on the GUI thread once it's done
};
Job sJob;

void SetStep(const std::string &step, float fraction) {
  std::lock_guard<std::mutex> guard(sJob.lock);
  sJob.step = step;
  sJob.fraction = fraction;
}

void StartJob(const std::function<void()> &work) {
  if (sJob.thread.joinable()) {
    sJob.thread.join();
  }
  sJob.cancel = false;
  sJob.finish = nullptr;
  SetStep("Starting...", -1.0f);
  sJob.running = true;
  sJob.thread = std::thread([work] {
    work();
    sJob.running = false;
  });
}

bool JobBusy() {
  return sJob.running || sJob.thread.joinable();
}

// GUI thread, every frame: hands the finished work's results over
void PollJob() {
  if (!sJob.running && sJob.thread.joinable()) {
    sJob.thread.join();
    std::function<void()> finish = std::move(sJob.finish);
    sJob.finish = nullptr;
    if (finish) {
      finish();
    }
  }
}

// --- patches --------------------------------------------------------------------------------

std::string Lower(std::string text) {
  for (char &c : text) {
    c = (char)tolower((unsigned char)c);
  }
  return text;
}

std::string Extension(const std::string &name) {
  size_t dot = name.rfind('.');
  return dot == std::string::npos ? std::string() : Lower(name.substr(dot));
}

std::string StripExtension(const std::string &name) {
  size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

struct Applied {
  std::vector<u8> data;
  int mismatches = 0;
  std::string error;
};

// Applies a patch file to source
Applied Apply(const std::vector<u8> &source, const std::string &patchPath) {
  Applied applied;
  std::vector<u8> patch;
  if (!ReadWholeFile(patchPath, patch)) {
    applied.error = "could not be read";
    return applied;
  }
  SIM_VCDiffResult result;
  SIM_VCDiff_Apply(source.data(), source.size(), patch.data(), patch.size(), &result);
  if (result.data == nullptr) {
    applied.error = result.error;
    return applied;
  }
  applied.data.assign(result.data, result.data + result.size);
  applied.mismatches = result.checksumMismatches;
  free(result.data);
  return applied;
}

// Bytes a patch changes in place
struct Change {
  u32 offset;
  u32 length;
  u32 data; // index into the pool
};

constexpr size_t kMaxInPlaceChanges = 16 * 1024 * 1024;

// The in-place changes from a to b (FALSE: not in place, or too many)
bool Diff(const std::vector<u8> &a, const std::vector<u8> &b, std::vector<Change> &changes, std::vector<u8> &pool) {
  changes.clear();
  pool.clear();
  if (a.size() != b.size()) {
    return false;
  }
  const size_t size = a.size();
  const size_t kBlock = 4096;
  size_t i = 0;
  while (i < size) {
    size_t block = std::min(kBlock, size - i);
    if (memcmp(&a[i], &b[i], block) == 0) {
      i += block;
      continue;
    }
    size_t end = i + block;
    while (i < end) {
      if (a[i] == b[i]) {
        i++;
        continue;
      }
      // A run of changes; equal stretches shorter than 16 bytes join neighbouring runs
      size_t start = i;
      size_t last = i;
      while (i < size && i < last + 16) {
        if (a[i] != b[i]) {
          last = i;
        }
        i++;
      }
      Change change = { (u32)start, (u32)(last + 1 - start), (u32)pool.size() };
      pool.insert(pool.end(), b.begin() + start, b.begin() + last + 1);
      if (!changes.empty() && changes.back().offset + changes.back().length == change.offset) {
        changes.back().length += change.length;
      } else {
        changes.push_back(change);
      }
      if (pool.size() > kMaxInPlaceChanges) {
        changes.clear();
        pool.clear();
        return false;
      }
      i = last + 1;
      end = std::max(end, i);
    }
  }
  return true;
}

bool Overlap(const std::vector<Change> &a, const std::vector<Change> &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].offset + a[i].length <= b[j].offset) {
      i++;
    } else if (b[j].offset + b[j].length <= a[i].offset) {
      j++;
    } else {
      return true;
    }
  }
  return false;
}

// --- files in a ROM image, and versions -------------------------------------------------------

u32 Read32(const std::vector<u8> &rom, size_t offset) {
  return offset + 4 <= rom.size() ? (u32)rom[offset] | ((u32)rom[offset + 1] << 8) | ((u32)rom[offset + 2] << 16) |
                                        ((u32)rom[offset + 3] << 24)
                                  : 0;
}

u16 Read16(const std::vector<u8> &rom, size_t offset) {
  return offset + 2 <= rom.size() ? (u16)(rom[offset] | (rom[offset + 1] << 8)) : 0;
}

// Where a file of the ROM's file system is (path as the game opens it, e.g.
// "poketool/personal/pl_personal.narc")
bool RomFileRange(const std::vector<u8> &rom, const std::string &path, u32 *offset, u32 *size) {
  const u32 fntOffset = Read32(rom, 0x40), fatOffset = Read32(rom, 0x48), fatSize = Read32(rom, 0x4C);
  if (fntOffset == 0 || (size_t)fntOffset + 8 > rom.size()) {
    return false;
  }
  u32 dir = 0;
  size_t start = 0;
  while (start < path.size()) {
    size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    const bool last = slash == std::string::npos;
    start = last ? path.size() : slash + 1;

    size_t p = fntOffset + Read32(rom, fntOffset + dir * 8);
    u32 fileId = Read16(rom, fntOffset + dir * 8 + 4);
    bool found = false;
    while (p < rom.size()) {
      const u8 type = rom[p++];
      if (type == 0) {
        break;
      }
      const size_t length = type & 0x7F;
      if (p + length > rom.size()) {
        return false;
      }
      bool match = length == part.size();
      for (size_t c = 0; match && c < length; c++) {
        match = tolower(rom[p + c]) == tolower((unsigned char)part[c]);
      }
      p += length;
      if (type & 0x80) {
        const u32 sub = Read16(rom, p) & 0xFFF;
        p += 2;
        if (match && !last) {
          dir = sub;
          found = true;
          break;
        }
      } else {
        if (match && last) {
          if ((size_t)fileId * 8 + 8 > fatSize) {
            return false;
          }
          const u32 begin = Read32(rom, fatOffset + fileId * 8), end = Read32(rom, fatOffset + fileId * 8 + 4);
          if (end < begin || end > rom.size()) {
            return false;
          }
          *offset = begin;
          *size = end - begin;
          return true;
        }
        fileId++;
      }
    }
    if (!found) {
      return false;
    }
  }
  return false;
}

// A version's changes, as edits of the files the game keeps per version
struct FileEdit {
  std::string path;
  u32 offset; // in the file
  std::vector<u8> bytes;
};

// changes (made to rom in place) as edits of the version files; false if any is elsewhere
bool AsVersionEdits(const std::vector<u8> &rom, const std::vector<Change> &changes, const std::vector<u8> &pool,
                    std::vector<FileEdit> &edits) {
  edits.clear();
  const std::vector<std::string> paths = VersionPaths();
  if (paths.empty() || changes.empty()) {
    return false;
  }
  std::vector<std::pair<u32, u32>> ranges;
  for (const std::string &path : paths) {
    u32 offset = 0, size = 0;
    ranges.push_back(RomFileRange(rom, path, &offset, &size) ? std::make_pair(offset, size) : std::make_pair(0u, 0u));
  }
  for (const Change &change : changes) {
    bool inside = false;
    for (size_t i = 0; i < paths.size(); i++) {
      const u32 begin = ranges[i].first, size = ranges[i].second;
      if (size > 0 && change.offset >= begin && (u64)change.offset + change.length <= (u64)begin + size) {
        edits.push_back({ paths[i], change.offset - begin,
                          std::vector<u8>(pool.begin() + change.data, pool.begin() + change.data + change.length) });
        inside = true;
        break;
      }
    }
    if (!inside) {
      edits.clear();
      return false;
    }
  }
  return true;
}

struct VersionPatch {
  std::string patch; // its file name without the extension
  std::vector<FileEdit> edits;
};

// Each version's files out of game (the game as built, without any version patch): first
// the game's own, then one per version patch. False if a file isn't in the game.
bool MakeVersionFiles(const std::vector<u8> &game, const std::vector<VersionPatch> &patches,
                      std::vector<VersionFiles> &versions) {
  versions.clear();
  VersionFiles own;
  own.name = VersionNameFor("");
  for (const std::string &path : VersionPaths()) {
    u32 offset = 0, size = 0;
    if (!RomFileRange(game, path, &offset, &size)) {
      return false;
    }
    own.files.push_back({ path, std::vector<u8>(game.begin() + offset, game.begin() + offset + size) });
  }
  versions.push_back(own);
  for (const VersionPatch &patch : patches) {
    VersionFiles version = own;
    version.name = VersionNameFor(patch.patch);
    version.patch = patch.patch;
    for (const FileEdit &edit : patch.edits) {
      for (auto &file : version.files) {
        if (file.first == edit.path && (size_t)edit.offset + edit.bytes.size() <= file.second.size()) {
          memcpy(&file.second[edit.offset], edit.bytes.data(), edit.bytes.size());
        }
      }
    }
    for (const VersionFiles &other : versions) {
      if (other.name == version.name) {
        version.name += " " + std::to_string(versions.size() + 1);
      }
    }
    versions.push_back(version);
  }
  return true;
}

// The cart's info line for a game with versions: "RenegadePlatinum4997; versions Complete, Classic"
std::string VersionsInfo(const std::string &info, const std::vector<VersionFiles> &versions) {
  std::string text = info + (info.empty() ? "" : "; ") + "versions ";
  for (size_t v = 0; v < versions.size(); v++) {
    text += (v > 0 ? ", " : "") + versions[v].name;
  }
  return text;
}

// --- a folder's plan --------------------------------------------------------------------------

struct Patch {
  std::string path;
  std::string name; // file name without the extension
};

struct Option {
  int patch = -1;
  bool inPlace = true; // FALSE: it rebuilds the game, so it goes on its own
  std::vector<Change> changes;
  std::vector<u8> pool;
  int group = -1;
  bool version = false; // only changes the version files: a version of the game, kept alongside
  std::vector<FileEdit> edits; // a version's changes
};

struct Group {
  std::vector<int> options;
  int selected = 0; // 0: none, n: options[n - 1]
  std::string label;
  std::vector<std::string> choices; // "None", then the options' names
};

struct Base {
  int patch = -1; // -1: the ROM as it is
  std::vector<Option> options;
  std::vector<Group> groups;
  std::vector<int> versions;            // options that are versions of the game
  std::vector<VersionFiles> versionFiles; // the game's own (first), then each version's
};

struct Unused {
  std::string name;
  std::string reason;
};

struct Plan {
  std::string folder;
  std::string folderName;
  std::vector<std::string> roms; // ROMs of this game
  std::vector<std::string> otherRoms;
  int rom = 0;
  std::vector<Patch> patches;
  std::vector<Base> bases;
  int base = 0;
  std::vector<Unused> unused;
  std::string error;
};

std::string BaseName(const Plan &plan, const Base &base) {
  return base.patch < 0 ? std::string("Original game") : plan.patches[base.patch].name;
}

void ListFolder(const std::string &dir, int depth, std::vector<fs::path> &files) {
  std::error_code error;
  for (const auto &entry : fs::directory_iterator(dir, error)) {
    if (entry.is_directory(error)) {
      if (depth == 0) {
        ListFolder(entry.path().string(), depth + 1, files);
      }
    } else if (entry.is_regular_file(error)) {
      files.push_back(entry.path());
    }
  }
}

std::string ReadHeader(const std::string &path, size_t size) {
  std::string header;
  FILE *file = fopen(path.c_str(), "rb");
  if (file != nullptr) {
    header.resize(size);
    header.resize(fread(&header[0], 1, size, file));
    fclose(file);
  }
  return header;
}

std::string SkipReason(const std::string &name, const std::string &header) {
  std::string ext = Extension(name);
  if (header.compare(0, 5, "PATCH") == 0 || ext == ".ips") {
    return "IPS patches aren't supported, only xdelta (.xdelta)";
  }
  if (header.compare(0, 4, "BPS1") == 0 || ext == ".bps") {
    return "BPS patches aren't supported, only xdelta (.xdelta)";
  }
  if (header.compare(0, 4, "UPS1") == 0 || ext == ".ups") {
    return "UPS patches aren't supported, only xdelta (.xdelta)";
  }
  if (ext == ".zip" || ext == ".7z" || ext == ".rar") {
    return "an archive: extract it, then choose the folder again";
  }
  return std::string();
}

// game: the base as built. Versions of it (patches only changing the version files) are
// set aside; the other options are grouped
void SortGroups(Plan &plan, Base &base, const std::vector<u8> &game) {
  const int count = (int)base.options.size();
  for (int i = 0; i < count; i++) {
    Option &option = base.options[i];
    if (option.inPlace && AsVersionEdits(game, option.changes, option.pool, option.edits)) {
      option.version = true;
      base.versions.push_back(i);
    }
  }
  if (!base.versions.empty()) {
    std::vector<VersionPatch> patches;
    for (int index : base.versions) {
      patches.push_back({ plan.patches[base.options[index].patch].name, base.options[index].edits });
    }
    if (!MakeVersionFiles(game, patches, base.versionFiles)) {
      for (int index : base.versions) {
        base.options[index].version = false;
      }
      base.versions.clear();
      base.versionFiles.clear();
    }
  }

  // Options that change the same bytes (or rebuild the game) can't go together
  std::vector<int> parent(count);
  std::iota(parent.begin(), parent.end(), 0);
  std::function<int(int)> find = [&](int i) { return parent[i] == i ? i : parent[i] = find(parent[i]); };
  for (int i = 0; i < count; i++) {
    for (int j = i + 1; j < count; j++) {
      if (base.options[i].inPlace && base.options[j].inPlace && !base.options[i].version && !base.options[j].version &&
          Overlap(base.options[i].changes, base.options[j].changes)) {
        parent[find(i)] = find(j);
      }
    }
  }
  std::vector<int> groupOf(count, -1);
  for (int i = 0; i < count; i++) {
    if (base.options[i].version) {
      continue;
    }
    int root = find(i);
    if (groupOf[root] < 0) {
      groupOf[root] = (int)base.groups.size();
      base.groups.push_back({});
    }
    base.options[i].group = groupOf[root];
    base.groups[groupOf[root]].options.push_back(i);
  }

  for (Group &group : base.groups) {
    if (group.options.size() == 1) {
      const Option &option = base.options[group.options[0]];
      group.label = plan.patches[option.patch].name;
      group.selected = option.inPlace ? 1 : 0; // extras go on unless they rebuild the game
      continue;
    }
    // A choice: "ShinyRate_1_4096" and "ShinyRate_1_8192" -> "ShinyRate": 4096 / 8192
    std::string prefix = plan.patches[base.options[group.options[0]].patch].name;
    for (int index : group.options) {
      const std::string &name = plan.patches[base.options[index].patch].name;
      size_t n = 0;
      while (n < prefix.size() && n < name.size() && tolower((unsigned char)prefix[n]) == tolower((unsigned char)name[n])) {
        n++;
      }
      prefix.resize(n);
    }
    std::string label = prefix;
    while (!label.empty() && (label.back() == '_' || label.back() == '-' || label.back() == ' ' || label.back() == '.' ||
                              isdigit((unsigned char)label.back()))) {
      label.pop_back();
    }
    group.label = label.size() >= 3 ? label : std::string("Pick one");
    group.choices.push_back("None");
    for (int index : group.options) {
      std::string name = plan.patches[base.options[index].patch].name;
      std::string rest = label.size() >= 3 ? name.substr(label.size()) : name;
      while (!rest.empty() && (rest[0] == '_' || rest[0] == '-' || rest[0] == ' ')) {
        rest.erase(0, 1);
      }
      for (char &c : rest) {
        c = c == '_' ? ' ' : c;
      }
      // "1 4096" (ShinyRate_1_4096) reads as odds
      if (rest.size() > 2 && rest.compare(0, 2, "1 ") == 0 &&
          std::all_of(rest.begin() + 2, rest.end(), [](char c) { return isdigit((unsigned char)c) != 0; })) {
        rest = "1 in " + rest.substr(2);
      }
      group.choices.push_back(rest.empty() ? name : rest);
    }
    group.selected = 0;
  }
}

// Worker: works out what the folder's patches do to its ROM
void Evaluate(Plan &plan) {
  std::vector<u8> rom;
  SetStep("Reading the ROM...", -1.0f);
  if (!ReadWholeFile(plan.roms[plan.rom], rom)) {
    plan.error = "The ROM could not be read.";
    return;
  }

  const int patchCount = (int)plan.patches.size();
  int done = 0;
  int total = patchCount * 2;
  auto step = [&](const std::string &text) {
    SetStep(text, total > 0 ? (float)done / (float)total : -1.0f);
    done++;
  };

  // 1. Every patch on the ROM
  std::vector<int> rebuilds; // they check out and rebuild the game: games to choose from
  std::vector<u8> firstRebuilt; // kept, so it isn't built twice
  std::vector<int> notOnRom;
  std::vector<Option> inPlace;
  std::vector<std::string> errors(patchCount);
  for (int i = 0; i < patchCount && !sJob.cancel; i++) {
    step("Checking " + plan.patches[i].name + "...");
    Applied applied = Apply(rom, plan.patches[i].path);
    if (!applied.error.empty()) {
      errors[i] = applied.error;
      continue;
    }
    if (applied.mismatches > 0) {
      notOnRom.push_back(i);
      continue;
    }
    Option option;
    option.patch = i;
    if (Diff(rom, applied.data, option.changes, option.pool)) {
      inPlace.push_back(std::move(option));
    } else {
      if (rebuilds.empty()) {
        firstRebuilt = std::move(applied.data);
      }
      rebuilds.push_back(i);
    }
  }
  total = done + (int)(rebuilds.size() * (notOnRom.size() + 1)) - 1;

  // 2. The rest on each rebuilt game
  std::vector<bool> used(patchCount, false);
  for (const Option &option : inPlace) {
    used[option.patch] = true;
  }
  for (int b : rebuilds) {
    if (sJob.cancel) {
      return;
    }
    used[b] = true;
    Base base;
    base.patch = b;
    Applied built;
    if (b == rebuilds[0]) {
      built.data = std::move(firstRebuilt);
    } else {
      step("Applying " + plan.patches[b].name + "...");
      built = Apply(rom, plan.patches[b].path);
      if (!built.error.empty()) {
        continue;
      }
    }
    for (int q : notOnRom) {
      if (sJob.cancel) {
        return;
      }
      step("Checking " + plan.patches[q].name + " on " + plan.patches[b].name + "...");
      Applied applied = Apply(built.data, plan.patches[q].path);
      if (!applied.error.empty() || applied.mismatches > 0) {
        continue;
      }
      Option option;
      option.patch = q;
      option.inPlace = Diff(built.data, applied.data, option.changes, option.pool);
      used[q] = true;
      base.options.push_back(std::move(option));
    }
    SortGroups(plan, base, built.data);
    plan.bases.push_back(std::move(base));
  }
  // Games with the most extras first
  std::stable_sort(plan.bases.begin(), plan.bases.end(),
                   [](const Base &a, const Base &b) { return a.options.size() > b.options.size(); });
  if (!inPlace.empty() || plan.bases.empty()) {
    Base original;
    original.options = std::move(inPlace);
    SortGroups(plan, original, rom);
    plan.bases.push_back(std::move(original));
  }

  for (int i = 0; i < patchCount; i++) {
    if (!errors[i].empty()) {
      plan.unused.push_back({ plan.patches[i].name, "can't be read as an xdelta patch (" + errors[i] + ")" });
    } else if (!used[i]) {
      plan.unused.push_back({ plan.patches[i].name, "made for a different ROM or version" });
    }
  }
}

// Worker: lists the folder's ROMs and patches, then evaluates them
std::unique_ptr<Plan> Scan(const std::string &folder, const std::string &extraRom, int romIndex) {
  auto plan = std::make_unique<Plan>();
  plan->folder = folder;
  plan->folderName = FileName(folder);
  SetStep("Looking in " + plan->folderName + "...", -1.0f);

  std::vector<fs::path> files;
  ListFolder(folder, 0, files);
  std::sort(files.begin(), files.end());
  for (const fs::path &file : files) {
    const std::string path = file.string();
    const std::string name = file.filename().string();
    if (name == ".skipped") {
      // Android: what the folder copy left out
      FILE *list = fopen(path.c_str(), "r");
      char line[512];
      while (list != nullptr && fgets(line, sizeof(line), list) != nullptr) {
        line[strcspn(line, "\r\n")] = '\0';
        std::string reason = SkipReason(line, "");
        if (line[0] != '\0' && !reason.empty()) {
          plan->unused.push_back({ line, reason });
        }
      }
      if (list != nullptr) {
        fclose(list);
      }
      continue;
    }
    if (name.empty() || name[0] == '.') {
      continue;
    }
    const std::string ext = Extension(name);
    const std::string header = ReadHeader(path, 16);
    if (ext == ".nds" || ext == ".srl") {
      if (GameCodeMatches((const u8 *)header.data(), header.size())) {
        plan->roms.push_back(path);
      } else {
        plan->otherRoms.push_back(name);
      }
    } else if (header.size() >= 4 && (u8)header[0] == 0xD6 && (u8)header[1] == 0xC3 && (u8)header[2] == 0xC4) {
      plan->patches.push_back({ path, StripExtension(name) });
    } else if (!SkipReason(name, header).empty()) {
      plan->unused.push_back({ name, SkipReason(name, header) });
    } else if (ext == ".xdelta" || ext == ".vcdiff" || ext == ".delta") {
      plan->unused.push_back({ StripExtension(name), "isn't an xdelta patch" });
    }
  }
  if (!extraRom.empty()) {
    plan->roms.push_back(extraRom);
  }
  if (plan->roms.empty()) {
    return plan;
  }
  if (romIndex >= 0 && romIndex < (int)plan->roms.size()) {
    plan->rom = romIndex;
    Evaluate(*plan);
    return plan;
  }
  // Several ROMs (e.g. an original and an already patched one): the first the patches
  // apply to
  const Plan listed = *plan;
  for (int i = 0; i < (int)listed.roms.size() && !sJob.cancel; i++) {
    Plan candidate = listed;
    candidate.rom = i;
    Evaluate(candidate);
    const bool applies = candidate.error.empty() && !candidate.bases.empty() &&
                         (candidate.bases.size() > 1 || candidate.bases[0].patch >= 0 || !candidate.bases[0].options.empty());
    if (i == 0 || applies) {
      *plan = std::move(candidate);
    }
    if (applies || listed.patches.empty()) {
      break;
    }
  }
  return plan;
}

// --- building the game ------------------------------------------------------------------------

struct Built {
  bool ok = false;
  std::string error;
  std::string id;
  std::string info;
  bool versionsAdded = false; // versions added to a game already there (id)
  std::string installed;      // that game's own version
};

// What the randomizer is to do with the new game (the game's SIM_Prj_Randomize)
struct Randomize {
  bool on = false;
  u32 seed = 0;
  u32 options = 0;
};

// Worker: randomizes the game just extracted (built.id); on failure it is removed again
void RandomizeBuilt(const Randomize &randomize, Built &built) {
  if (!built.ok || !randomize.on) {
    return;
  }
  SetStep("Randomizing...", -1.0f);
  char summary[96] = "";
  char error[160] = "";
  const std::string dir = CartDir(built.id);
  if (!SIM_Prj_Randomize(dir.c_str(), randomize.seed, randomize.options, summary, sizeof(summary), error,
                         sizeof(error))) {
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    built.ok = false;
    built.id.clear();
    built.error = error[0] != '\0' ? error : "The game could not be randomized.";
    return;
  }
  if (summary[0] != '\0') {
    built.info += (built.info.empty() ? "" : " + ") + std::string(summary);
    WriteWholeFile(dir + "/cart.info", (const u8 *)built.info.data(), built.info.size());
  }
}

// Worker: extracts rom (patched or not) as a new game, with its versions (if any: the
// first is the game's own files)
void Extract(const std::vector<u8> *patched, const std::string &romPath, const std::string &info, Built &built,
             const std::vector<VersionFiles> &versions = {}) {
  std::string path = romPath;
  std::string temp;
  if (patched != nullptr) {
    if (!GameCodeMatches(patched->data(), patched->size())) {
      built.error = "The patched ROM is not this game.";
      return;
    }
    SetStep("Saving the patched ROM...", -1.0f);
    std::error_code error;
    fs::create_directories(BaseDir() + "/imports", error);
    temp = BaseDir() + "/imports/patched.nds";
    if (!WriteWholeFile(temp, patched->data(), patched->size())) {
      remove(temp.c_str());
      built.error = "There is not enough space to build the game.";
      return;
    }
    path = temp;
  }

  SetStep("Extracting the game files...", -1.0f);
  std::string id = NewCartId();
  std::error_code error;
  fs::create_directories(CartDir(id), error);
  int result = error ? -1 : SIM_NDSExtractTo(path, CartDir(id));
  if (!temp.empty()) {
    remove(temp.c_str());
  }
  if (result != 0 || !HasFiles(CartDir(id))) {
    fs::remove_all(CartDir(id), error);
    built.error = "The game could not be extracted (is there enough space?).";
    return;
  }
  if (!versions.empty() && !WriteVersions(CartDir(id), versions, 0)) {
    fs::remove_all(CartDir(id), error);
    built.error = "The game's versions could not be saved (is there enough space?).";
    return;
  }
  if (!info.empty()) {
    WriteWholeFile(CartDir(id) + "/cart.info", (const u8 *)info.data(), info.size());
  }
  built.ok = true;
  built.id = id;
  built.info = info;
}

struct Selection {
  int base;
  std::vector<int> options; // indices into the base's options
};

// Worker: the folder's game with the chosen extras, in memory (versions not applied)
bool PatchFromPlan(const Plan &plan, const Selection &selection, std::vector<u8> &game, std::string &info,
                   std::string &error) {
  const Base &base = plan.bases[selection.base];
  const std::string romPath = plan.roms[plan.rom];
  SetStep("Reading the ROM...", -1.0f);
  if (!ReadWholeFile(romPath, game)) {
    error = "The ROM could not be read.";
    return false;
  }
  info.clear();
  if (base.patch >= 0) {
    const Patch &patch = plan.patches[base.patch];
    SetStep("Applying " + patch.name + "...", -1.0f);
    Applied applied = Apply(game, patch.path);
    if (!applied.error.empty() || applied.mismatches > 0) {
      error = patch.name + " could not be applied.";
      return false;
    }
    game = std::move(applied.data);
    info = patch.name;
  }
  for (int index : selection.options) {
    const Option &option = base.options[index];
    const Patch &patch = plan.patches[option.patch];
    SetStep("Applying " + patch.name + "...", -1.0f);
    if (option.inPlace) {
      for (const Change &change : option.changes) {
        if ((size_t)change.offset + change.length > game.size()) {
          error = patch.name + " could not be applied.";
          return false;
        }
        memcpy(&game[change.offset], &option.pool[change.data], change.length);
      }
    } else {
      Applied applied = Apply(game, patch.path);
      if (!applied.error.empty()) {
        error = patch.name + " could not be applied.";
        return false;
      }
      game = std::move(applied.data);
    }
    info += (info.empty() ? "" : " + ") + patch.name;
  }
  return true;
}

std::vector<VersionPatch> VersionPatches(const Plan &plan, const Base &base) {
  std::vector<VersionPatch> patches;
  for (int index : base.versions) {
    patches.push_back({ plan.patches[base.options[index].patch].name, base.options[index].edits });
  }
  return patches;
}

// Worker: the folder's game with the chosen extras (and its versions)
void BuildFromPlan(const Plan &plan, const Selection &selection, Built &built) {
  const Base &base = plan.bases[selection.base];
  const std::string romPath = plan.roms[plan.rom];
  if (base.patch < 0 && selection.options.empty() && base.versions.empty()) {
    SetStep("Checking the ROM...", -1.0f);
    std::string header = ReadHeader(romPath, 16);
    if (!GameCodeMatches((const u8 *)header.data(), header.size())) {
      built.error = "That file is not a ROM of this game.";
      return;
    }
    Extract(nullptr, romPath, "", built);
    return;
  }

  std::vector<u8> game;
  std::string info;
  if (!PatchFromPlan(plan, selection, game, info, built.error)) {
    return;
  }
  std::vector<VersionFiles> versions;
  if (!base.versions.empty()) {
    if (!MakeVersionFiles(game, VersionPatches(plan, base), versions)) {
      built.error = "The game's versions could not be made.";
      return;
    }
    info = VersionsInfo(info, versions);
  }
  Extract(&game, romPath, info, built, versions);
}

// Which of the versions the game in dir is (its own files), or -1
int InstalledVersion(const std::string &dir, const std::vector<VersionFiles> &versions) {
  for (size_t v = 0; v < versions.size(); v++) {
    bool same = !versions[v].files.empty();
    for (const auto &entry : versions[v].files) {
      std::vector<u8> file;
      same = same && ReadWholeFile(dir + "/" + entry.first, file) && file == entry.second;
    }
    if (same) {
      return (int)v;
    }
  }
  return -1;
}

// Worker: the versions added to a game already there, which is one of them (its version
// files are that version's): it keeps its saves, which stay that version. Only the version
// files change, so the extras it was made with (code) stay as they are.
void AddVersionsTo(const Plan &plan, int baseIndex, const std::string &id, Built &built) {
  const Base &base = plan.bases[baseIndex];
  const std::string dir = CartDir(id);
  Selection selection;
  selection.base = baseIndex;
  std::vector<u8> game;
  std::string info;
  if (!PatchFromPlan(plan, selection, game, info, built.error)) {
    return;
  }
  SetStep("Comparing with the game you have...", -1.0f);
  std::vector<VersionFiles> versions;
  if (!MakeVersionFiles(game, VersionPatches(plan, base), versions)) {
    built.error = "The game's versions could not be made.";
    return;
  }
  const int installed = InstalledVersion(dir, versions);
  if (installed < 0) {
    built.error = "That game isn't one of these versions (it was made from other patches). Add it as a new game.";
    return;
  }
  // Its info line: what it was made with, and the versions
  SIM_Cart cart = {};
  cart.hasFiles = TRUE;
  ReadName(id, &cart);
  info = cart.info;
  for (const VersionFiles &version : versions) {
    // "RenegadePlatinum4997 + ClassicVersion" -> "RenegadePlatinum4997"
    const std::string part = " + " + version.patch;
    const size_t at = version.patch.empty() ? std::string::npos : info.find(part);
    if (at != std::string::npos) {
      info.erase(at, part.size());
    }
  }
  // A name that says which version it is ("Renegade Platinum Classic") gets the game's name
  // for it with its versions (unless it was named otherwise)
  char autoName[SIM_CART_NAME_LEN];
  SIM_Prj_CartName(dir.c_str(), autoName, sizeof(autoName));
  const bool renamed = strcmp(autoName, cart.name) != 0;

  SetStep("Saving the versions...", -1.0f);
  if (!WriteVersions(dir, versions, installed)) {
    built.error = "The versions could not be saved (is there enough space?).";
    return;
  }
  if (!renamed) {
    SIM_Prj_CartName(dir.c_str(), autoName, sizeof(autoName));
    const std::string line = std::string(autoName) + "\n";
    WriteWholeFile(dir + "/cart.name", (const u8 *)line.data(), line.size());
  }
  if (info.empty() && base.patch >= 0) {
    info = plan.patches[base.patch].name;
  }
  info = VersionsInfo(info, versions);
  WriteWholeFile(dir + "/cart.info", (const u8 *)info.data(), info.size());
  built.ok = true;
  built.id = id;
  built.info = info;
  built.versionsAdded = true;
  built.installed = versions[installed].name;
}

// Worker: a ROM and patches applied one after the other
void BuildChain(const std::string &romPath, const std::vector<Patch> &patches, Built &built) {
  if (patches.empty()) {
    std::string header = ReadHeader(romPath, 16);
    if (!GameCodeMatches((const u8 *)header.data(), header.size())) {
      built.error = "That file is not a ROM of this game.";
      return;
    }
    Extract(nullptr, romPath, "", built);
    return;
  }
  SetStep("Reading the ROM...", -1.0f);
  std::vector<u8> game;
  if (!ReadWholeFile(romPath, game)) {
    built.error = "The ROM could not be read.";
    return;
  }
  if (!GameCodeMatches(game.data(), game.size())) {
    built.error = "That ROM is not this game. Choose your Pokemon Platinum (USA) ROM.";
    return;
  }
  std::string info;
  std::vector<VersionPatch> versionPatches; // patches that only make a version (kept alongside)
  for (size_t i = 0; i < patches.size(); i++) {
    SetStep("Applying " + patches[i].name + "...", (float)i / (float)patches.size());
    Applied applied = Apply(game, patches[i].path);
    if (!applied.error.empty()) {
      built.error = patches[i].name + ": " + applied.error;
      return;
    }
    if (i == 0 && applied.mismatches > 0) {
      built.error = patches[i].name + " was made for a different version of the ROM. (Renegade Platinum: 4997 for "
                    "later carts and dumps, 3541 for early ones.)";
      return;
    }
    std::vector<Change> changes;
    std::vector<u8> pool;
    std::vector<FileEdit> edits;
    if (i > 0 && Diff(game, applied.data, changes, pool) && AsVersionEdits(game, changes, pool, edits)) {
      versionPatches.push_back({ patches[i].name, std::move(edits) });
      continue;
    }
    game = std::move(applied.data);
    info += (info.empty() ? "" : " + ") + patches[i].name;
  }
  std::vector<VersionFiles> versions;
  if (!versionPatches.empty()) {
    if (!MakeVersionFiles(game, versionPatches, versions)) {
      built.error = "The game's versions could not be made.";
      return;
    }
    info = VersionsInfo(info, versions);
  }
  Extract(&game, romPath, info, built, versions);
}

// --- the screen -------------------------------------------------------------------------------

enum class Mode { MENU, FOLDER, FILES };
enum class Waiting { NOTHING, FOLDER, FOLDER_ROM, FILES_ROM, FILES_PATCH };

bool sOpen = false;
Mode sMode = Mode::MENU;
Waiting sWaiting = Waiting::NOTHING;
void (*sDone)(BOOL ok, const char *id) = nullptr;

std::shared_ptr<Plan> sPlan;
std::string sFolder;   // the folder chosen (an imported copy on Android)
std::string sExtraRom; // a ROM chosen for a folder without one
bool sScanning = false;
int sPlanGeneration = 0; // a new plan (what's worked out from it is done again)

std::string sRom;            // files mode
std::vector<Patch> sChain;   // files mode

// The randomizer (when the game has one): off unless turned on for this game
bool sRandomize = false;
int sSeed = 0;
std::vector<bool> sRandomOptions;

std::string sMessage;
bool sFailed = false;
std::string sAddedId;
std::string sAddedName;
std::string sAddedInfo;

void ClearImports() {
  RemoveIfImported(sFolder);
  RemoveIfImported(sExtraRom);
  RemoveIfImported(sRom);
  for (const Patch &patch : sChain) {
    RemoveIfImported(patch.path);
  }
  sFolder.clear();
  sExtraRom.clear();
  sRom.clear();
  sChain.clear();
  sPlan.reset();
}

void BackToMenu() {
  ClearImports();
  sMode = Mode::MENU;
  sMessage.clear();
  sFailed = false;
}

void Close() {
  ClearImports();
  sOpen = false;
  sDone = nullptr;
  sMode = Mode::MENU;
}

void StartScan(int romIndex) {
  sScanning = true;
  sMessage.clear();
  sFailed = false;
  const std::string folder = sFolder, extraRom = sExtraRom;
  StartJob([folder, extraRom, romIndex] {
    std::shared_ptr<Plan> plan(Scan(folder, extraRom, romIndex).release());
    const bool cancelled = sJob.cancel;
    sJob.finish = [plan, cancelled] {
      sScanning = false;
      if (cancelled) {
        BackToMenu();
        return;
      }
      sPlan = plan;
      sPlanGeneration++;
    };
  });
}

void FolderPicked(const char *path, void *) {
  sWaiting = Waiting::NOTHING;
  if (path == nullptr) {
    return;
  }
  ClearImports();
  sFolder = path;
  sMode = Mode::FOLDER;
  StartScan(-1);
}

void FolderRomPicked(const char *path, void *) {
  sWaiting = Waiting::NOTHING;
  if (path == nullptr) {
    return;
  }
  RemoveIfImported(sExtraRom);
  sExtraRom = path;
  StartScan(-1);
}

void FilePicked(const char *path, void *user) {
  const bool isPatch = user != nullptr;
  sWaiting = Waiting::NOTHING;
  if (path == nullptr) {
    return;
  }
  if (isPatch) {
    sChain.push_back({ path, StripExtension(FileName(path)) });
  } else {
    RemoveIfImported(sRom);
    sRom = path;
  }
  sMessage.clear();
  sFailed = false;
}

// GUI thread: the new game is in place
void Added(const Built &built) {
  if (!built.ok) {
    sMessage = built.error;
    sFailed = true;
    return;
  }
  if (built.versionsAdded) {
    // A game already there got its versions: it keeps its saves
    SIM_Cart cart = {};
    cart.hasFiles = TRUE;
    ReadName(built.id, &cart);
    if (built.id == SIM_Carts_Current()->id) {
      LoadVersions();
    }
    sMessage = std::string(cart.name) + " now has the versions too. Its saves stay " + built.installed +
               "; starting a new game there asks which one it uses.";
    sFailed = false;
    ClearImports(); // done with the folder
    sMode = Mode::MENU;
    return;
  }
  SIM_Cart cart = {};
  cart.hasFiles = TRUE;
  ReadName(built.id, &cart);

  // Two games with one name: number the new one
  SIM_Cart carts[SIM_CARTS_MAX];
  int count = SIM_Carts_List(carts);
  int same = 0;
  for (int i = 0; i < count; i++) {
    if (built.id != carts[i].id && strcmp(carts[i].name, cart.name) == 0) {
      same++;
    }
  }
  if (same > 0) {
    char name[SIM_CART_NAME_LEN];
    snprintf(name, sizeof(name), "%.40s (%d)", cart.name, same + 1);
    std::string line = std::string(name) + "\n";
    WriteWholeFile(CartDir(built.id) + "/cart.name", (const u8 *)line.data(), line.size());
    snprintf(cart.name, sizeof(cart.name), "%s", name);
  }

  ClearImports();
  sAddedId = built.id;
  sAddedName = cart.name;
  sAddedInfo = built.info;
  sMessage.clear();
  sFailed = false;
  if (sDone != nullptr) {
    sDone(TRUE, built.id.c_str());
  }
}

Randomize CurrentRandomize() {
  Randomize randomize;
  randomize.on = sRandomize;
  randomize.seed = (u32)sSeed;
  for (size_t i = 0; i < sRandomOptions.size() && i < 32; i++) {
    if (sRandomOptions[i]) {
      randomize.options |= 1u << i;
    }
  }
  return randomize;
}

void StartBuild(const std::function<void(Built &)> &work) {
  sMessage.clear();
  sFailed = false;
  const Randomize randomize = CurrentRandomize();
  StartJob([work, randomize] {
    auto built = std::make_shared<Built>();
    work(*built);
    RandomizeBuilt(randomize, *built);
    sJob.finish = [built] { Added(*built); };
  });
}

Selection CurrentSelection() {
  Selection selection;
  selection.base = sPlan->base;
  const Base &base = sPlan->bases[selection.base];
  for (const Group &group : base.groups) {
    if (group.selected > 0) {
      selection.options.push_back(group.options[group.selected - 1]);
    }
  }
  return selection;
}

void StartFolderBuild() {
  const Selection selection = CurrentSelection();
  std::shared_ptr<Plan> plan = sPlan; // not changed while the work runs
  StartBuild([plan, selection](Built &built) { BuildFromPlan(*plan, selection, built); });
}

// The versions added to a game already there instead
void StartAddVersions(const std::string &id) {
  const int base = sPlan->base;
  std::shared_ptr<Plan> plan = sPlan;
  sMessage.clear();
  sFailed = false;
  StartJob([plan, base, id] {
    auto built = std::make_shared<Built>();
    AddVersionsTo(*plan, base, id, *built);
    sJob.finish = [built] { Added(*built); };
  });
}

// Games already added that are this one as one of its versions, without the others (they
// can get them, keeping their saves)
struct VersionMatch {
  std::string id;
  std::string name;
  std::string version;
};

std::vector<VersionMatch> FindVersionMatches(const Base &base) {
  std::vector<VersionMatch> matches;
  if (base.versionFiles.empty()) {
    return matches;
  }
  SIM_Cart carts[SIM_CARTS_MAX];
  const int count = SIM_Carts_List(carts);
  for (int i = 0; i < count; i++) {
    const std::string dir = CartDir(carts[i].id);
    if (!carts[i].hasFiles || HasVersions(dir) || strstr(carts[i].info, "Randomized") != nullptr) {
      continue;
    }
    const int v = InstalledVersion(dir, base.versionFiles);
    if (v >= 0) {
      matches.push_back({ carts[i].id, carts[i].name, base.versionFiles[v].name });
    }
  }
  return matches;
}

void StartFilesBuild() {
  const std::string rom = sRom;
  const std::vector<Patch> chain = sChain;
  StartBuild([rom, chain](Built &built) { BuildChain(rom, chain, built); });
}

void ChooseFolder() {
  char code[5];
  ExpectedGameCode(code);
  sWaiting = Waiting::FOLDER;
  SIM_Files_PickFolder("Choose the folder with the ROM and patches", code[0] != '\0' ? code : nullptr, FolderPicked,
                       nullptr);
}

const char *PickerLabel(const char *idle) {
  return sWaiting != Waiting::NOTHING ? "Waiting for the file picker..." : idle;
}

// --- screen parts

void DrawBusy() {
  std::string step;
  float fraction;
  {
    std::lock_guard<std::mutex> guard(sJob.lock);
    step = sJob.step;
    fraction = sJob.fraction;
  }
  Progress(sJob.cancel ? "Stopping..." : step.c_str(), fraction);
  Note(sScanning ? "Trying each patch on the ROM to see which ones go together. This takes a few seconds."
                 : "Building the game. This takes a few seconds.");
  if (sScanning && Button("Stop", nullptr, false, !sJob.cancel)) {
    sJob.cancel = true;
  }
}

void DrawAdded() {
  std::string text = "Added " + sAddedName + ".";
  Status(text.c_str());
  if (!sAddedInfo.empty()) {
    Note(("Patches: " + sAddedInfo).c_str());
  }
  std::string play = "Play " + sAddedName;
  if (Button(play.c_str(), PickerActive() ? nullptr : "Your game is saved first when it can be, then the app restarts.",
             true)) {
    std::string id = sAddedId;
    sAddedId.clear();
    Close();
    if (PickerActive()) {
      PickerPlay(id);
    } else {
      SIM_Carts_SwitchTo(id.c_str());
    }
  }
  SetDefaultFocus();
  if (Button(PickerActive() ? "Back to the list" : "Later", "Switch to it any time in Settings > Games.")) {
    sAddedId.clear();
    Close();
  }
}

int NewSeed() {
  static std::mt19937 generator((unsigned)std::random_device{}() ^ (unsigned)time(nullptr));
  return 1 + (int)(generator() % 99999999u);
}

// The randomizer's options for the game being added (when the game has a randomizer)
void DrawRandomizer() {
  const SIM_RandomizerOption *options = nullptr;
  const int count = SIM_Prj_RandomizerOptions(&options);
  if (count <= 0 || options == nullptr) {
    return;
  }
  if ((int)sRandomOptions.size() != count) {
    sRandomOptions.assign(count, false);
    for (int i = 0; i < count; i++) {
      sRandomOptions[i] = options[i].defaultOn != FALSE;
    }
  }
  if (sSeed <= 0) {
    sSeed = NewSeed();
  }
  Section("Randomizer");
  Toggle("Randomize it", "A new adventure: different Pokemon to meet, battle and choose from. The same seed and options "
                         "give the same game, on any device.", &sRandomize);
  if (!sRandomize) {
    return;
  }
  NumberField("Seed", "Share it to play the same randomized game as a friend.", &sSeed, 1, 99999999);
  if (Button("New seed")) {
    sSeed = NewSeed();
  }
  for (int i = 0; i < count; i++) {
    ImGui::PushID(1000 + i);
    bool on = sRandomOptions[i];
    if (Toggle(options[i].label, options[i].help, &on)) {
      sRandomOptions[i] = on;
    }
    ImGui::PopID();
  }
}

void DrawMenu() {
  if (!sMessage.empty()) {
    Status(sMessage.c_str(), sFailed);
  }
  Note("Add another game next to the ones you have, like Drayano's Renegade Platinum. Each game keeps its own saves.");
  if (Button(PickerLabel("Choose a folder"),
             "A folder with your Pokemon Platinum (USA) ROM and the patches for it (.xdelta). The app works out which "
             "patches go on the ROM and which go together.",
             false, sWaiting == Waiting::NOTHING)) {
    ChooseFolder();
  }
  SetDefaultFocus();
  if (Button("Choose the files one by one", "The ROM, then each patch in the order it goes on. Also for adding a ROM "
                                            "as it is.")) {
    sMode = Mode::FILES;
  }
  Section("Renegade Platinum");
  Note("Unzip the download from Drayano's page, put your ROM in the same folder (or choose it after the folder), and "
       "choose the folder. RenegadePlatinum4997 (3541 for early ROMs) goes on the ROM; the shiny rate and speed-up "
       "patches show up as extras. Classic comes along as a version: each new game asks Classic or Complete.");
  const SIM_RandomizerOption *options = nullptr;
  if (SIM_Prj_RandomizerOptions(&options) > 0) {
    Section("Randomizer");
    Note("Any game can be randomized as it's added, Renegade Platinum too: once the ROM (and patches) are chosen, turn "
         "on \"Randomize it\" and pick a seed.");
  }
}

void DrawFolder() {
  Plan &plan = *sPlan;
  std::string title = "Folder: " + plan.folderName;
  Section(title.c_str());

  if (!plan.error.empty()) {
    Status(plan.error.c_str(), true);
  }

  // The ROM
  if (plan.roms.empty()) {
    Status("There's no Pokemon Platinum (USA) ROM in this folder.", true);
    if (!plan.otherRoms.empty()) {
      std::string others = "Not this game: ";
      for (size_t i = 0; i < plan.otherRoms.size(); i++) {
        others += (i > 0 ? ", " : "") + plan.otherRoms[i];
      }
      Note(others.c_str());
    }
    if (!plan.patches.empty()) {
      std::string found = "Patches found: ";
      for (size_t i = 0; i < plan.patches.size(); i++) {
        found += (i > 0 ? ", " : "") + plan.patches[i].name;
      }
      found += ". Choose your ROM and they're tried on it.";
      Note(found.c_str());
    }
    if (Button(PickerLabel("Choose the ROM file"), "Your Pokemon Platinum (USA) ROM (.nds), from anywhere.", true,
               sWaiting == Waiting::NOTHING)) {
      sWaiting = Waiting::FOLDER_ROM;
      SIM_Files_PickToOpen("Choose the game ROM", ".nds,.srl", FolderRomPicked, nullptr);
    }
    SetDefaultFocus();
  } else if (plan.roms.size() > 1) {
    Section("ROM");
    for (int i = 0; i < (int)plan.roms.size(); i++) {
      ImGui::PushID(i);
      if (Card(FileName(plan.roms[i]).c_str(), i == plan.rom ? "The patches are tried on this one" : "Tap to use this "
               "one instead", i == plan.rom) && i != plan.rom) {
        StartScan(i);
      }
      ImGui::PopID();
    }
  } else {
    Card(FileName(plan.roms[0]).c_str(), "Pokemon Platinum (USA)", true);
  }

  if (!plan.roms.empty() && plan.error.empty()) {
    Base *base = plan.bases.empty() ? nullptr : &plan.bases[plan.base];
    // The game: the ROM as it is, or a patch that rebuilds it
    if (plan.bases.size() > 1) {
      std::vector<std::string> names;
      for (const Base &b : plan.bases) {
        names.push_back(BaseName(plan, b));
      }
      std::vector<const char *> options;
      for (const std::string &name : names) {
        options.push_back(name.c_str());
      }
      Section("Game");
      Choice("Which game", "These patches each turn the ROM into a different game: pick one.", &plan.base,
             options.data(), (int)options.size());
      base = &plan.bases[plan.base];
    } else if (base != nullptr && base->patch >= 0) {
      Section("Game");
      Card(plan.patches[base->patch].name.c_str(), "Matches this ROM: it goes on first", true);
    }

    // Extras
    bool alone = false; // an extra that rebuilds the game is on: nothing else can go with it
    if (base != nullptr) {
      for (const Group &group : base->groups) {
        if (group.options.size() == 1 && group.selected > 0 && !base->options[group.options[0]].inPlace) {
          alone = true;
        }
      }
    }
    if (base != nullptr && !base->groups.empty()) {
      Section("Extras");
      std::string onTop = base->patch >= 0 ? "Goes on top of " + plan.patches[base->patch].name + "."
                                           : std::string("Goes on the ROM.");
      for (size_t g = 0; g < base->groups.size(); g++) {
        Group &group = base->groups[g];
        ImGui::PushID((int)g);
        if (group.options.size() == 1) {
          const Option &option = base->options[group.options[0]];
          bool on = group.selected > 0;
          const bool enabled = on || !alone;
          std::string help = option.inPlace ? onTop : std::string("Rebuilds the game, so it can't go with the other "
                                                                  "extras.");
          if (Toggle(group.label.c_str(), help.c_str(), &on, enabled)) {
            group.selected = on ? 1 : 0;
          }
        } else {
          std::vector<const char *> options;
          for (const std::string &choice : group.choices) {
            options.push_back(choice.c_str());
          }
          Choice(group.label.c_str(), "These change the same thing: pick one, or none.", &group.selected,
                 options.data(), (int)options.size(), !alone);
        }
        ImGui::PopID();
      }
      if (alone) {
        for (Group &group : base->groups) {
          if (!(group.options.size() == 1 && !base->options[group.options[0]].inPlace)) {
            group.selected = 0;
          }
        }
      }
    } else if (plan.patches.empty()) {
      Note("There are no patches in this folder: the ROM is added as it is.");
    }

    // Versions: all of them come with the game
    if (base != nullptr && base->versionFiles.size() > 1) {
      Section("Versions");
      std::string names;
      for (size_t v = 0; v < base->versionFiles.size(); v++) {
        names += (v == 0 ? "" : v + 1 == base->versionFiles.size() ? " and " : ", ") + base->versionFiles[v].name;
      }
      std::string text = names + ": both come with the game. Each new game asks which one it uses, and a save keeps "
                         "the one it started with.";
      Note(text.c_str());

      static int sMatchedPlan = -1;
      static int sMatchedBase = -1;
      static std::vector<VersionMatch> sMatches;
      if (sMatchedPlan != sPlanGeneration || sMatchedBase != plan.base) {
        sMatchedPlan = sPlanGeneration;
        sMatchedBase = plan.base;
        sMatches = FindVersionMatches(*base);
      }
      for (size_t m = 0; m < sMatches.size(); m++) {
        ImGui::PushID(2000 + (int)m);
        std::string label = "Add them to " + sMatches[m].name;
        std::string help = "You have it already, as " + sMatches[m].version + ": it gets the other version too and "
                           "keeps its saves (they stay " + sMatches[m].version + ") and its extras.";
        if (Button(label.c_str(), help.c_str(), false, sWaiting == Waiting::NOTHING)) {
          StartAddVersions(sMatches[m].id);
          sMatchedPlan = -1;
        }
        ImGui::PopID();
      }
    }
  }

  if (!plan.unused.empty()) {
    Section("Not used");
    for (const Unused &note : plan.unused) {
      std::string text = note.name + ": " + note.reason + ".";
      Note(text.c_str());
    }
  }

  if (!plan.roms.empty() && plan.error.empty()) {
    DrawRandomizer();
  }

  Space(4.0f);
  if (!sMessage.empty()) {
    Status(sMessage.c_str(), sFailed);
  }
  const bool ready = !plan.roms.empty() && plan.error.empty() && !plan.bases.empty();
  bool patching = ready && (plan.bases[plan.base].patch >= 0 || std::any_of(plan.bases[plan.base].groups.begin(),
                                                                            plan.bases[plan.base].groups.end(),
                                                                            [](const Group &g) { return g.selected > 0; }));
  if (!plan.roms.empty()) {
    if (Button(sRandomize ? "Randomize and add" : patching ? "Patch and add" : "Add the game", nullptr, true, ready)) {
      StartFolderBuild();
    }
    if (ready) {
      SetDefaultFocus();
    }
  }
  if (Button(PickerLabel("Choose another folder"), nullptr, false, sWaiting == Waiting::NOTHING)) {
    ChooseFolder();
  }
}

void DrawFiles() {
  Section("1. The game's ROM");
  if (Card(sRom.empty() ? PickerLabel("Choose the ROM") : FileName(sRom).c_str(),
           sRom.empty() ? "Your Pokemon Platinum (USA) ROM (.nds)" : "Tap to choose another", !sRom.empty(),
           sWaiting == Waiting::NOTHING)) {
    sWaiting = Waiting::FILES_ROM;
    SIM_Files_PickToOpen("Choose the game ROM", ".nds,.srl", FilePicked, nullptr);
  }
  SetDefaultFocus();

  Section("2. Patches, in order (optional)");
  if (sChain.empty()) {
    Note("None: the ROM is added as it is.");
  }
  for (size_t i = 0; i < sChain.size(); i++) {
    std::string line = std::to_string(i + 1) + ". " + sChain[i].name;
    Note(line.c_str());
  }
  int pressed = ButtonPair(PickerLabel("Add a patch"), "Remove the last", false, sWaiting == Waiting::NOTHING,
                           !sChain.empty());
  if (pressed == 1) {
    sWaiting = Waiting::FILES_PATCH;
    SIM_Files_PickToOpen("Choose a patch", ".xdelta,.vcdiff", FilePicked, (void *)1);
  } else if (pressed == 2) {
    RemoveIfImported(sChain.back().path);
    sChain.pop_back();
  }
  Note("Renegade Platinum: your Platinum (USA) ROM, then RenegadePlatinum4997.xdelta (3541 for early ROMs). Add "
       "ClassicVersion.xdelta too and both versions come with the game: each new game asks which one.");

  DrawRandomizer();

  Space(4.0f);
  if (!sMessage.empty()) {
    Status(sMessage.c_str(), sFailed);
  }
  if (Button(sRandomize ? "Randomize and add" : sChain.empty() ? "Add the game" : "Patch and add", nullptr, true,
             !sRom.empty())) {
    StartFilesBuild();
  }
}

} // namespace

bool AddOpen() {
  return sOpen;
}

void OpenAdd(void (*done)(BOOL ok, const char *id)) {
  if (sOpen) {
    return;
  }
  sOpen = true;
  sMode = Mode::MENU;
  sWaiting = Waiting::NOTHING;
  sMessage.clear();
  sFailed = false;
  sAddedId.clear();
  sDone = done;
  sRandomize = false;
  sSeed = 0;
  sRandomOptions.clear();
}

void AddBack() {
  if (JobBusy()) {
    if (sScanning) {
      sJob.cancel = true;
    }
    return;
  }
  if (sWaiting != Waiting::NOTHING) {
    return;
  }
  if (!sAddedId.empty()) {
    sAddedId.clear();
    Close();
  } else if (sMode != Mode::MENU) {
    BackToMenu();
  } else {
    Close();
  }
}

void DrawAdd() {
  PollJob();
  if (!sOpen) {
    return;
  }
  const bool busy = JobBusy();
  const char *back = busy || sWaiting != Waiting::NOTHING ? nullptr
                   : !sAddedId.empty()                     ? "Done"
                   : sMode != Mode::MENU                   ? "Back"
                                                           : (PickerActive() ? "Games" : "Close");
  const char *title = sMode == Mode::FOLDER ? "Add a game from a folder"
                    : sMode == Mode::FILES  ? "Add a game from files"
                                            : "Add a game";
  // What's shown changed: the pad's cursor goes to its default item
  static int sLastView = -1;
  const int view = busy ? 1 : !sAddedId.empty() ? 2 : (sMode == Mode::FOLDER && sPlan) ? 3 : 4 + (int)sMode;
  if (view != sLastView) {
    RequestFocus();
    sLastView = view;
  }
  bool goBack = BeginScreen("Add a game", title, back, S(680.0f));
  if (busy) {
    DrawBusy();
  } else if (!sAddedId.empty()) {
    DrawAdded();
  } else if (sMode == Mode::FOLDER && sPlan) {
    DrawFolder();
  } else if (sMode == Mode::FOLDER) {
    Progress("Waiting for the folder...", -1.0f);
  } else if (sMode == Mode::FILES) {
    DrawFiles();
  } else {
    DrawMenu();
  }
  EndScreen();
  if (goBack && !SIM_Files_IsBusy()) {
    AddBack();
  }
}

} // namespace SIM::Carts

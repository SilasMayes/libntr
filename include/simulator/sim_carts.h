#ifndef LIBNTR_SIM_CARTS_H
#define LIBNTR_SIM_CARTS_H

// Several games side by side, like cartridges: the data directory holds the first game
// (its extracted ROM and saves) and the settings shared by all; every other game lives
// in carts/<id>/ with its own extracted ROM and saves. Before the game starts, the chosen
// game's directory becomes the working directory, so the game's files and saves resolve
// there, while settings go through SIM_SettingsPath.
//
// With more than one game a picker comes up at launch (unless turned off); games are
// added from it or from the settings: from a folder holding a ROM and patches (which
// patches go together is worked out by trying them), or from files chosen one by one.

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_CARTS_MAX 8
#define SIM_CART_NAME_LEN 48

typedef struct SIM_Cart {
    char id[32];   // "" for the game in the data directory itself
    char name[SIM_CART_NAME_LEN];
    char info[96]; // the patches it was built with ("" if none or not known)
    BOOL hasFiles; // its ROM has been extracted
} SIM_Cart;

// Game hook (weak default "Pokemon"): a display name for the ROM extracted in dir
void SIM_Prj_CartName(const char *dir, char *name, int nameSize);

// Game hooks for a randomizer on the "Add a game" screen (weak defaults: none, and the
// screen doesn't offer one). Its options are toggles there, with a seed.
typedef struct SIM_RandomizerOption {
    const char *label;
    const char *help;
    BOOL defaultOn;
} SIM_RandomizerOption;

// The options; returns how many (0: no randomizer)
int SIM_Prj_RandomizerOptions(const SIM_RandomizerOption **options);

// Randomizes the game just extracted in dir, from seed, with option i on when bit i of
// options is set. Runs on a worker thread: it may only use the files in dir (not the
// running game's). summary receives a short description for the game's info line
// ("Randomized: wild Pokemon, trainers"); error the reason when it returns FALSE.
BOOL SIM_Prj_Randomize(const char *dir, u32 seed, u32 options, char *summary, int summarySize, char *error,
                       int errorSize);

// Versions. A patch that only changes files the game keeps per save (SIM_Prj_VersionFiles;
// Renegade Platinum's Classic patch only changes the Pokemon data) isn't an extra: the game
// added holds both versions, its own files being one of them and versions/<name>/ holding
// the other's copies of those files (versions.ini lists them). The game then chooses a
// version per save: SIM_Carts_UseVersion makes the game's file opens (FS_OpenFile) go to
// that version's copies, and SIM_Carts_AskVersion asks which one a new game gets.
#define SIM_CART_VERSIONS_MAX 4
#define SIM_CART_VERSION_NAME_LEN 24

// Game hooks (weak defaults: none, and "Standard" / the patch's name without "Version"):
// the files (paths in the ROM) a version may differ in, and a version's name from the patch
// that makes it (patchName NULL: the game without one)
int SIM_Prj_VersionFiles(const char *const **paths);
void SIM_Prj_VersionName(const char *patchName, char *name, int nameSize);

// The running game's versions; 0 if it has none (only its own files). installed (may be
// NULL) receives which one its own files are.
int SIM_Carts_Versions(char names[SIM_CART_VERSIONS_MAX][SIM_CART_VERSION_NAME_LEN], int *installed);
// The version whose files the game opens from now on (NULL: the installed one)
void SIM_Carts_UseVersion(const char *name);
const char *SIM_Carts_VersionInUse(void); // "" when the game has no versions
// The file to open for path (relative, as the game opens it): the version's copy if it has one
const char *SIM_Carts_VersionFile(const char *path);

// Asks which version (one card each, with its description); the game waits meanwhile (its
// logic pauses while the question is up). Then SIM_Carts_TakeVersionAnswer: -2 while
// asking, -1 if the player went back (canCancel), else the index chosen (once).
void SIM_Carts_AskVersion(const char *title, const char *note, const char *const *names,
                          const char *const *descriptions, int count, BOOL canCancel);
int SIM_Carts_TakeVersionAnswer(void);

// Settings file in the data directory, whichever game is running
const char *SIM_SettingsPath(const char *fileName);

int SIM_Carts_List(SIM_Cart carts[SIM_CARTS_MAX]);
const SIM_Cart *SIM_Carts_Current(void);
BOOL SIM_Carts_AskAtLaunch(void);
void SIM_Carts_SetAskAtLaunch(BOOL ask);

// Opens the "Add a game" screen: the player picks a folder with a ROM and patches
// (.xdelta, e.g. Renegade Platinum's), or a ROM and patches one by one, and it is
// extracted as a new game, which the screen then offers to play. done (may be NULL) is
// called with the new game's id.
void SIM_Carts_Add(void (*done)(BOOL ok, const char *id));

// Makes id the game to start and restarts the app (Android) or quits (desktop), letting
// the game save first (SIM_LIFECYCLE_QUIT)
void SIM_Carts_SwitchTo(const char *id);

// A soft reset (the DS's L+R+Start+Select, OS_ResetSystem): restarts the app into the
// game being played, without saving and without the picker. The new process then sees
// SIM_Carts_StartedBySoftReset() == TRUE.
void SIM_Carts_SoftReset(void);
BOOL SIM_Carts_StartedBySoftReset(void);

// libntr internals
void SIM_Carts_SetArgs(int argc, char **argv); // main()'s, for restarting on desktop
void SIM_Carts_Init(void);      // first thing in main(): remembers the data directory
void SIM_Carts_Choose(void);    // once the window exists: picker, then enter the game's directory
BOOL SIM_Carts_PickerActive(void);
BOOL SIM_Carts_UIActive(void);  // the picker or the "Add a game" screen is up
void SIM_Carts_DrawUI(void);    // every frame, from the GUI
void SIM_Carts_Back(void);      // the settings key while they are up

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_CARTS_H

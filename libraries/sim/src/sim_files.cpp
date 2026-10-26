// File picking for imports/exports. See include/simulator/sim_files.h
#include <simulator/sim_files.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef SDK_BUILD_ANDROID
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <jni.h>
#else
#include "ImGuiFileDialog.h"
#include "simulator/imgui/imgui.hpp"
#endif

namespace {

enum class Kind { NONE, OPEN, SAVE };

Kind sKind = Kind::NONE;
SIM_FileOpenedFn sOpenedFn = nullptr;
SIM_FileSavedFn sSavedFn = nullptr;
void *sUser = nullptr;
std::string sSourcePath;

void Finish(const char *openedPath, bool saved) {
  Kind kind = sKind;
  SIM_FileOpenedFn openedFn = sOpenedFn;
  SIM_FileSavedFn savedFn = sSavedFn;
  void *user = sUser;

  sKind = Kind::NONE;
  sOpenedFn = nullptr;
  sSavedFn = nullptr;
  sUser = nullptr;

  if (kind == Kind::OPEN && openedFn != nullptr) {
    openedFn(openedPath, user);
  } else if (kind == Kind::SAVE && savedFn != nullptr) {
    savedFn(saved ? TRUE : FALSE, user);
  }
}

#ifdef SDK_BUILD_ANDROID

// Results come back on the Java UI thread; they are handed over to the game thread
enum Result { RESULT_NONE, RESULT_READY };
std::atomic<int> sResult{RESULT_NONE};
std::mutex sResultLock;
std::string sResultPath;
bool sResultOk = false;

bool CallActivity(const char *name, const char *signature, jstring arg1, jstring arg2) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(activityClass, name, signature);
  bool ok = false;
  if (method != nullptr) {
    if (arg2 != nullptr) {
      env->CallVoidMethod(activity, method, arg1, arg2);
    } else {
      env->CallVoidMethod(activity, method);
    }
    ok = !env->ExceptionCheck();
  }
  env->ExceptionClear();
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return ok;
}

#else

constexpr const char *kDialogKey = "SimFilesDialog";

#endif

} // namespace

extern "C" BOOL SIM_Files_IsBusy(void) {
  return sKind != Kind::NONE ? TRUE : FALSE;
}

extern "C" void SIM_Files_PickToOpen(const char *title, const char *filters, SIM_FileOpenedFn done,
                                     void *user) {
  if (sKind != Kind::NONE) {
    if (done != nullptr) {
      done(nullptr, user);
    }
    return;
  }
  sKind = Kind::OPEN;
  sOpenedFn = done;
  sUser = user;

#ifdef SDK_BUILD_ANDROID
  (void)title;
  (void)filters;
  sResult = RESULT_NONE;
  if (!CallActivity("pickDocument", "()V", nullptr, nullptr)) {
    Finish(nullptr, false);
  }
#else
  IGFD::FileDialogConfig config;
  config.path = ".";
  ImGuiFileDialog::Instance()->OpenDialog(kDialogKey, title, filters, config);
#endif
}

extern "C" void SIM_Files_SaveCopy(const char *title, const char *sourcePath, const char *suggestedName,
                                   SIM_FileSavedFn done, void *user) {
  if (sKind != Kind::NONE) {
    if (done != nullptr) {
      done(FALSE, user);
    }
    return;
  }
  sKind = Kind::SAVE;
  sSavedFn = done;
  sUser = user;
  sSourcePath = sourcePath;

#ifdef SDK_BUILD_ANDROID
  (void)title;
  sResult = RESULT_NONE;
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  bool started = false;
  if (env != nullptr) {
    jstring jSource = env->NewStringUTF(sourcePath);
    jstring jName = env->NewStringUTF(suggestedName);
    started = CallActivity("exportDocument", "(Ljava/lang/String;Ljava/lang/String;)V", jSource, jName);
    env->DeleteLocalRef(jSource);
    env->DeleteLocalRef(jName);
  }
  if (!started) {
    Finish(nullptr, false);
  }
#else
  IGFD::FileDialogConfig config;
  config.path = ".";
  config.fileName = suggestedName;
  config.flags = ImGuiFileDialogFlags_ConfirmOverwrite;
  // Filter from the suggested name's extension, so the dialog keeps it
  std::string name = suggestedName;
  size_t dot = name.rfind('.');
  std::string filter = dot != std::string::npos ? name.substr(dot) : std::string(".*");
  ImGuiFileDialog::Instance()->OpenDialog(kDialogKey, title, filter.c_str(), config);
#endif
}

#ifndef SDK_BUILD_ANDROID
static bool SimCopyFile(const std::string &from, const std::string &to) {
  FILE *in = fopen(from.c_str(), "rb");
  if (in == nullptr) {
    return false;
  }
  FILE *out = fopen(to.c_str(), "wb");
  if (out == nullptr) {
    fclose(in);
    return false;
  }
  char buffer[64 * 1024];
  size_t count;
  bool ok = true;
  while ((count = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    if (fwrite(buffer, 1, count, out) != count) {
      ok = false;
      break;
    }
  }
  fclose(in);
  if (fclose(out) != 0) {
    ok = false;
  }
  return ok;
}
#endif

extern "C" void SIM_Files_Update(void) {
  if (sKind == Kind::NONE) {
    return;
  }

#ifdef SDK_BUILD_ANDROID
  if (sResult.load() != RESULT_READY) {
    return;
  }
  std::string path;
  bool ok;
  {
    std::lock_guard<std::mutex> guard(sResultLock);
    path = sResultPath;
    ok = sResultOk;
  }
  sResult = RESULT_NONE;
  Finish(ok && !path.empty() ? path.c_str() : nullptr, ok);
#else
  ImVec2 minSize(600, 400);
  if (ImGuiFileDialog::Instance()->Display(kDialogKey, ImGuiWindowFlags_NoCollapse, minSize)) {
    bool accepted = ImGuiFileDialog::Instance()->IsOk();
    std::string chosen = accepted ? ImGuiFileDialog::Instance()->GetFilePathName() : std::string();
    ImGuiFileDialog::Instance()->Close();

    if (sKind == Kind::OPEN) {
      Finish(accepted ? chosen.c_str() : nullptr, accepted);
    } else {
      bool saved = accepted && SimCopyFile(sSourcePath, chosen);
      Finish(nullptr, saved);
    }
  }
#endif
}

#ifdef SDK_BUILD_ANDROID
// From org.pokeplatinum.port.PlatinumActivity (UI thread)
extern "C" JNIEXPORT void JNICALL Java_org_pokeplatinum_port_PlatinumActivity_nativeDocumentPicked(
    JNIEnv *env, jclass, jstring path) {
  std::lock_guard<std::mutex> guard(sResultLock);
  if (path != nullptr) {
    const char *chars = env->GetStringUTFChars(path, nullptr);
    sResultPath = chars;
    env->ReleaseStringUTFChars(path, chars);
    sResultOk = true;
  } else {
    sResultPath.clear();
    sResultOk = false;
  }
  sResult = RESULT_READY;
}

extern "C" JNIEXPORT void JNICALL Java_org_pokeplatinum_port_PlatinumActivity_nativeDocumentExported(
    JNIEnv *, jclass, jboolean ok) {
  std::lock_guard<std::mutex> guard(sResultLock);
  sResultPath.clear();
  sResultOk = ok ? true : false;
  sResult = RESULT_READY;
}
#endif

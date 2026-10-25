// Android-only glue: logging, storage location and ROM import through the
// system file picker. See include/simulator/sim_android.h.
#ifdef SDK_BUILD_ANDROID

#include <simulator/sim_android.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr const char *kLogTag = "pokeplatinum";
// Java class that owns the SDL activity (see android/app/src/main/java)
constexpr const char *kActivityClass = "org/pokeplatinum/port/PlatinumActivity";

int sLogPipe[2] = {-1, -1};

// stdout/stderr have nowhere to go on Android: forward them to logcat line by line
void *LogForwardThread(void *) {
  char buffer[1024];
  size_t used = 0;
  for (;;) {
    ssize_t count = read(sLogPipe[0], buffer + used, sizeof(buffer) - 1 - used);
    if (count <= 0) {
      break;
    }
    used += (size_t)count;
    buffer[used] = '\0';

    char *lineStart = buffer;
    char *newline;
    while ((newline = strchr(lineStart, '\n')) != nullptr) {
      *newline = '\0';
      __android_log_write(ANDROID_LOG_INFO, kLogTag, lineStart);
      lineStart = newline + 1;
    }
    used = strlen(lineStart);
    if (used == sizeof(buffer) - 1) {
      // Very long line: flush it as is
      __android_log_write(ANDROID_LOG_INFO, kLogTag, lineStart);
      used = 0;
    } else {
      memmove(buffer, lineStart, used);
    }
  }
  return nullptr;
}

enum ImportState {
  IMPORT_IDLE,
  IMPORT_WAITING,
  IMPORT_DONE,
  IMPORT_CANCELLED,
};
std::atomic<int> sImportState{IMPORT_IDLE};
std::mutex sImportLock;
std::string sImportedPath;

} // namespace

extern "C" void SIM_Android_Init(void) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  if (pipe(sLogPipe) == 0) {
    dup2(sLogPipe[1], STDOUT_FILENO);
    dup2(sLogPipe[1], STDERR_FILENO);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, LogForwardThread, nullptr) == 0) {
      pthread_detach(thread);
    }
  }

  // Back button/gesture is delivered to the game (it opens the settings GUI)
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

  // All game files (extracted ROM, saves, configs) live in the app's external files
  // directory: Android/data/<package>/files. Relative paths in libntr resolve there.
  const char *dataDir = SDL_AndroidGetExternalStoragePath();
  if (dataDir == nullptr) {
    dataDir = SDL_AndroidGetInternalStoragePath();
  }
  if (dataDir != nullptr && chdir(dataDir) == 0) {
    printf("Data directory: %s\n", dataDir);
  } else {
    printf("Could not enter the data directory: %s\n", SDL_GetError());
  }
}

extern "C" void SIM_Android_RequestRomImport(void) {
  sImportState = IMPORT_WAITING;

  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (env == nullptr || activity == nullptr) {
    sImportState = IMPORT_CANCELLED;
    return;
  }

  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(activityClass, "requestRomImport", "()V");
  if (method != nullptr) {
    env->CallVoidMethod(activity, method);
  } else {
    env->ExceptionClear();
    sImportState = IMPORT_CANCELLED;
  }
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
}

extern "C" int SIM_Android_PollRomImport(char *pathOut, size_t pathOutSize) {
  switch (sImportState.load()) {
  case IMPORT_DONE: {
    std::lock_guard<std::mutex> guard(sImportLock);
    snprintf(pathOut, pathOutSize, "%s", sImportedPath.c_str());
    sImportState = IMPORT_IDLE;
    return 1;
  }
  case IMPORT_CANCELLED:
    sImportState = IMPORT_IDLE;
    return -1;
  default:
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_pokeplatinum_port_PlatinumActivity_nativeRomImportFinished(JNIEnv *env,
                                                                    jclass,
                                                                    jstring path) {
  if (path == nullptr) {
    sImportState = IMPORT_CANCELLED;
    return;
  }
  const char *chars = env->GetStringUTFChars(path, nullptr);
  {
    std::lock_guard<std::mutex> guard(sImportLock);
    sImportedPath = chars;
  }
  env->ReleaseStringUTFChars(path, chars);
  sImportState = IMPORT_DONE;
}

#endif // SDK_BUILD_ANDROID

// Android-only glue: logging, storage location and ROM import through the
// system file picker. See include/simulator/sim_android.h.
#ifdef SDK_BUILD_ANDROID

#include <simulator/sim_android.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
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

// --- CPU speed ------------------------------------------------------------------------

namespace {

// Performance hint sessions (NDK, Android 13): looked up at run time (the app runs on 10+)
typedef void *(*GetManagerFn)();
typedef void *(*CreateSessionFn)(void *, const int32_t *, size_t, int64_t);
typedef int (*ReportWorkFn)(void *, int64_t);
ReportWorkFn sReportWork = nullptr;
void *sHintSession = nullptr;
char sCpuInfo[160] = "";

constexpr int64_t kFrameTargetNs = 16666667; // a 60 Hz frame

int ReadCpuMaxFreq(int cpu) {
  char path[96];
  snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
  FILE *file = fopen(path, "r");
  if (file == nullptr) {
    return 0;
  }
  int khz = 0;
  if (fscanf(file, "%d", &khz) != 1) {
    khz = 0;
  }
  fclose(file);
  return khz;
}

} // namespace

extern "C" void SIM_Android_GameThreadStarted(void) {
  // The fastest cores: those within 75% of the fastest one's top clock (on the Thor's
  // Snapdragon 8 Gen 2 that leaves out only the three small efficiency cores)
  const int cpuCount = (int)sysconf(_SC_NPROCESSORS_CONF);
  int freqs[64] = {};
  int fastest = 0;
  for (int cpu = 0; cpu < cpuCount && cpu < 64; cpu++) {
    freqs[cpu] = ReadCpuMaxFreq(cpu);
    fastest = freqs[cpu] > fastest ? freqs[cpu] : fastest;
  }
  char cores[64] = "";
  if (fastest > 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    int chosen = 0;
    for (int cpu = 0; cpu < cpuCount && cpu < 64; cpu++) {
      if (freqs[cpu] >= fastest * 3 / 4) {
        CPU_SET(cpu, &set);
        size_t used = strlen(cores);
        snprintf(cores + used, sizeof(cores) - used, "%s%d", chosen > 0 ? "," : "", cpu);
        chosen++;
      }
    }
    const pid_t tid = (pid_t)syscall(SYS_gettid);
    if (chosen == 0 || sched_setaffinity(tid, sizeof(set), &set) != 0) {
      snprintf(cores, sizeof(cores), "any");
    }
  } else {
    snprintf(cores, sizeof(cores), "any");
  }

  void *library = dlopen("libandroid.so", RTLD_NOW);
  GetManagerFn getManager = library ? (GetManagerFn)dlsym(library, "APerformanceHint_getManager") : nullptr;
  CreateSessionFn createSession =
      library ? (CreateSessionFn)dlsym(library, "APerformanceHint_createSession") : nullptr;
  sReportWork = library ? (ReportWorkFn)dlsym(library, "APerformanceHint_reportActualWorkDuration") : nullptr;
  if (getManager != nullptr && createSession != nullptr && sReportWork != nullptr) {
    void *manager = getManager();
    const int32_t tid = (int32_t)syscall(SYS_gettid);
    sHintSession = manager != nullptr ? createSession(manager, &tid, 1, kFrameTargetNs) : nullptr;
  }
  snprintf(sCpuInfo, sizeof(sCpuInfo), "Game thread on cores %s (top %d MHz); performance hints %s", cores,
           fastest / 1000, sHintSession != nullptr ? "on" : "not available");
  printf("%s\n", sCpuInfo);
}

extern "C" void SIM_Android_ReportFrameWork(long long workNs) {
  if (sHintSession != nullptr && workNs > 0) {
    sReportWork(sHintSession, (int64_t)workNs);
  }
}

extern "C" const char *SIM_Android_CpuInfo(void) {
  return sCpuInfo;
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

extern "C" void SIM_Android_RestartApp(void) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (env != nullptr && activity != nullptr) {
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(activityClass, "restartApp", "()V");
    if (method != nullptr) {
      env->CallVoidMethod(activity, method);
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
  }
  // restartApp ends this process; if it couldn't, quit at least
  SDL_Delay(2000);
  exit(0);
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

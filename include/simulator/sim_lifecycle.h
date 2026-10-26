#ifndef LIBNTR_SIM_LIFECYCLE_H
#define LIBNTR_SIM_LIFECYCLE_H

// Lets the game react when the app is about to be backgrounded or closed (to save, for
// example). Called on the game thread from the frame's event processing, i.e. between
// two game frames, right before the app pauses (Android) or exits (desktop window close).

#ifdef __cplusplus
extern "C" {
#endif

enum SIMLifecycleEvent {
    SIM_LIFECYCLE_BACKGROUND = 0, // app going to the background (Android home/sleep/app switch)
    SIM_LIFECYCLE_QUIT = 1,       // app about to exit
};

// Weak no-op in libntr; a game overrides it by defining the function.
void SIM_Prj_OnLifecycleEvent(int event);

#ifdef __cplusplus
}
#endif

#endif // LIBNTR_SIM_LIFECYCLE_H

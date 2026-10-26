// Default (no-op) game hook. See include/simulator/sim_lifecycle.h
#include <simulator/sim_lifecycle.h>

__attribute__((weak)) void SIM_Prj_OnLifecycleEvent(int event)
{
    (void)event;
}

// Companion UI hooks (see include/simulator/sim_companion.h): no companion by default
#include <simulator/sim_companion.h>

__attribute__((weak)) BOOL SIM_Prj_CompanionEnabled(void)
{
    return FALSE;
}

__attribute__((weak)) void SIM_Prj_CompanionSetup(float outputWidth)
{
    (void)outputWidth;
}

__attribute__((weak)) void SIM_Prj_CompanionDraw(const SIM_CompanionFrame *frame)
{
    (void)frame;
}

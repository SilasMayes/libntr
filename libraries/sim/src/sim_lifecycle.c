// Default (no-op) game hook. See include/simulator/sim_lifecycle.h
#include <simulator/sim_lifecycle.h>

__attribute__((weak)) void SIM_Prj_OnLifecycleEvent(int event)
{
    (void)event;
}

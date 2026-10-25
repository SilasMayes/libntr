#ifndef SCREENQUADS_H
#define SCREENQUADS_H
#include <nitro.h>
#include <simulator/g3_draw.h>
#include <simulator/config/sim_config.h>

#ifdef __cplusplus
extern "C" {
#endif

G3SIM_Vertex_t * sim_GetScreenQuadArray(SIM_config_screen_layout_type layout, BOOL isSwapped);
int sim_GetScreenQuadVertexCount(SIM_config_screen_layout_type layout, BOOL isSwapped);
G3SIM_Vertex_t * sim_GetSingleScreenQuadArray(BOOL bottomScreen);
int sim_GetSingleScreenQuadVertexCount(void);

#ifdef __cplusplus
}
#endif

#endif
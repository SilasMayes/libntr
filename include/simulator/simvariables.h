#include <nitro/os/common/arena.h>
#include <nitro/spi/ARM9/tp.h>

#ifdef  __cplusplus
extern "C" {
#endif

extern u32 SDK_AUTOLOAD_DTCM_START[2];

//ioreg_CP
extern REGType16v s_reg_CP_DIVCNT;
extern REGType64v s_reg_CP_DIV_NUMER;
extern REGType32v s_reg_CP_DIV_NUMER_L;
extern REGType32v s_reg_CP_DIV_NUMER_H;
extern REGType64v s_reg_CP_DIV_DENOM;
extern REGType32v s_reg_CP_DIV_DENOM_L;
extern REGType32v s_reg_CP_DIV_DENOM_H;
extern REGType64v s_reg_CP_DIV_RESULT;
extern REGType32v s_reg_CP_DIV_RESULT_L;
extern REGType32v s_reg_CP_DIV_RESULT_H;
extern REGType64v s_reg_CP_DIVREM_RESULT;
extern REGType32v s_reg_CP_DIVREM_RESULT_L;
extern REGType32v s_reg_CP_DIVREM_RESULT_H;
extern REGType16v s_reg_CP_SQRTCNT;
extern REGType32v s_reg_CP_SQRT_RESULT;
extern REGType64v s_reg_CP_SQRT_PARAM;
extern REGType32v s_reg_CP_SQRT_PARAM_L;
extern REGType32v s_reg_CP_SQRT_PARAM_H;

//ioreg_G2
extern REGType16v s_reg_G2_BG0CNT;
extern REGType16v s_reg_G2_BG1CNT;
extern REGType16v s_reg_G2_BG2CNT;
extern REGType16v s_reg_G2_BG3CNT;
extern REGType32v s_reg_G2_BG0OFS;
extern REGType16v s_reg_G2_BG0HOFS;
extern REGType16v s_reg_G2_BG0VOFS;
extern REGType32v s_reg_G2_BG1OFS;
extern REGType16v s_reg_G2_BG1HOFS;
extern REGType16v s_reg_G2_BG1VOFS;
extern REGType32v s_reg_G2_BG2OFS;
extern REGType16v s_reg_G2_BG2HOFS;
extern REGType16v s_reg_G2_BG2VOFS;
extern REGType32v s_reg_G2_BG3OFS;
extern REGType16v s_reg_G2_BG3HOFS;
extern REGType16v s_reg_G2_BG3VOFS;
extern REGType16v s_reg_G2_BG2PA;
extern REGType16v s_reg_G2_BG2PB;
extern REGType16v s_reg_G2_BG2PC;
extern REGType16v s_reg_G2_BG2PD;
extern REGType32v s_reg_G2_BG2X;
extern REGType32v s_reg_G2_BG2Y;
extern REGType16v s_reg_G2_BG3PA;
extern REGType16v s_reg_G2_BG3PB;
extern REGType16v s_reg_G2_BG3PC;
extern REGType16v s_reg_G2_BG3PD;
extern REGType32v s_reg_G2_BG3X;
extern REGType32v s_reg_G2_BG3Y;
extern REGType16v s_reg_G2_WIN0H;
extern REGType16v s_reg_G2_WIN1H;
extern REGType16v s_reg_G2_WIN0V;
extern REGType16v s_reg_G2_WIN1V;
extern REGType16v s_reg_G2_WININ;
extern REGType16v s_reg_G2_WINOUT;
extern REGType16v s_reg_G2_MOSAIC;
extern REGType16v s_reg_G2_BLDCNT;
extern REGType16v s_reg_G2_BLDALPHA;
extern REGType16v s_reg_G2_BLDY;

//ioreg_G2S
extern REGType16v s_reg_G2S_DB_BG0CNT;
extern REGType16v s_reg_G2S_DB_BG1CNT;
extern REGType16v s_reg_G2S_DB_BG2CNT;
extern REGType16v s_reg_G2S_DB_BG3CNT;
extern REGType32v s_reg_G2S_DB_BG0OFS;
extern REGType16v s_reg_G2S_DB_BG0HOFS;
extern REGType16v s_reg_G2S_DB_BG0VOFS;
extern REGType32v s_reg_G2S_DB_BG1OFS;
extern REGType16v s_reg_G2S_DB_BG1HOFS;
extern REGType16v s_reg_G2S_DB_BG1VOFS;
extern REGType32v s_reg_G2S_DB_BG2OFS;
extern REGType16v s_reg_G2S_DB_BG2HOFS;
extern REGType16v s_reg_G2S_DB_BG2VOFS;
extern REGType32v s_reg_G2S_DB_BG3OFS;
extern REGType16v s_reg_G2S_DB_BG3HOFS;
extern REGType16v s_reg_G2S_DB_BG3VOFS;
extern REGType16v s_reg_G2S_DB_BG2PA;
extern REGType16v s_reg_G2S_DB_BG2PB;
extern REGType16v s_reg_G2S_DB_BG2PC;
extern REGType16v s_reg_G2S_DB_BG2PD;
extern REGType32v s_reg_G2S_DB_BG2X;
extern REGType32v s_reg_G2S_DB_BG2Y;
extern REGType16v s_reg_G2S_DB_BG3PA;
extern REGType16v s_reg_G2S_DB_BG3PB;
extern REGType16v s_reg_G2S_DB_BG3PC;
extern REGType16v s_reg_G2S_DB_BG3PD;
extern REGType32v s_reg_G2S_DB_BG3X;
extern REGType32v s_reg_G2S_DB_BG3Y;
extern REGType16v s_reg_G2S_DB_WIN0H;
extern REGType16v s_reg_G2S_DB_WIN1H;
extern REGType16v s_reg_G2S_DB_WIN0V;
extern REGType16v s_reg_G2S_DB_WIN1V;
extern REGType16v s_reg_G2S_DB_WININ;
extern REGType16v s_reg_G2S_DB_WINOUT;
extern REGType16v s_reg_G2S_DB_MOSAIC;
extern REGType16v s_reg_G2S_DB_BLDCNT;
extern REGType16v s_reg_G2S_DB_BLDALPHA;
extern REGType16v s_reg_G2S_DB_BLDY;

//ioreg_G3
extern REGType32v s_reg_G3_MTX_MODE;
extern REGType32v s_reg_G3_MTX_PUSH;
extern REGType32v s_reg_G3_MTX_POP;
extern REGType32v s_reg_G3_MTX_STORE;
extern REGType32v s_reg_G3_MTX_RESTORE;
extern REGType32v s_reg_G3_MTX_IDENTITY;
extern REGType32v s_reg_G3_MTX_LOAD_4x4;
extern REGType32v s_reg_G3_MTX_LOAD_4x3;
extern REGType32v s_reg_G3_MTX_MULT_4x4;
extern REGType32v s_reg_G3_MTX_MULT_4x3;
extern REGType32v s_reg_G3_MTX_MULT_3x3;
extern REGType32v s_reg_G3_MTX_SCALE;
extern REGType32v s_reg_G3_MTX_TRANS;
extern REGType32v s_reg_G3_COLOR;
extern REGType32v s_reg_G3_NORMAL;
extern REGType32v s_reg_G3_TEXCOORD;
extern REGType32v s_reg_G3_VTX_16;
extern REGType32v s_reg_G3_VTX_10;
extern REGType32v s_reg_G3_VTX_XY;
extern REGType32v s_reg_G3_VTX_XZ;
extern REGType32v s_reg_G3_VTX_YZ;
extern REGType32v s_reg_G3_VTX_DIFF;
extern REGType32v s_reg_G3_POLYGON_ATTR;
extern REGType32v s_reg_G3_TEXIMAGE_PARAM;
extern REGType32v s_reg_G3_TEXPLTT_BASE;
extern REGType32v s_reg_G3_DIF_AMB;
extern REGType32v s_reg_G3_SPE_EMI;
extern REGType32v s_reg_G3_LIGHT_VECTOR;
extern REGType32v s_reg_G3_LIGHT_COLOR;
extern REGType32v s_reg_G3_SHININESS;
extern REGType32v s_reg_G3_BEGIN_VTXS;
extern REGType32v s_reg_G3_END_VTXS;
extern REGType32v s_reg_G3_SWAP_BUFFERS;
extern REGType32v s_reg_G3_VIEWPORT;
extern REGType32v s_reg_G3_BOX_TEST;
extern REGType32v s_reg_G3_POS_TEST;
extern REGType32v s_reg_G3_VEC_TEST;

//ioreg_G3X
extern REGType16v s_reg_G3X_DISP3DCNT;
extern REGType16v s_reg_G3X_RDLINES_COUNT;
// The eight edge colours are one block, as on the DS: G3X_SetEdgeColorTable copies all 16 bytes
// from EDGE_COLOR_0_L's address
extern REGType32v s_reg_G3X_EDGE_COLOR[4];
extern REGType16v s_reg_G3X_ALPHA_TEST_REF;
extern REGType32v s_reg_G3X_CLEAR_COLOR;
extern REGType16v s_reg_G3X_CLEAR_DEPTH;
extern REGType16v s_reg_G3X_CLRIMAGE_OFFSET;
extern REGType32v s_reg_G3X_FOG_COLOR;
extern REGType16v s_reg_G3X_FOG_OFFSET;
extern REGType32v s_reg_G3X_FOG_TABLE_0;
extern REGType16v s_reg_G3X_FOG_TABLE_0_L; 
extern REGType16v s_reg_G3X_FOG_TABLE_0_H;
extern REGType32v s_reg_G3X_FOG_TABLE_1;
extern REGType16v s_reg_G3X_FOG_TABLE_1_L;
extern REGType16v s_reg_G3X_FOG_TABLE_1_H;
extern REGType32v s_reg_G3X_FOG_TABLE_2;
extern REGType16v s_reg_G3X_FOG_TABLE_2_L;
extern REGType16v s_reg_G3X_FOG_TABLE_2_H;
extern REGType32v s_reg_G3X_FOG_TABLE_3;
extern REGType16v s_reg_G3X_FOG_TABLE_3_L;
extern REGType16v s_reg_G3X_FOG_TABLE_3_H;
extern REGType32v s_reg_G3X_FOG_TABLE_4;
extern REGType16v s_reg_G3X_FOG_TABLE_4_L;
extern REGType16v s_reg_G3X_FOG_TABLE_4_H;
extern REGType32v s_reg_G3X_FOG_TABLE_5;
extern REGType16v s_reg_G3X_FOG_TABLE_5_L;
extern REGType16v s_reg_G3X_FOG_TABLE_5_H;
extern REGType32v s_reg_G3X_FOG_TABLE_6;
extern REGType16v s_reg_G3X_FOG_TABLE_6_L;
extern REGType16v s_reg_G3X_FOG_TABLE_6_H;
extern REGType32v s_reg_G3X_FOG_TABLE_7;
extern REGType16v s_reg_G3X_FOG_TABLE_7_L;
extern REGType16v s_reg_G3X_FOG_TABLE_7_H;
extern REGType32v s_reg_G3X_TOON_TABLE_0;
extern REGType16v s_reg_G3X_TOON_TABLE_0_L;
extern REGType16v s_reg_G3X_TOON_TABLE_0_H;
extern REGType32v s_reg_G3X_TOON_TABLE_1;
extern REGType16v s_reg_G3X_TOON_TABLE_1_L;
extern REGType16v s_reg_G3X_TOON_TABLE_1_H;
extern REGType32v s_reg_G3X_TOON_TABLE_2;
extern REGType16v s_reg_G3X_TOON_TABLE_2_L;
extern REGType16v s_reg_G3X_TOON_TABLE_2_H;
extern REGType32v s_reg_G3X_TOON_TABLE_3;
extern REGType16v s_reg_G3X_TOON_TABLE_3_L;
extern REGType16v s_reg_G3X_TOON_TABLE_3_H;
extern REGType32v s_reg_G3X_TOON_TABLE_4;
extern REGType16v s_reg_G3X_TOON_TABLE_4_L;
extern REGType16v s_reg_G3X_TOON_TABLE_4_H;
extern REGType32v s_reg_G3X_TOON_TABLE_5;
extern REGType16v s_reg_G3X_TOON_TABLE_5_L;
extern REGType16v s_reg_G3X_TOON_TABLE_5_H;
extern REGType32v s_reg_G3X_TOON_TABLE_6;
extern REGType16v s_reg_G3X_TOON_TABLE_6_L;
extern REGType16v s_reg_G3X_TOON_TABLE_6_H;
extern REGType32v s_reg_G3X_TOON_TABLE_7;
extern REGType16v s_reg_G3X_TOON_TABLE_7_L;
extern REGType16v s_reg_G3X_TOON_TABLE_7_H;
extern REGType32v s_reg_G3X_TOON_TABLE_8;
extern REGType16v s_reg_G3X_TOON_TABLE_8_L;
extern REGType16v s_reg_G3X_TOON_TABLE_8_H;
extern REGType32v s_reg_G3X_TOON_TABLE_9;
extern REGType16v s_reg_G3X_TOON_TABLE_9_L;
extern REGType16v s_reg_G3X_TOON_TABLE_9_H;
extern REGType32v s_reg_G3X_TOON_TABLE_10;
extern REGType16v s_reg_G3X_TOON_TABLE_10_L;
extern REGType16v s_reg_G3X_TOON_TABLE_10_H;
extern REGType32v s_reg_G3X_TOON_TABLE_11;
extern REGType16v s_reg_G3X_TOON_TABLE_11_L;
extern REGType16v s_reg_G3X_TOON_TABLE_11_H;
extern REGType32v s_reg_G3X_TOON_TABLE_12;
extern REGType16v s_reg_G3X_TOON_TABLE_12_L;
extern REGType16v s_reg_G3X_TOON_TABLE_12_H;
extern REGType32v s_reg_G3X_TOON_TABLE_13;
extern REGType16v s_reg_G3X_TOON_TABLE_13_L;
extern REGType16v s_reg_G3X_TOON_TABLE_13_H;
extern REGType32v s_reg_G3X_TOON_TABLE_14;
extern REGType16v s_reg_G3X_TOON_TABLE_14_L;
extern REGType16v s_reg_G3X_TOON_TABLE_14_H;
extern REGType32v s_reg_G3X_TOON_TABLE_15;
extern REGType16v s_reg_G3X_TOON_TABLE_15_L;
extern REGType16v s_reg_G3X_TOON_TABLE_15_H;
extern REGType32v s_reg_G3X_GXFIFO;
extern REGType32v s_reg_G3X_GXSTAT;
extern REGType16v s_reg_G3X_LISTRAM_COUNT;
extern REGType16v s_reg_G3X_VTXRAM_COUNT;
extern REGType16v s_reg_G3X_DISP_1DOT_DEPTH;
extern REGType32v s_reg_G3X_POS_RESULT_X;
extern REGType32v s_reg_G3X_POS_RESULT_Y;
extern REGType32v s_reg_G3X_POS_RESULT_Z;
extern REGType32v s_reg_G3X_POS_RESULT_W;
extern REGType16v s_reg_G3X_VEC_RESULT_X;
extern REGType16v s_reg_G3X_VEC_RESULT_Y;
extern REGType16v s_reg_G3X_VEC_RESULT_Z;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_0;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_1;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_2;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_3;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_4;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_5;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_6;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_7;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_8;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_9;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_10;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_11;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_12;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_13;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_14;
extern REGType32v s_reg_G3X_CLIPMTX_RESULT_15;
extern REGType32v s_reg_G3X_VECMTX_RESULT_0;
extern REGType32v s_reg_G3X_VECMTX_RESULT_1;
extern REGType32v s_reg_G3X_VECMTX_RESULT_2;
extern REGType32v s_reg_G3X_VECMTX_RESULT_3;
extern REGType32v s_reg_G3X_VECMTX_RESULT_4;
extern REGType32v s_reg_G3X_VECMTX_RESULT_5;
extern REGType32v s_reg_G3X_VECMTX_RESULT_6;
extern REGType32v s_reg_G3X_VECMTX_RESULT_7;
extern REGType32v s_reg_G3X_VECMTX_RESULT_8;


//ioreg_GX
extern REGType32v s_reg_GX_DISPCNT;
extern REGType16v s_reg_GX_DISPSTAT;
extern REGType16v s_reg_GX_VCOUNT;
extern REGType32v s_reg_GX_DISPCAPCNT;
extern REGType32v s_reg_GX_DISP_MMEM_FIFO;
extern REGType16v s_reg_GX_DISP_MMEM_FIFO_L;
extern REGType16v s_reg_GX_DISP_MMEM_FIFO_H;
extern REGType16v s_reg_GX_MASTER_BRIGHT;
extern REGType16v s_reg_GX_TVOUTCNT;
extern REGType32v s_reg_GX_VRAMCNT;
extern REGType8v s_reg_GX_VRAMCNT_A;
extern REGType8v s_reg_GX_VRAMCNT_B;
extern REGType8v s_reg_GX_VRAMCNT_C;
extern REGType8v s_reg_GX_VRAMCNT_D;
extern REGType32v s_reg_GX_WVRAMCNT;
extern REGType8v s_reg_GX_VRAMCNT_E;
extern REGType8v s_reg_GX_VRAMCNT_F;
extern REGType8v s_reg_GX_VRAMCNT_G;
extern REGType8v s_reg_GX_VRAMCNT_WRAM;
extern REGType16v s_reg_GX_VRAM_HI_CNT;
extern REGType8v s_reg_GX_VRAMCNT_H;
extern REGType8v s_reg_GX_VRAMCNT_I;
extern REGType16v s_reg_GX_POWCNT;

//ioreg_GXS
extern REGType32v s_reg_GXS_DB_DISPCNT;
extern REGType16v s_reg_GXS_DB_MASTER_BRIGHT;

//ioreg_MI
extern REGType32v s_reg_MI_DMA0SAD;
extern REGType32v s_reg_MI_DMA0DAD;
extern REGType32v s_reg_MI_DMA0CNT;
extern REGType32v s_reg_MI_DMA1SAD;
extern REGType32v s_reg_MI_DMA1DAD;
extern REGType32v s_reg_MI_DMA1CNT;
extern REGType32v s_reg_MI_DMA2SAD;
extern REGType32v s_reg_MI_DMA2DAD;
extern REGType32v s_reg_MI_DMA2CNT;
extern REGType32v s_reg_MI_DMA3SAD;
extern REGType32v s_reg_MI_DMA3DAD;
extern REGType32v s_reg_MI_DMA3CNT;
extern REGType32v s_reg_MI_DMA0_CLR_DATA;
extern REGType32v s_reg_MI_DMA1_CLR_DATA;
extern REGType32v s_reg_MI_DMA2_CLR_DATA;
extern REGType32v s_reg_MI_DMA3_CLR_DATA;
extern REGType16v s_reg_MI_MCCNT0;
extern REGType16v s_reg_MI_MCD0;
extern REGType32v s_reg_MI_MCD1;
extern REGType32v s_reg_MI_MCCNT1;
extern REGType32v s_reg_MI_MCCMD0;
extern REGType32v s_reg_MI_MCCMD1;
extern REGType16v s_reg_MI_EXMEMCNT;

//ioreg_OS
extern REGType16v s_reg_OS_TM0CNT_L;
extern REGType16v s_reg_OS_TM0CNT_H;
extern REGType16v s_reg_OS_TM1CNT_L;
extern REGType16v s_reg_OS_TM1CNT_H;
extern REGType16v s_reg_OS_TM2CNT_L;
extern REGType16v s_reg_OS_TM2CNT_H;
extern REGType16v s_reg_OS_TM3CNT_L;
extern REGType16v s_reg_OS_TM3CNT_H;
extern REGType16v s_reg_OS_IME;
extern REGType32v s_reg_OS_IE;
extern REGType32v s_reg_OS_IF;
extern REGType16v s_reg_OS_PAUSE;

//ioreg_PAD
extern REGType16v s_reg_PAD_KEYINPUT;
extern REGType16v s_reg_PAD_KEYCNT;

//ioreg_PXI
extern REGType16v s_reg_PXI_SUBPINTF;
extern REGType16v s_reg_PXI_SUBP_FIFO_CNT;
extern REGType32v s_reg_PXI_SEND_FIFO;
extern REGType32v s_reg_PXI_RECV_FIFO;

//ioreg_SND
extern REGType16v s_reg_SND_POWCNT;
extern REGType32v s_reg_SND_SOUND0CNT;
extern REGType16v  s_reg_SND_SOUND0CNT_VOL_16;
extern REGType8v s_reg_SND_SOUND0CNT_VOL;
extern REGType8v s_reg_SND_SOUND0CNT_PAN;
extern REGType8v s_reg_SND_SOUND0CNT_8;
extern REGType32v s_reg_SND_SOUND0SAD;
extern REGType16v s_reg_SND_SOUND0TMR;
extern REGType16v s_reg_SND_SOUND0RPT_PT;
extern REGType32v s_reg_SND_SOUND0RPT_LEN;
extern REGType16v s_reg_SND_SOUNDCNT;
extern REGType8v s_reg_SND_SOUNDCNT_8;
extern REGType8v s_reg_SND_SOUNDCNT_VOL;
extern REGType16v s_reg_SND_SNDCAPCNT;
extern REGType8v s_reg_SND_SNDCAP0CNT;
extern REGType8v s_reg_SND_SNDCAP1CNT;
extern REGType32v s_reg_SND_SNDCAP0DAD;
extern REGType16v s_reg_SND_SNDCAP0LEN;
extern REGType32v s_reg_SND_SNDCAP1DAD;
extern REGType16v s_reg_SND_SNDCAP1LEN;

//ioreg_SPI
extern REGType16v s_reg_SPI_SPICNT;
extern REGType16v s_reg_SPI_SPID;

//mmap_shared
extern u8 s_HW_MAIN_MEM_SYSTEM[HW_MAIN_MEM_SYSTEM_SIZE];
extern u8 s_HW_ARENA_INFO_BUF[sizeof(OSArenaInfo)];
extern u8 s_HW_MAIN_MEM_SUB[0x400000];

//mmap_global
extern u8 s_HW_MAIN_MEM[0x800000];
extern u8 s_HW_MAIN_MEM_EX[0x800000];
extern u8 s_HW_BG_PLTT[0x200];
extern u8 s_HW_OBJ_PLTT[0x200];
extern u8 s_HW_DB_BG_PLTT[0x200];
extern u8 s_HW_DB_OBJ_PLTT[0x200];
extern u8 s_HW_BG_VRAM[0x80000];
extern u8 s_HW_DB_BG_VRAM[0x20000];
extern u8 s_HW_OBJ_VRAM[0x40000];
extern u8 s_HW_DB_OBJ_VRAM[0x20000];
extern u8 s_HW_LCDC_VRAM[0xA4000];
extern u8 s_HW_OAM[0x400];
extern u8 s_HW_DB_OAM[0x400];
extern u8 s_HW_CTRDG_ROM[0x20000];
extern u8 s_HW_EXT_WRAM[0x40000];
extern u8 s_HW_PRV_WRAM[0x10000];

//mmap_tcm
extern u32 s_HW_EXCP_VECTOR_BUF;
extern u32 s_HW_INTR_CHECK_BUF;


//TP
extern TPData s_tpData;

//Wireless
extern u8 s_HW_WIRELESS_INTF0[0x8000];
extern u8 s_HW_WIRELESS_INTF1[0x8000];

#if (SDK_VERSION_MAJOR == 5) && defined(SDK_TWL)
//MMAP TWL
extern u8 s_HW_TWL_MAIN_MEM[0x01000000];
extern u8 s_HW_TWL_MAIN_MEM_EX[0x01000000];
#endif


#ifdef  __cplusplus
}
#endif
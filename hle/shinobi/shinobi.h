/* shinobi.h - internal interface of the Shinobi HLE (the hle/shinobi sources).
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include "../hle.h"
#include "../seam.h"

/* sys.c */
void shinobi_frame(sh4_ctx *c);           /* one guest frame elapsed */
/* hw.c: the hardware registers game code touches directly */
void shinobi_hw_init(void);
/* vsync.c: the VBlank callback the game registers */
void shinobi_run_vblank(sh4_ctx *c);
/* chain.c: handlers the game registers for interrupt sources */
void shinobi_run_vblank_callbacks(sh4_ctx *c);
/* memory.c: the syMalloc heap over a guest RAM region */
void shinobi_memory_init(void);
/* gdfs.c: the GD-ROM file system over the data root */
void shinobi_gdfs_init(void);
/* timer.c: syTmr up-counter, 1.28 counts per microsecond */
uint32_t shinobi_timer_count(void);
/* memory.c */
uint32_t shinobi_heap_peak(void);
/* chain.c */
uint32_t shinobi_chain_add(uint32_t type, uint32_t fn, uint32_t prio, uint32_t arg);
int shinobi_chain_delete(uint32_t handle);

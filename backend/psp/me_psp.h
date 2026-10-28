/* me_psp.h - the Media Engine from the game's side (me_psp.c).
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

/* Boots the ME into fn(arg) through sa1me.prx (MODE 3), on a stack of its
 * own; 0, or -1 when the module cannot be loaded or the boot fails.
 * fn runs on the ME: it may only call code that makes no system calls and
 * touches only the memory it is handed, and it must keep its caches
 * coherent itself (me_dcache_wbinv_all). */
int me_psp_boot(const char *prx_path, void (*fn)(void *), void *arg);
/* Holds the ME in reset again and unloads the module (the ME must be idle). */
void me_psp_halt(void);
/* What the ME's exception handler recorded (sa1me.c ME_EXC): 0 if none. */
int me_psp_exception(uint32_t *cause, uint32_t *epc, uint32_t *badvaddr);

/* ME side: write back and invalidate the ME's whole D-cache. */
void me_dcache_wbinv_all(void);

/* Uncached aliases of a main-RAM address: the game's view (user segment)
 * and the ME's (kseg1). */
#define ME_UNCACHED_MAIN(p) ((void *)((uint32_t)(p) | 0x40000000u))
#define ME_UNCACHED_ME(p) ((void *)(((uint32_t)(p) & 0x1FFFFFFFu) | 0xA0000000u))

/* --me-test 3: the ME runs a probe of the game's code and memory. */
void me_psp_probe(const char *prx_path);

/* chain.c - interrupt-handler chains (syChain): the game registers VBlank
 * and timer callbacks here; the frame loop runs them at frame boundaries.
 * There is no asynchronous preemption (README section 4).
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"

#define CHAIN_MAX 16

typedef struct { uint32_t type, fn, prio, arg; int used; } chain_entry;
static chain_entry chain[CHAIN_MAX];

/* Registers fn for interrupt source type; returns a handle (index + 1) or 0. */
uint32_t shinobi_chain_add(uint32_t type, uint32_t fn, uint32_t prio, uint32_t arg) {
    for (int i = 0; i < CHAIN_MAX; i++) {
        if (chain[i].used) continue;
        chain[i].type = type; chain[i].fn = fn; chain[i].prio = prio; chain[i].arg = arg; chain[i].used = 1;
        sa1_log("chain: handler 0x%08x registered for source 0x%x (prio %u, arg 0x%08x)", fn, type, prio, arg);
        return (uint32_t)i + 1;
    }
    sa1_log("chain: table full, handler 0x%08x for source 0x%x dropped", fn, type);
    return 0;
}

int shinobi_chain_delete(uint32_t handle) {
    if (handle == 0 || handle > CHAIN_MAX || !chain[handle - 1].used) return -1;
    chain[handle - 1].used = 0;
    return 0;
}

static int in_callbacks;

void shinobi_run_vblank_callbacks(sh4_ctx *c) {
    if (in_callbacks) return;          /* a callback that waits for vsync must not recurse */
    in_callbacks = 1;
    for (int i = 0; i < CHAIN_MAX; i++) {
        if (!chain[i].used) continue;
        sh4_ctx saved = *c;
        hle_call_guest(c, chain[i].fn, chain[i].arg, 0, 0, 0);
        *c = saved;                    /* callbacks run as interrupts: caller state is preserved */
    }
    in_callbacks = 0;
}

/* 0x8C64594C: syChainAddHandler(source, function, priority, argument).
 * The SDK keeps 256 handler records and threads them per interrupt source
 * (the RTC piece at 0x8C10BA48 registers 0x8C10BA80 on source 0x900).
 * r0 = a handle for syChainDeleteHandler, or -1 when the table is full. */
void sdk_8c64594c(sh4_ctx *c) {
    HLE_ENTER(0x8c64594cu, "syChainAddHandler");
    uint32_t h = shinobi_chain_add(ARG0, ARG1, ARG2, ARG3);
    RET(h ? h : (uint32_t)-1);
}

/* 0x8C645C54: syChainDeleteHandler(handle).  r0 = 0, or -1 if unknown. */
void sdk_8c645c54(sh4_ctx *c) {
    HLE_ENTER(0x8c645c54u, "syChainDeleteHandler");
    RET(shinobi_chain_delete(ARG0));
}

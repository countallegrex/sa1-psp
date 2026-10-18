/* memory.c - syMalloc: the Katana heap, kept inside guest RAM because the
 * pointers it hands out are stored and dereferenced by game code.
 *
 * The game creates one heap: syMallocInit(0x8CF00000, 1 MB) from its system
 * init (0x8C010180).  The SDK's own layout (32-byte block headers, an
 * address-ordered circular free list) is not reproduced; the game never
 * reads the headers, only the pointers.  Blocks are 32-byte aligned as the
 * SDK's are, and the bookkeeping lives natively.
 *
 * Entries: 0x8C602128 syMallocInit(area, size), 0x8C6021D6 syMalloc(size)
 * (0 when exhausted), 0x8C602288 syFree(ptr) (NULL ignored).
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"
#include "../state.h"

#define BLOCK_MAX 1024
#define ALIGN 32u

typedef struct { uint32_t addr, size; int used; } heap_block;   /* sorted by addr */
static heap_block blocks[BLOCK_MAX];
static uint32_t nblocks;
static uint32_t heap_base, heap_size, heap_used, heap_peak;

void shinobi_memory_init(void) { nblocks = 0; heap_base = heap_size = 0; }

static void heap_init(uint32_t area, uint32_t size) {
    heap_base = (area + ALIGN - 1) & ~(ALIGN - 1);
    heap_size = (size - (heap_base - area)) & ~(ALIGN - 1);
    nblocks = 1;
    blocks[0].addr = heap_base; blocks[0].size = heap_size; blocks[0].used = 0;
    heap_used = heap_peak = 0;
    sa1_log("syMalloc: heap 0x%08x, %u bytes", heap_base, heap_size);
}

/* Like the SDK, a block is carved from the END of the first free block that
 * fits: the game stages whole files at the heap's base (0x8CF00000) while
 * only a few SDK blocks are live near the top, so the layout matters. */
static uint32_t heap_alloc(uint32_t size) {
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    if (size == 0) size = ALIGN;
    for (uint32_t i = 0; i < nblocks; i++) {
        heap_block *b = &blocks[i];
        if (b->used || b->size < size) continue;
        if (b->size > size) {
            if (nblocks >= BLOCK_MAX) { sa1_log("syMalloc: block table full"); return 0; }
            for (uint32_t j = nblocks; j > i + 1; j--) blocks[j] = blocks[j - 1];
            nblocks++;
            b->size -= size;                        /* the free remainder stays below */
            blocks[i + 1].addr = b->addr + b->size; blocks[i + 1].size = size; blocks[i + 1].used = 1;
            b = &blocks[i + 1];
        } else {
            b->used = 1;
        }
        heap_used += size;
        if (heap_used > heap_peak) heap_peak = heap_used;
        return b->addr;
    }
    return 0;
}

static int heap_free(uint32_t addr) {
    for (uint32_t i = 0; i < nblocks; i++) {
        heap_block *b = &blocks[i];
        if (b->addr != addr || !b->used) continue;
        b->used = 0;
        heap_used -= b->size;
        /* coalesce with the next block, then the previous one */
        if (i + 1 < nblocks && !blocks[i + 1].used) {
            b->size += blocks[i + 1].size;
            for (uint32_t j = i + 1; j + 1 < nblocks; j++) blocks[j] = blocks[j + 1];
            nblocks--;
        }
        if (i > 0 && !blocks[i - 1].used) {
            blocks[i - 1].size += b->size;
            for (uint32_t j = i; j + 1 < nblocks; j++) blocks[j] = blocks[j + 1];
            nblocks--;
        }
        return 0;
    }
    return -1;
}

uint32_t shinobi_heap_peak(void) { return heap_peak; }
uint32_t shinobi_heap_used(void) { return heap_used; }
/* Native allocation from the same heap (the SDK took its vertex buffers
 * from it too); returns 0 when exhausted. */
uint32_t shinobi_malloc(uint32_t size) {
    uint32_t p = heap_alloc(size);
    if (!p) sa1_log("shinobi_malloc(%u) failed: %u of %u bytes in use", size, heap_used, heap_size);
    return p;
}
void shinobi_free(uint32_t addr) { if (addr) heap_free(addr); }

void sdk_8c602128(sh4_ctx *c) {
    HLE_ENTER(0x8c602128u, "syMallocInit");
    heap_init(ARG0, ARG1);
}

void sdk_8c6021d6(sh4_ctx *c) {
    HLE_ENTER(0x8c6021d6u, "syMalloc");
    uint32_t p = heap_alloc(ARG0);
    if (!p) sa1_log("syMalloc(%u) failed: %u of %u bytes in use", ARG0, heap_used, heap_size);
    RET(p);
}

void sdk_8c602288(sh4_ctx *c) {
    HLE_ENTER(0x8c602288u, "syFree");
    if (ARG0 && heap_free(ARG0) != 0) sa1_log("syFree(0x%08x): not a live block", ARG0);
}

void shinobi_memory_state(hle_state_io *io) {
    hle_state_blob(io, "heap.blocks", blocks, sizeof blocks);
    hle_state_blob(io, "heap.n", &nblocks, sizeof nblocks);
    hle_state_blob(io, "heap.base", &heap_base, sizeof heap_base);
    hle_state_blob(io, "heap.size", &heap_size, sizeof heap_size);
    hle_state_blob(io, "heap.used", &heap_used, sizeof heap_used);
    hle_state_blob(io, "heap.peak", &heap_peak, sizeof heap_peak);
}

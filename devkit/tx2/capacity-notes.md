# tx2 capacity notes (Fable, 2026-10-28, stopped on the orchestrator's order before any result)

Question: why the card EBOOT (XMB launch) gets 6 texture chunks and the PSPLink PRX 9.
Numbers: at "mem (start)" (heap in use 0 KB) the largest free block is 30656 KB on the card vs 34752 KB
under PSPLink (same EBOOT image 18.7 MB, same heap in use 22987 KB at boot; boot largest free 7680 vs
11776 KB). Exactly 4096 KB of user memory are missing on the XMB launch.

Subagent (Opus) state when stopped: no edits, no builds; worktree private\tx2f\wt clean. Its last lead:
it had found the ARK-4 CFW source and was reading high_mem.c / syspatch.c, i.e. the CFW's high-memory
(large memory) handling for XMB-launched homebrew vs what PSPLink's loader sets up. Other candidates not
yet quantified: the EBOOT's module flags (PSP_LARGE_MEMORY / PSP_HEAP_SIZE_KB in backend/psp/main_psp.c),
a launcher or kernel module (sa1exc.prx via kubridge; --no-crash-screen under PSPLink) holding a 4 MB
partition. A fallback if the gap cannot be closed: take the pool's heap chunks at draw_init, before the
game's allocations fragment the heap (only 5 + EDRAM fit at 7680 KB free, so this alone does not reach 9).
Step B (private\tx1\stepB-nomip.diff: no generated mips where the DC has none, ~25 % less pool) is the
other lever; desk-only so far.

/* backtrace_win.c - host stack trace for the trap hook (debug aid, desktop
 * only).  Recompiled functions are named after their guest address
 * (fn_8c0xxxxx), so a host backtrace is a guest call stack.
 *
 * Part of sa1-psp (MIT). */
#include "sh4_rt.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

static void sym_init(void) {
    static int init;
    if (!init) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        init = 1;
    }
}

/* Writes the function-execution trace: "name first_frame hits" per line. */
static void trace_line(void *fn, uint32_t first, uint32_t hits, void *arg) {
    FILE *f = (FILE *)arg;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)fn, &disp, sym))
        fprintf(f, "%s %u %u\n", sym->Name, first, hits);
    else
        fprintf(f, "%p %u %u\n", fn, first, hits);
}

void plat_trace_dump(const char *path) {
    sym_init();
    FILE *f = fopen(path, "w");
    if (!f) return;
    sa1_trace_dump(trace_line, f);
    fclose(f);
}

/* Watchdog: a thread that, when the guest frame counter has not moved for
 * `seconds`, suspends the main thread and walks its stack (a pure guest
 * loop that polls memory makes no HLE call, so nothing else would report
 * it), then exits the process. */
static HANDLE main_thread;
static uint32_t (*wd_frame)(void);
static DWORD wd_seconds;

static void walk_thread(HANDLE th) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &ctx)) { sa1_log("watchdog: GetThreadContext failed"); return; }
    STACKFRAME64 fr;
    memset(&fr, 0, sizeof fr);
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    fr.AddrPC.Offset = ctx.Rip; fr.AddrPC.Mode = AddrModeFlat;
    fr.AddrFrame.Offset = ctx.Rbp; fr.AddrFrame.Mode = AddrModeFlat;
    fr.AddrStack.Offset = ctx.Rsp; fr.AddrStack.Mode = AddrModeFlat;
    HANDLE proc = GetCurrentProcess();
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sa1_log("watchdog: main thread stack:");
    for (int i = 0; i < 48; i++) {
        if (!StackWalk64(machine, proc, th, &fr, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        if (fr.AddrPC.Offset == 0) break;
        DWORD64 disp = 0;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(proc, fr.AddrPC.Offset, &disp, sym))
            sa1_log("  #%-2d %s+0x%llx", i, sym->Name, (unsigned long long)disp);
        else
            sa1_log("  #%-2d 0x%llx", i, (unsigned long long)fr.AddrPC.Offset);
    }
}

static DWORD WINAPI watchdog_main(LPVOID p) {
    (void)p;
    uint32_t last = wd_frame();
    for (;;) {
        Sleep(wd_seconds * 1000);
        uint32_t now = wd_frame();
        if (now != last) { last = now; continue; }
        sa1_log("watchdog: no frame for %lu s (frame %u); the game is spinning", (unsigned long)wd_seconds, now);
        SuspendThread(main_thread);
        sym_init();
        walk_thread(main_thread);
        sa1_seam_report(30);
        ExitProcess(3);
    }
    return 0;
}

void plat_watchdog_start(uint32_t (*frame)(void), unsigned seconds) {
    wd_frame = frame; wd_seconds = seconds;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog_main, NULL, 0, NULL);
}

void plat_backtrace(void) {
    HANDLE proc = GetCurrentProcess();
    sym_init();
    void *frames[64];
    USHORT n = CaptureStackBackTrace(0, 64, frames, NULL);
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sa1_log("host backtrace (%u frames):", (unsigned)n);
    for (USHORT i = 0; i < n; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        DWORD64 disp = 0;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(proc, addr, &disp, sym))
            sa1_log("  #%-2u %s+0x%llx", (unsigned)i, sym->Name, (unsigned long long)disp);
        else
            sa1_log("  #%-2u 0x%llx", (unsigned)i, (unsigned long long)addr);
    }
}
#else
void plat_backtrace(void) { sa1_log("host backtrace: not available on this platform"); }
void plat_trace_dump(const char *path) { (void)path; }
void plat_watchdog_start(uint32_t (*frame)(void), unsigned seconds) { (void)frame; (void)seconds; }
#endif

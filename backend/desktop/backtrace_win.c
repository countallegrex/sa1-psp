/* backtrace_win.c - host stack trace for the trap hook (debug aid, desktop
 * only).  Recompiled functions are named after their guest address
 * (fn_8c0xxxxx), so a host backtrace is a guest call stack.
 *
 * Part of sa1-psp (MIT). */
#include "sh4_rt.h"

#include <stdio.h>

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
#endif

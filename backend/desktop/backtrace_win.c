/* backtrace_win.c - host stack trace for the trap hook (debug aid, desktop
 * only).  Recompiled functions are named after their guest address
 * (fn_8c0xxxxx), so a host backtrace is a guest call stack.
 *
 * Part of sa1-psp (MIT). */
#include "sh4_rt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

void plat_backtrace(void) {
    static int init;
    HANDLE proc = GetCurrentProcess();
    if (!init) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, NULL, TRUE);
        init = 1;
    }
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
#endif

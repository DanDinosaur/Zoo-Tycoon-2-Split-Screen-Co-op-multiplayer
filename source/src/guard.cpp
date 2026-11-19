#include <windows.h>
#include <psapi.h>
#include "guard.h"
#include "log.h"

namespace {
    // JPOG34: a small stack of jump buffers per thread, so GUARDED() may nest (JPOG33 had ONE buffer: an inner
    // GUARDED overwrote the outer one, e.g. Attack -> NearestCb -> Dead).
    constexpr int kMaxDepth = 8;
    thread_local jmp_buf     t_jb[kMaxDepth];
    thread_local const char* t_what[kMaxDepth] = {};
    thread_local int         t_depth = 0;

    void Where(void* addr, char* out, size_t n) {
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &m) && m) {
            char name[MAX_PATH]{}; GetModuleFileNameA(m, name, MAX_PATH);
            const char* b = strrchr(name, '\\'); b = b ? b + 1 : name;
            wsprintfA(out, "%s+0x%X", b, (unsigned)((BYTE*)addr - (BYTE*)m));
        } else wsprintfA(out, "%p", addr);
        (void)n;
    }

    LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (t_depth > 0 && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                            code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO)) {
            char w[300]; Where(ep->ExceptionRecord->ExceptionAddress, w, sizeof w);
            const int d = (t_depth > kMaxDepth ? kMaxDepth : t_depth) - 1;
            LOG("guard: FAULT %08lX in [%s] at %s (addr %p, ecx=%08lX)", code, t_what[d] ? t_what[d] : "?",
                w, (void*)ep->ExceptionRecord->ExceptionInformation[1], ep->ContextRecord->Ecx);
            longjmp(t_jb[d], 1);
        }
        // ZT19: the 11-18 crash left no CRASH line (the game / SafeDisc replaces our filter). Log the first few faults
        // outside our guards (first chance) - the last one before the log stops is the crash.
        if (t_depth == 0 && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                             code == EXCEPTION_INT_DIVIDE_BY_ZERO)) {
            static volatile LONG s_n = 0;
            DWORD ea = (DWORD)ep->ExceptionRecord->ExceptionAddress;
            HMODULE me = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Where, &me);
            HMODULE at = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ea, &at);
            // ZT19k: any module but the system ones (KernelBase / ntdll / kernel32 = IsBadReadPtr probes): a crash in the
            // sound (mss32) or the driver must show up too
            char an[MAX_PATH] = ""; if (at) GetModuleFileNameA(at, an, MAX_PATH);
            const char* bn = strrchr(an, '\\'); bn = bn ? bn + 1 : an;
            bool sys = !lstrcmpiA(bn, "KernelBase.dll") || !lstrcmpiA(bn, "ntdll.dll") || !lstrcmpiA(bn, "kernel32.dll");
            bool ours = !sys || code == EXCEPTION_STACK_OVERFLOW; (void)me;
            static volatile LONG s_min = 0; LONG mi = (LONG)(GetTickCount() / 60000);
            if (InterlockedExchange(&s_min, mi) != mi) s_n = 0;   // at most 12 a minute
            if (ours && InterlockedIncrement(&s_n) <= 12) {
                char w[300]; Where(ep->ExceptionRecord->ExceptionAddress, w, sizeof w);
                LOG("guard: first-chance %08lX at %s (data %p) thread %lu eax=%08lX ecx=%08lX esi=%08lX edi=%08lX esp=%08lX", code, w,
                    (void*)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId(), ep->ContextRecord->Eax, ep->ContextRecord->Ecx,
                    ep->ContextRecord->Esi, ep->ContextRecord->Edi, ep->ContextRecord->Esp);
                DWORD* sp = (DWORD*)ep->ContextRecord->Esp; char st[200]; int k = 0;
                MEMORY_BASIC_INFORMATION mb{}; bool rd = VirtualQuery(sp, &mb, sizeof mb) && mb.State == MEM_COMMIT &&
                    (BYTE*)(sp + 24) <= (BYTE*)mb.BaseAddress + mb.RegionSize;
                for (int i = 0; rd && i < 24 && k < 180; ++i) { DWORD v = sp[i];
                    if (v >= 0x401000 && v < 0xa92000) k += wsprintfA(st + k, " %08lx", v); }
                st[k] = 0; LOG("guard:   stack code refs:%s", st);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
    LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep) {
        char w[300]; Where(ep->ExceptionRecord->ExceptionAddress, w, sizeof w);
        LOG("CRASH: exception %08lX at %s (data %p) eax=%08lX ecx=%08lX esp=%08lX",
            ep->ExceptionRecord->ExceptionCode, w, (void*)ep->ExceptionRecord->ExceptionInformation[1],
            ep->ContextRecord->Eax, ep->ContextRecord->Ecx, ep->ContextRecord->Esp);
        return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace guard {
void Install() {
    AddVectoredExceptionHandler(1, Veh);
    g_prevFilter = SetUnhandledExceptionFilter(Unhandled);
}
jmp_buf* Enter(const char* what) {
    int d = t_depth < kMaxDepth ? t_depth : kMaxDepth - 1;   // deeper than 8 shares the last buffer (never happens)
    t_what[d] = what; ++t_depth; return &t_jb[d];
}
void Leave() { if (t_depth > 0) --t_depth; }
void Rearm() {   // ZT19: put our crash logger back on top if the game replaced it (chains to theirs)
    LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(Unhandled);
    if (old != Unhandled) { g_prevFilter = old; static bool s = false; if (!s) { s = true; LOG("guard: crash logger re-armed (the game had replaced it)"); } }
}
}

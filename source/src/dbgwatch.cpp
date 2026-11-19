#include <windows.h>
#include "dbgwatch.h"
#include "log.h"

namespace {
    constexpr DWORD kArmCode = 0xE0D8A001, kDisarmCode = 0xE0D8A002;
    volatile DWORD g_addr = 0; float g_until = -1.f;
    DWORD g_seen[128][2]; int g_n = 0; DWORD g_hits = 0;
    HMODULE g_self = nullptr;
    bool Ours(DWORD a) {
        MEMORY_BASIC_INFORMATION mi{};
        return VirtualQuery((void*)a, &mi, sizeof mi) && mi.AllocationBase == (void*)g_self;
    }
    LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        CONTEXT* c = ep->ContextRecord;
        if (code == kArmCode) {
            c->Dr0 = g_addr; c->Dr7 = 0x000F0001;   // L0, read/write, 4 bytes
            c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code == kDisarmCode) {
            c->Dr0 = 0; c->Dr7 = 0; c->Dr6 = 0; c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code == EXCEPTION_SINGLE_STEP && (c->Dr6 & 1)) {
            ++g_hits;
            DWORD eip = c->Eip;
            if (!Ours(eip)) {
                DWORD ret = 0, ret2 = 0;
                MEMORY_BASIC_INFORMATION mi{};
                if (c->Ebp > 0x10000 && VirtualQuery((void*)c->Ebp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                    ret = *(DWORD*)(c->Ebp + 4);
                    DWORD e2 = *(DWORD*)c->Ebp;
                    if (e2 > 0x10000 && VirtualQuery((void*)e2, &mi, sizeof mi) && mi.State == MEM_COMMIT) ret2 = *(DWORD*)(e2 + 4);
                }
                bool seen = false;
                for (int i = 0; i < g_n && !seen; ++i) seen = g_seen[i][0] == eip && g_seen[i][1] == ret;
                if (!seen && g_n < 128) { g_seen[g_n][0] = eip; g_seen[g_n][1] = ret; ++g_n;
                    LOG("watch: %08lx touched by eip %08lx <- %08lx <- %08lx", g_addr, eip, ret, ret2); }
            }
            c->Dr6 = 0; c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace dbgwatch {
void Install() {
    static bool s = false; if (s) return; s = true;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Install, &g_self);
    AddVectoredExceptionHandler(1, Veh);
}
namespace { HANDLE g_main = nullptr; volatile LONG g_setOn = 0;
DWORD WINAPI SetDr(void*) {   // WOW64 ignores Dr changes made in a VEH: set them from another thread
    if (SuspendThread(g_main) == (DWORD)-1) { LOG("watch: suspend failed %lu", GetLastError()); return 0; }
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    BOOL ok = GetThreadContext(g_main, &c);
    if (g_setOn) { c.Dr0 = g_addr; c.Dr7 = 0x000F0001; } else { c.Dr0 = 0; c.Dr7 = 0; }
    c.Dr6 = 0; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    BOOL ok2 = ok && SetThreadContext(g_main, &c);
    ResumeThread(g_main);
    LOG("watch: debug registers %s (get %d set %d err %lu)", g_setOn ? "set" : "cleared", ok, ok2, GetLastError());
    return 0;
} }
void Arm(void* addr, float now) {
    Install();
    if (!g_main) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_addr = (DWORD)addr; g_n = 0; g_hits = 0; g_until = now + 3.f;
    LOG("watch: armed on %p for 3 s", addr);
    g_setOn = 1; HANDLE t = CreateThread(nullptr, 0, SetDr, nullptr, 0, nullptr); if (t) CloseHandle(t);
}
void Tick(float now) {
    if (g_until > 0 && now > g_until) {
        g_until = -1.f;
        g_setOn = 0; HANDLE t = CreateThread(nullptr, 0, SetDr, nullptr, 0, nullptr); if (t) CloseHandle(t);
        LOG("watch: done, %lu hits, %d distinct game addresses", g_hits, g_n);
    }
}
}

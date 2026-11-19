#include <windows.h>
#include <cstring>
#include "hook.h"
#include "log.h"
namespace hook {
bool Install(unsigned addr, const unsigned char* expect, int len, void* fn, void** tramp, const char* name) {
    BYTE* p = (BYTE*)addr;
    if (IsBadReadPtr(p, len) || memcmp(p, expect, len) != 0) {
        LOG("hook %s @%08x: prologue mismatch (%02x %02x %02x %02x %02x %02x) - not hooked", name, addr, p[0], p[1], p[2], p[3], p[4], p[5]);
        return false;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, p, len); t[len] = 0xE9; *(int*)(t + len + 1) = (int)((p + len) - (t + len + 5));
    DWORD op; VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &op);
    *tramp = t;
    p[0] = 0xE9; *(int*)(p + 1) = (int)((BYTE*)fn - (p + 5));
    for (int i = 5; i < len; ++i) p[i] = 0x90;
    VirtualProtect(p, len, op, &op); FlushInstructionCache(GetCurrentProcess(), p, len);
    LOG("hook %s @%08x installed", name, addr);
    return true;
}
}

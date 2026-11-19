#include <windows.h>
#include <cstring>
#include <cstdio>
#include "sndwatch.h"
#include "log.h"

namespace {
    typedef int (__stdcall* Start_t)(void*);
    Start_t o2d = nullptr, o3d = nullptr;
    volatile LONG g_n2d = 0, g_n3d = 0;
    DWORD g_until = 0; char g_what[64] = "";
    void Note(bool three) {
        LONG n = InterlockedIncrement(three ? &g_n3d : &g_n2d);
        if (GetTickCount() < g_until) LOG("sound: %s sample started (#%ld) after '%s'", three ? "3D" : "2D", n, g_what);
    }
    int __stdcall H2d(void* s) { Note(false); return o2d(s); }
    typedef void (__stdcall* Pos_t)(void*, float*, float*, float*);
    typedef void (__stdcall* Dist_t)(void*, float*, float*);
    typedef void (__stdcall* SetDist_t)(void*, float, float);
    Pos_t gPos = nullptr; Dist_t gDist = nullptr; SetDist_t sDist = nullptr;
    int __stdcall H3d(void* s) {
        Note(true);
        if (GetTickCount() < g_until && s) {
            // a ridden animal's call / attack: the game positions it at the animal, but the listener is the overhead camera
            // far above (ZT2's own camera rules), so it was too quiet to hear. Make it audible at any distance.
            float x = 0, y = 0, z = 0, mx = 0, mn = 0;
            if (gPos) gPos(s, &x, &y, &z);
            if (gDist) gDist(s, &mx, &mn);
            if (sDist) sDist(s, 4000.f, 400.f);
            LOG("sound: 3D call at %.1f %.1f %.1f, distances max %.1f min %.1f -> max 4000 min 400", x, y, z, mx, mn);
        }
        return o3d(s);
    }
    // length of the common prologue instructions (enough to move >= 5 bytes); 0 = unknown
    int Len(const BYTE* p) {
        switch (p[0]) {
        case 0x55: case 0x53: case 0x56: case 0x57: case 0x51: case 0x50: case 0x52: return 1;
        case 0x6A: return 2; case 0x68: case 0xA1: return 5;
        case 0x33: return 2;
        case 0x66: return (p[1] == 0xFF && p[2] == 0x05) ? 7 : 0;   // inc word ptr [abs32] (Miles API lock counter)
        case 0x83: return p[1] == 0xEC ? 3 : 0;
        case 0x81: return p[1] == 0xEC ? 6 : 0;
        case 0x8B:
            if (p[1] == 0xEC || p[1] == 0xFF || (p[1] >= 0xC0)) return 2;
            if ((p[1] & 0xC7) == 0x44 && p[2] == 0x24) return 4;   // mov r, [esp+disp8]
            if ((p[1] & 0xC0) == 0x40 && (p[1] & 7) != 4) return 3; // mov r, [r+disp8]
            if ((p[1] & 0xC7) == 0x05) return 6;                   // mov r, [imm32]
            return 0;
        }
        return 0;
    }
    bool Hook(const char* name, void* fn, void** orig) {
        HMODULE m = GetModuleHandleA("mss32.dll"); if (!m) { LOG("sound: mss32.dll not loaded"); return false; }
        BYTE* p = (BYTE*)GetProcAddress(m, name); if (!p) { LOG("sound: %s not exported", name); return false; }
        int n = 0;
        while (n < 5) { int l = Len(p + n); if (!l) { LOG("sound: %s prologue %02x %02x %02x %02x %02x %02x %02x %02x unknown - not watched", name,
                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); return false; } n += l; }
        BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); if (!t) return false;
        memcpy(t, p, n); t[n] = 0xE9; *(int*)(t + n + 1) = (int)((p + n) - (t + n + 5));
        DWORD op; VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &op);
        *orig = t; p[0] = 0xE9; *(int*)(p + 1) = (int)((BYTE*)fn - (p + 5)); for (int i = 5; i < n; ++i) p[i] = 0x90;
        VirtualProtect(p, n, op, &op); FlushInstructionCache(GetCurrentProcess(), p, n);
        LOG("sound: watching %s", name);
        return true;
    }
}
namespace sndwatch {
void Install() {
    static bool done = false; if (done) return; done = true;
    Hook("_AIL_start_sample@4", (void*)H2d, (void**)&o2d);
    Hook("_AIL_start_3D_sample@4", (void*)H3d, (void**)&o3d);
    if (HMODULE m = GetModuleHandleA("mss32.dll")) {
        gPos = (Pos_t)GetProcAddress(m, "_AIL_3D_position@16");
        gDist = (Dist_t)GetProcAddress(m, "_AIL_3D_sample_distances@12");
        sDist = (SetDist_t)GetProcAddress(m, "_AIL_set_3D_sample_distances@12");
    }
}
void Watch(const char* what, float secs) { snprintf(g_what, sizeof g_what, "%s", what); g_until = GetTickCount() + (DWORD)(secs * 1000); }
}
namespace sndwatch { long Count3D() { return g_n3d; } }

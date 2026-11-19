// d3d9.dll proxy: forwards every export to the real system d3d9.dll and wraps Direct3DCreate9
// so the mod can hook the device (Present = per-frame tick on the game thread).
#include <windows.h>
#include <d3d9.h>
#include "mod.h"
namespace {
    HMODULE g_real = nullptr;
    FARPROC Real(const char* n) {
        if (!g_real) {
            char p[MAX_PATH]; UINT k = GetSystemDirectoryA(p, MAX_PATH);   // WoW64 -> SysWOW64
            if (k && k < MAX_PATH - 12) { lstrcatA(p, "\\d3d9.dll"); g_real = LoadLibraryA(p); }
        }
        return g_real ? GetProcAddress(g_real, n) : nullptr;
    }
}
extern "C" {
IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    typedef IDirect3D9* (WINAPI* P)(UINT); static P p = (P)Real("Direct3DCreate9");
    IDirect3D9* d = p ? p(sdk) : nullptr;
    mod::OnD3DCreated(d);
    return d;
}
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    typedef HRESULT (WINAPI* P)(UINT, IDirect3D9Ex**); static P p = (P)Real("Direct3DCreate9Ex");
    return p ? p(sdk, out) : E_FAIL;
}
int WINAPI D3DPERF_BeginEvent(DWORD c, LPCWSTR n) { typedef int (WINAPI* P)(DWORD, LPCWSTR); static P p = (P)Real("D3DPERF_BeginEvent"); return p ? p(c, n) : 0; }
int WINAPI D3DPERF_EndEvent() { typedef int (WINAPI* P)(); static P p = (P)Real("D3DPERF_EndEvent"); return p ? p() : 0; }
DWORD WINAPI D3DPERF_GetStatus() { typedef DWORD (WINAPI* P)(); static P p = (P)Real("D3DPERF_GetStatus"); return p ? p() : 0; }
BOOL WINAPI D3DPERF_QueryRepeatFrame() { typedef BOOL (WINAPI* P)(); static P p = (P)Real("D3DPERF_QueryRepeatFrame"); return p ? p() : FALSE; }
void WINAPI D3DPERF_SetMarker(DWORD c, LPCWSTR n) { typedef void (WINAPI* P)(DWORD, LPCWSTR); static P p = (P)Real("D3DPERF_SetMarker"); if (p) p(c, n); }
void WINAPI D3DPERF_SetOptions(DWORD o) { typedef void (WINAPI* P)(DWORD); static P p = (P)Real("D3DPERF_SetOptions"); if (p) p(o); }
void WINAPI D3DPERF_SetRegion(DWORD c, LPCWSTR n) { typedef void (WINAPI* P)(DWORD, LPCWSTR); static P p = (P)Real("D3DPERF_SetRegion"); if (p) p(c, n); }
void* WINAPI Direct3DShaderValidatorCreate9() { typedef void* (WINAPI* P)(); static P p = (P)Real("Direct3DShaderValidatorCreate9"); return p ? p() : nullptr; }
}

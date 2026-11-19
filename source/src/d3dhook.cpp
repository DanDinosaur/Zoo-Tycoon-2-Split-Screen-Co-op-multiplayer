// Device vtable hooks. Present drives the mod; the other hooks only log during a one-frame trace,
// so the render path of the protected zt.exe can be found from return addresses.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include "d3dhook.h"
#include "mod.h"
#include "log.h"
#include "config.h"

namespace {
    void** g_devVt = nullptr;
    IDirect3DDevice9* g_device = nullptr;
    HWND   g_hwnd = nullptr;
    int    g_w = 0, g_h = 0;
    volatile LONG g_traceReq = 0;
    bool   g_trace = false;
    int    g_traceLines = 0;

    typedef HRESULT (WINAPI* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    typedef HRESULT (WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    typedef HRESULT (WINAPI* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT (WINAPI* V0_t)(IDirect3DDevice9*);
    typedef HRESULT (WINAPI* Clear_t)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
    typedef HRESULT (WINAPI* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    typedef HRESULT (WINAPI* SetViewport_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);
    typedef HRESULT (WINAPI* SetRT_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    typedef HRESULT (WINAPI* DIP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    typedef HRESULT (WINAPI* DP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    typedef HRESULT (WINAPI* DPUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    typedef HRESULT (WINAPI* DIPUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
    typedef HRESULT (WINAPI* VSC_t)(IDirect3DDevice9*, UINT, const float*, UINT);

    CreateDevice_t oCreateDevice; Present_t oPresent; Reset_t oReset; V0_t oBegin, oEnd;
    Clear_t oClear; SetTransform_t oSetTransform; SetViewport_t oSetViewport; SetRT_t oSetRT;
    DIP_t oDIP; DP_t oDP; DPUP_t oDPUP; DIPUP_t oDIPUP; VSC_t oVSC;

    void* Patch(void** vt, int idx, void* fn) {
        void* old = vt[idx];
        if (old == fn) return nullptr;   // already ours (caller keeps its original)
        DWORD op; VirtualProtect(&vt[idx], 4, PAGE_EXECUTE_READWRITE, &op);
        vt[idx] = fn; VirtualProtect(&vt[idx], 4, op, &op);
        return old;
    }

    // Game return addresses on the stack (zt.exe code range), e.g. "47a1b2<-49c0d0<-..."
    void Callers(void* frame, char* out, int n) {
        out[0] = 0; int len = 0, found = 0;
        DWORD* sp = (DWORD*)frame;
        for (int i = 0; i < 300 && found < 7; ++i) {
            DWORD v;
            if (IsBadReadPtr(sp + i, 4)) break;
            v = sp[i];
            if (v >= 0x401000 && v < 0x8b8000) { len += snprintf(out + len, n - len, found ? "<%06lx" : "%06lx", v); ++found; }
        }
    }
    volatile LONG g_cnt[8];   // 0 Begin 1 End 2 Clear 3 DIP 4 Present 5 SCPresent
    #define TRACE(fmt, ...) do { if (g_trace && g_traceLines < 6000) { char cs[128]; Callers(__builtin_frame_address(0), cs, sizeof cs); \
        LOG("T " fmt " | %s", ##__VA_ARGS__, cs); ++g_traceLines; } } while (0)

    HRESULT WINAPI hBegin(IDirect3DDevice9* d) { InterlockedIncrement(&g_cnt[0]); TRACE("BeginScene"); return oBegin(d); }
    HRESULT WINAPI hEnd(IDirect3DDevice9* d) { InterlockedIncrement(&g_cnt[1]); TRACE("EndScene"); return oEnd(d); }
    HRESULT WINAPI hClear(IDirect3DDevice9* d, DWORD c, const D3DRECT* r, DWORD f, D3DCOLOR col, float z, DWORD s) { InterlockedIncrement(&g_cnt[2]);
        TRACE("Clear n=%lu flags=%lx col=%08lx", c, f, col); return oClear(d, c, r, f, col, z, s); }
    HRESULT WINAPI hSetTransform(IDirect3DDevice9* d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) {
        if (g_trace && m && (t == D3DTS_VIEW || t == D3DTS_PROJECTION))
            TRACE("SetTransform %d [%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]", (int)t,
                m->_11,m->_12,m->_13,m->_14,m->_21,m->_22,m->_23,m->_24,m->_31,m->_32,m->_33,m->_34,m->_41,m->_42,m->_43,m->_44);
        return oSetTransform(d, t, m); }
    HRESULT WINAPI hSetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
        if (v) TRACE("SetViewport %lu,%lu %lux%lu z%.2f-%.2f", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
        return oSetViewport(d, v); }
    HRESULT WINAPI hSetRT(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
        if (g_trace) { D3DSURFACE_DESC sd{}; if (s) s->GetDesc(&sd); TRACE("SetRenderTarget %lu %p %ux%u fmt=%d", i, s, sd.Width, sd.Height, (int)sd.Format); }
        return oSetRT(d, i, s); }
    HRESULT WINAPI hDIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT pc) { InterlockedIncrement(&g_cnt[3]);
        TRACE("DIP t=%d nv=%u pc=%u", (int)t, nv, pc); return oDIP(d, t, b, mi, nv, si, pc); }
    HRESULT WINAPI hDP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT pc) { TRACE("DP t=%d pc=%u", (int)t, pc); return oDP(d, t, s, pc); }
    HRESULT WINAPI hDPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT st) { TRACE("DPUP t=%d pc=%u stride=%u", (int)t, pc, st); return oDPUP(d, t, pc, v, st); }
    HRESULT WINAPI hDIPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void* i, D3DFORMAT f, const void* v, UINT st) {
        TRACE("DIPUP t=%d nv=%u pc=%u", (int)t, nv, pc); return oDIPUP(d, t, mi, nv, pc, i, f, v, st); }
    HRESULT WINAPI hVSC(IDirect3DDevice9* d, UINT r, const float* f, UINT n) {
        if (g_trace && f && r < 8) TRACE("VSConstF r=%u n=%u [%.3f %.3f %.3f %.3f]", r, n, f[0], f[1], f[2], f[3]);
        return oVSC(d, r, f, n); }

    IDirect3DDevice9* g_dev = nullptr;
    typedef HRESULT (WINAPI* SCPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
    SCPresent_t oSCPresent;
    void FrameStart(IDirect3DDevice9* d, const char* via) {
        static const char* logged = nullptr;
        if (logged != via) { logged = via; LOG("d3d: frames presented via %s", via); }
        if (g_trace) { LOG("T ---- frame end (%d lines) ----", g_traceLines); g_trace = false; }
        mod::OnFrame(d, g_hwnd);
    }
    void FrameEnd() {
        if (InterlockedExchange(&g_traceReq, 0)) { g_trace = true; g_traceLines = 0; LOG("T ---- trace of one frame ----"); }
    }
    HRESULT WINAPI hPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
        InterlockedIncrement(&g_cnt[4]); FrameStart(d, "device Present");
        HRESULT hr = oPresent(d, a, b, w, r);
        FrameEnd(); return hr;
    }
    HRESULT WINAPI hSCPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
        InterlockedIncrement(&g_cnt[5]);
        IDirect3DDevice9* d = nullptr;
        if (SUCCEEDED(sc->GetDevice(&d)) && d) { FrameStart(d, "swap chain Present"); d->Release(); }
        HRESULT hr = oSCPresent(sc, a, b, w, r, f);
        FrameEnd(); return hr;
    }
    void ForceWindowed(D3DPRESENT_PARAMETERS* pp, HWND w) {
        if (!pp || !config::Get().windowed || pp->Windowed) return;
        pp->Windowed = TRUE; pp->FullScreen_RefreshRateInHz = 0;
        if (pp->BackBufferFormat == D3DFMT_UNKNOWN || pp->BackBufferFormat == D3DFMT_X8R8G8B8 || pp->BackBufferFormat == D3DFMT_A8R8G8B8) pp->BackBufferFormat = D3DFMT_UNKNOWN;
        if (w) {
            RECT r = {0, 0, (LONG)pp->BackBufferWidth, (LONG)pp->BackBufferHeight};
            DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
            SetWindowLongA(w, GWL_STYLE, st); SetWindowLongA(w, GWL_EXSTYLE, 0);
            AdjustWindowRect(&r, st, FALSE);
            SetWindowPos(w, HWND_NOTOPMOST, 40, 40, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        LOG("d3d: forcing windowed mode %ux%u", pp->BackBufferWidth, pp->BackBufferHeight);
    }
    HRESULT WINAPI hReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
        ForceWindowed(pp, g_hwnd);
        mod::OnDeviceLost();
        HRESULT hr = oReset(d, pp);
        if (pp) { g_w = pp->BackBufferWidth; g_h = pp->BackBufferHeight; }
        LOG("d3d: Reset -> %08lx  %dx%d windowed=%d", hr, g_w, g_h, pp ? pp->Windowed : -1);
        return hr;
    }

    IDirect3DDevice9* g_hookedDev = nullptr;
    void HookDevice(IDirect3DDevice9* dev);
    const char* ModOf(void* a, char* buf) {
        HMODULE m = nullptr; buf[0] = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
            char n[MAX_PATH]; GetModuleFileNameA(m, n, MAX_PATH); const char* b = strrchr(n, '\\'); snprintf(buf, 96, "%s+%lx", b ? b + 1 : n, (DWORD)((char*)a - (char*)m)); }
        else snprintf(buf, 96, "%p(no module)", a);
        return buf;
    }
    DWORD WINAPI StatsThread(void*) {
        for (int i = 0; ; ++i) {
            for (int k = 0; k < 60; ++k) { Sleep(50); if (g_hookedDev && *(void***)g_hookedDev != g_devVt) HookDevice(g_hookedDev); }
            if (i >= 20) continue;
            LOG("d3d stats: begin=%ld end=%ld clear=%ld dip=%ld present=%ld scpresent=%ld", g_cnt[0], g_cnt[1], g_cnt[2], g_cnt[3], g_cnt[4], g_cnt[5]); }
        return 0;
    }
    // Windows system DLLs are hot-patchable: "mov edi,edi" + 5 padding bytes before the function.
    // Hooking the code (not one object's vtable) catches every device/swap chain the game makes.
    bool HotPatch(void* fn, void* hook, void** orig) {
        BYTE* p = (BYTE*)fn;
        if (IsBadReadPtr(p - 5, 7)) return false;
        if (p[0] == 0xEB && p[1] == 0xF9 && p[-5] == 0xE9) {   // already hot-patched (by us earlier or another tool)
            BYTE* tgt = p + *(int*)(p - 4);
            if (tgt == (BYTE*)hook) return true;
            LOG("d3d: %p already hot-patched by someone else -> %p", p, tgt); return false;
        }
        if (p[0] == 0x6A && p[2] == 0xB8) {   // "push imm8; mov eax, imm32" (SEH-prolog functions): move 7 bytes to a trampoline
            BYTE* tr = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!tr) return false;
            memcpy(tr, p, 7); tr[7] = 0xE9; *(int*)(tr + 8) = (int)((p + 7) - (tr + 12));
            DWORD op; VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &op);
            *orig = tr;
            p[0] = 0xE9; *(int*)(p + 1) = (int)((BYTE*)hook - (p + 5)); p[5] = 0x90; p[6] = 0x90;
            VirtualProtect(p, 7, op, &op); FlushInstructionCache(GetCurrentProcess(), p, 7);
            return true;
        }
        if (!(p[0] == 0x8B && p[1] == 0xFF)) return false;
        for (int i = 1; i <= 5; ++i) if (p[-i] != 0xCC && p[-i] != 0x90) return false;
        DWORD op; VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &op);
        *orig = p + 2;
        p[-5] = 0xE9; *(int*)(p - 4) = (int)((BYTE*)hook - p);
        *(volatile WORD*)p = 0xF9EB;
        VirtualProtect(p - 5, 7, op, &op);
        FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
        return true;
    }
    void* g_target[128];
    template <class T> void P(void** vt, int i, T& orig, void* fn) {
        if (orig) {
            if (vt[i] != g_target[i] && vt[i] != fn) LOG("d3d: slot %d of another vtable uses different code %p (hooked %p) - not hooked", i, vt[i], g_target[i]);
            return;
        }
        void* target = vt[i]; g_target[i] = target;
        if (HotPatch(target, fn, (void**)&orig)) return;
        BYTE* b = (BYTE*)target; LOG("d3d: slot %d not hot-patchable (%02x %02x %02x %02x), vtable patch only", i, b[0], b[1], b[2], b[3]);
        void* o = Patch(vt, i, fn); if (o) orig = (T)o;
    }
    void HookDevice(IDirect3DDevice9* dev) {
        void** vt = *(void***)dev;
        if (vt == g_devVt) return;
        bool first = g_devVt == nullptr;
        g_devVt = vt; g_hookedDev = dev;
        if (!first) LOG("d3d: device vtable changed to %p - re-hooking", vt);
        { char b1[100], b2[100], b3[100]; LOG("d3d: vtable %s, Present %s, EndScene %s", ModOf(vt, b1), ModOf(vt[17], b2), ModOf(vt[42], b3)); }
        if (first) CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
        P(vt, 16, oReset, (void*)hReset);   P(vt, 17, oPresent, (void*)hPresent);
        P(vt, 37, oSetRT, (void*)hSetRT);   P(vt, 41, oBegin, (void*)hBegin);
        P(vt, 42, oEnd, (void*)hEnd);       P(vt, 43, oClear, (void*)hClear);
        P(vt, 44, oSetTransform, (void*)hSetTransform); P(vt, 47, oSetViewport, (void*)hSetViewport);
        P(vt, 81, oDP, (void*)hDP);         P(vt, 82, oDIP, (void*)hDIP);
        P(vt, 83, oDPUP, (void*)hDPUP);     P(vt, 84, oDIPUP, (void*)hDIPUP);
        P(vt, 94, oVSC, (void*)hVSC);
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
            void** svt = *(void***)sc;
            P(svt, 3, oSCPresent, (void*)hSCPresent);
            sc->Release();
        }
        LOG("d3d: device %p hooked (vtable %p, swap chain present hooked=%d)", dev, vt, oSCPresent != nullptr);
    }

    HRESULT WINAPI hCreateDevice(IDirect3D9* d3d, UINT ad, D3DDEVTYPE t, HWND fw, DWORD fl, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
        ForceWindowed(pp, (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : fw);
        HRESULT hr = oCreateDevice(d3d, ad, t, fw, fl, pp, out);
        g_hwnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : fw;
        if (pp) { g_w = pp->BackBufferWidth; g_h = pp->BackBufferHeight; }
        LOG("d3d: CreateDevice -> %08lx  %dx%d windowed=%d flags=%lx hwnd=%p thread=%lu", hr, g_w, g_h, pp ? pp->Windowed : -1, fl, g_hwnd, GetCurrentThreadId());
        if (SUCCEEDED(hr) && out && *out) { g_device = *out; HookDevice(*out); }
        return hr;
    }
}

namespace d3dhook {
void HookD3D(IDirect3D9* d3d) {
    if (!d3d) return;
    void** vt = *(void***)d3d;
    static void** done = nullptr;
    if (vt == done) return;
    done = vt;
    if (!HotPatch(vt[16], (void*)hCreateDevice, (void**)&oCreateDevice)) oCreateDevice = (CreateDevice_t)Patch(vt, 16, (void*)hCreateDevice);
    LOG("d3d: IDirect3D9 %p hooked", d3d);
}
IDirect3DDevice9* Device() { return g_device; }
void RequestTrace() { InterlockedExchange(&g_traceReq, 1); }
bool Tracing() { return g_trace; }
HWND GameWindow() { return g_hwnd; }
void ScreenSize(int& w, int& h) {
    RECT rc; if (g_hwnd && GetClientRect(g_hwnd, &rc) && rc.right > 0) { w = rc.right; h = rc.bottom; } else { w = g_w; h = g_h; }
}
}

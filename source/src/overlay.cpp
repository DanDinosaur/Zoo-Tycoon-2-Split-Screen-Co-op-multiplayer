#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include "overlay.h"
#include "config.h"
#include "log.h"
#include <d3dx9core.h>

namespace {
    IDirect3DTexture9* g_tex = nullptr;
    bool g_texTried = false;
    IDirect3DStateBlock9* g_sb = nullptr;
    HMODULE g_d3dx = nullptr;
    typedef HRESULT (WINAPI* TexFromFile_t)(IDirect3DDevice9*, const char*, IDirect3DTexture9**);
    typedef HRESULT (WINAPI* SaveSurf_t)(const char*, int, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
    HMODULE D3DX() {
        if (!g_d3dx) { const char* n[] = {"d3dx9_32.dll","d3dx9_43.dll","d3dx9_42.dll","d3dx9_41.dll","d3dx9_40.dll"};
            for (auto s : n) if ((g_d3dx = LoadLibraryA(s))) { LOG("overlay: using %s", s); break; } }
        return g_d3dx;
    }
    struct V { float x, y, z, rhw; DWORD c; float u, v; };
    ID3DXFont* g_fonts[4] = {};
    int g_fontH[4] = {};
    typedef HRESULT (WINAPI* CreateFontA_t)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR, ID3DXFont**);
    ID3DXFont* Font(IDirect3DDevice9* d, int h) {
        int slot = -1;
        for (int i = 0; i < 4; ++i) if (g_fonts[i] && g_fontH[i] == h) return g_fonts[i];
        for (int i = 0; i < 4 && slot < 0; ++i) if (!g_fonts[i]) slot = i;
        if (slot < 0) { g_fonts[3]->Release(); g_fonts[3] = nullptr; slot = 3; }
        HMODULE x = D3DX(); if (!x) return nullptr;
        auto cf = (CreateFontA_t)GetProcAddress(x, "D3DXCreateFontA"); if (!cf) return nullptr;
        if (FAILED(cf(d, h, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                      DEFAULT_PITCH | FF_SWISS, "Arial", &g_fonts[slot]))) { g_fonts[slot] = nullptr; return nullptr; }
        g_fontH[slot] = h; return g_fonts[slot];
    }
}

namespace overlay {
namespace { IDirect3DStateBlock9* g_sbSky = nullptr; IDirect3DSurface9* g_grab = nullptr; bool g_grabPending = false;
            IDirect3DSurface9* g_samp = nullptr; IDirect3DSurface9* g_sampSys = nullptr; }   // ZT18h: SampleRect surfaces
void OnLost() {
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
    if (g_sbSky) { g_sbSky->Release(); g_sbSky = nullptr; }
    if (g_grab) { g_grab->Release(); g_grab = nullptr; } g_grabPending = false;   // default-pool render target
    if (g_samp) { g_samp->Release(); g_samp = nullptr; } if (g_sampSys) { g_sampSys->Release(); g_sampSys = nullptr; }
    for (auto& f : g_fonts) if (f) { f->Release(); f = nullptr; }   // D3DX fonts hold default-pool resources
}   // texture is MANAGED and survives Reset

void DrawMarker(IDirect3DDevice9* d, float cx, float cy, float dia, float t) {
    if (!g_texTried) {
        g_texTried = true;
        // ZT14: the game's own yellow terrain-brush ring (UI/cursor_texture/biomecursor.dds, put next to the mod by the
        // installer) - the old delete circle stays as the fallback
        const char* names[2] = { "biomecursor.dds", "recycle.dds" };
        for (int i = 0; i < 2 && !g_tex; ++i) {
            char p[MAX_PATH]; snprintf(p, MAX_PATH, "%lszt2mod\\%s", config::GameDir(), names[i]);
            if (HMODULE x = D3DX()) if (auto f = (TexFromFile_t)GetProcAddress(x, "D3DXCreateTextureFromFileA")) {
                HRESULT hr = f(d, p, &g_tex); LOG("overlay: marker texture %s -> %08lx", p, hr); if (FAILED(hr)) g_tex = nullptr; }
        }
    }
    if (!g_sb && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_sb))) return;
    g_sb->Capture();
    if (FAILED(d->BeginScene())) return;
    D3DVIEWPORT9 vp; d->GetViewport(&vp);
    int W, H; { IDirect3DSurface9* bb = nullptr; d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb); D3DSURFACE_DESC sd{}; if (bb) { bb->GetDesc(&sd); bb->Release(); } W = sd.Width; H = sd.Height; }
    D3DVIEWPORT9 full = {0, 0, (DWORD)W, (DWORD)H, 0.f, 1.f}; d->SetViewport(&full);
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE); d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetTexture(0, g_tex);
    d->SetTextureStageState(0, D3DTSS_COLOROP, g_tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, g_tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    // Game's delete cursor: rotates once per 2.5 s, colour pulses every 0.5 s (ui/modes/modes.xml mode_deletion).
    float ang = -t * 6.2831853f / 2.5f;
    float pulse = 0.5f + 0.5f * sinf(t * 6.2831853f / 0.5f);
    DWORD col = D3DCOLOR_ARGB(230, 255, (int)(160 + 95 * pulse), (int)(60 * pulse));
    float rx = dia * 0.5f, ry = dia * 0.5f * 0.55f;   // flattened like a circle lying on the ground
    if (g_tex) {
        V q[4]; float cu[4] = {0,1,1,0}, cvv[4] = {0,0,1,1};
        for (int i = 0; i < 4; ++i) {
            float lx = (cu[i] - 0.5f) * 2.f, ly = (cvv[i] - 0.5f) * 2.f;
            float x = lx * cosf(ang) - ly * sinf(ang), y = lx * sinf(ang) + ly * cosf(ang);
            q[i] = { cx + x * rx - 0.5f, cy + y * ry - 0.5f, 0.f, 1.f, col, cu[i], cvv[i] };
        }
        d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, q, sizeof(V));
    } else {
        V ring[66]; int n = 0;   // procedural fallback: thick ring as a triangle strip
        for (int i = 0; i <= 32; ++i) { float a = ang + i * 6.2831853f / 32;
            ring[n++] = { cx + cosf(a) * rx, cy + sinf(a) * ry, 0, 1, col, 0, 0 };
            ring[n++] = { cx + cosf(a) * rx * 0.82f, cy + sinf(a) * ry * 0.82f, 0, 1, col, 0, 0 }; }
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, n - 2, ring, sizeof(V));
    }
    d->EndScene();
    g_sb->Apply();
}

// Panel + text lines (player 2's menus, hints). One call = one BeginScene/EndScene with the state block restored.
void Panel(IDirect3DDevice9* d, const RECT& r, DWORD bg, const Line* lines, int n, int fontPx) {
    if (!g_sb && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_sb))) return;
    g_sb->Capture();
    if (FAILED(d->BeginScene())) return;
    int W, H; { IDirect3DSurface9* bb = nullptr; d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb); D3DSURFACE_DESC sd{}; if (bb) { bb->GetDesc(&sd); bb->Release(); } W = sd.Width; H = sd.Height; }
    D3DVIEWPORT9 full = {0, 0, (DWORD)W, (DWORD)H, 0.f, 1.f}; d->SetViewport(&full);
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr); d->SetTexture(0, nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE); d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (bg >> 24) {
        V q[4] = { {(float)r.left, (float)r.top, 0, 1, bg, 0, 0}, {(float)r.right, (float)r.top, 0, 1, bg, 0, 0},
                   {(float)r.right, (float)r.bottom, 0, 1, bg, 0, 0}, {(float)r.left, (float)r.bottom, 0, 1, bg, 0, 0} };
        d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, q, sizeof(V));
    }
    for (int i = 0; i < n; ++i) if (lines[i].bg >> 24) {
        const RECT& b = lines[i].box; DWORD c = lines[i].bg;
        V q[4] = { {(float)b.left, (float)b.top, 0, 1, c, 0, 0}, {(float)b.right, (float)b.top, 0, 1, c, 0, 0},
                   {(float)b.right, (float)b.bottom, 0, 1, c, 0, 0}, {(float)b.left, (float)b.bottom, 0, 1, c, 0, 0} };
        d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, q, sizeof(V));
    }
    if (n > 0 && fontPx > 0) if (ID3DXFont* f = Font(d, fontPx)) {
        for (int i = 0; i < n; ++i) {
            RECT t = lines[i].box; t.left += fontPx / 3;
            f->DrawTextA(nullptr, lines[i].text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP, lines[i].color);
        }
    }
    d->EndScene();
    g_sb->Apply();
}

bool Screenshot(IDirect3DDevice9* d, const wchar_t* wpath) {
    HMODULE x = D3DX(); if (!x) return false;
    auto save = (SaveSurf_t)GetProcAddress(x, "D3DXSaveSurfaceToFileA"); if (!save) return false;
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr; bool ok = false;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC sd; bb->GetDesc(&sd);
        if (SUCCEEDED(d->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
            SUCCEEDED(d->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
            SUCCEEDED(d->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(d->GetRenderTargetData(rt, sys))) {
            char p[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, wpath, -1, p, MAX_PATH, nullptr, nullptr);
            ok = SUCCEEDED(save(p, 1 /*JPG*/, sys, nullptr, nullptr));
        }
    }
    if (sys) sys->Release(); if (rt) rt->Release(); if (bb) bb->Release();
    return ok;
}
}

namespace overlay {
// ZT18f: the sky. The game draws no sky for camera poses that are not its own (player 2's views, riding), so the
// back buffer's clear colour (black) showed above the horizon. A vertical gradient, horizon -> zenith, by the ray
// elevation of each screen edge; the game's fog colour (if it uses one) is the horizon colour so distant ground blends.
void Sky(IDirect3DDevice9* d, float pitch, float frT, float frB) {
    if (!d || frT <= frB) return;
    D3DVIEWPORT9 vp; if (FAILED(d->GetViewport(&vp)) || !vp.Width || !vp.Height) return;
    float eTop = atanf(frT) - pitch;              // elevation of the top edge's ray (rad, up > 0)
    if (eTop < -0.02f) return;                    // no sky in view
    if (!g_sbSky && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_sbSky))) return;
    DWORD fogOn = 0, fogCol = 0;
    d->GetRenderState(D3DRS_FOGENABLE, &fogOn); d->GetRenderState(D3DRS_FOGCOLOR, &fogCol);
    g_sbSky->Capture();
    int hr = 176, hg = 204, hb = 232;             // horizon (light haze blue)
    if (fogOn && fogCol) { int r = (fogCol >> 16) & 255, g = (fogCol >> 8) & 255, b = fogCol & 255;
        if (r + g + b > 150 && r + g + b < 720) { hr = r; hg = g; hb = b; } }
    const int zr = 74, zg = 128, zb = 206;        // zenith
    auto col = [&](float elev) -> DWORD {
        float t = elev / 1.2f; if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f; t = sqrtf(t);
        return D3DCOLOR_ARGB(255, (int)(hr + (zr - hr) * t), (int)(hg + (zg - hg) * t), (int)(hb + (zb - hb) * t)); };
    // screen y of a ray elevation e: tan(e + pitch) = frustum y  ->  0 at frB .. 1 at frT (top)
    auto yOf = [&](float e) { float fy = tanf(e + pitch); float u = (fy - frB) / (frT - frB); return vp.Y + (1.f - u) * vp.Height; };
    float eBot = atanf(frB) - pitch;
    float x0 = (float)vp.X - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f;
    float yTop = (float)vp.Y - 0.5f, yBot = (float)(vp.Y + vp.Height) - 0.5f;
    float eH = eBot > 0.f ? eBot : 0.f;           // horizon (or the bottom edge if it is above the horizon)
    float yH = eBot > 0.f ? yBot : yOf(0.f); if (yH > yBot) yH = yBot;
    struct CV { float x, y, z, rhw; DWORD c; };
    DWORD cTop = col(eTop), cH = col(eH);
    CV q[8] = { {x0, yTop, 1.f, 1.f, cTop}, {x1, yTop, 1.f, 1.f, cTop}, {x0, yH, 1.f, 1.f, cH}, {x1, yH, 1.f, 1.f, cH},
                {x0, yH, 1.f, 1.f, cH}, {x1, yH, 1.f, 1.f, cH}, {x0, yBot, 1.f, 1.f, cH}, {x1, yBot, 1.f, 1.f, cH} };
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr); d->SetTexture(0, nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE); d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (yH > yTop + 0.5f) d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(CV));
    if (yBot > yH + 0.5f) d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q + 4, sizeof(CV));
    g_sbSky->Apply();
}
bool GrabRect(IDirect3DDevice9* d, const RECT& r) {
    if (!d || r.right <= r.left || r.bottom <= r.top) return false;
    IDirect3DSurface9* bb = nullptr; bool ok = false;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC sd; bb->GetDesc(&sd);
        UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
        if (g_grab) { D3DSURFACE_DESC gd; g_grab->GetDesc(&gd); if (gd.Width != w || gd.Height != h || gd.Format != sd.Format) { g_grab->Release(); g_grab = nullptr; } }
        if (!g_grab) d->CreateRenderTarget(w, h, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_grab, nullptr);
        ok = g_grab && SUCCEEDED(d->StretchRect(bb, &r, g_grab, nullptr, D3DTEXF_NONE));
        bb->Release();
    }
    g_grabPending = ok;
    return ok;
}
bool GrabPending() { return g_grabPending; }
bool SampleRect(IDirect3DDevice9* d, const RECT& r, float rgb[3]) {
    if (!d || r.right <= r.left || r.bottom <= r.top) return false;
    IDirect3DSurface9* bb = nullptr; bool ok = false;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC sd; bb->GetDesc(&sd);
    if (sd.Format == D3DFMT_X8R8G8B8 || sd.Format == D3DFMT_A8R8G8B8) {
        if (!g_samp) d->CreateRenderTarget(16, 4, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_samp, nullptr);
        if (!g_sampSys) d->CreateOffscreenPlainSurface(16, 4, sd.Format, D3DPOOL_SYSTEMMEM, &g_sampSys, nullptr);
        if (g_samp && g_sampSys && SUCCEEDED(d->StretchRect(bb, &r, g_samp, nullptr, D3DTEXF_LINEAR)) &&
            SUCCEEDED(d->GetRenderTargetData(g_samp, g_sampSys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(g_sampSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                double s[3] = {};
                for (int y = 0; y < 4; ++y) { const BYTE* row = (const BYTE*)lr.pBits + y * lr.Pitch;
                    for (int x = 0; x < 16; ++x) { s[0] += row[x * 4 + 2]; s[1] += row[x * 4 + 1]; s[2] += row[x * 4]; } }
                g_sampSys->UnlockRect();
                for (int i = 0; i < 3; ++i) rgb[i] = (float)(s[i] / 64.0);
                ok = true;
            }
        }
    }
    bb->Release();
    return ok;
}
bool SaveGrab(IDirect3DDevice9* d, const wchar_t* wpath) {
    if (!g_grabPending || !g_grab) return false;
    g_grabPending = false;
    HMODULE x = D3DX(); if (!x) return false;
    auto save = (SaveSurf_t)GetProcAddress(x, "D3DXSaveSurfaceToFileA"); if (!save) return false;
    D3DSURFACE_DESC gd; g_grab->GetDesc(&gd);
    IDirect3DSurface9* sys = nullptr; bool ok = false;
    if (SUCCEEDED(d->CreateOffscreenPlainSurface(gd.Width, gd.Height, gd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(d->GetRenderTargetData(g_grab, sys))) {
        char p[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, wpath, -1, p, MAX_PATH, nullptr, nullptr);
        ok = SUCCEEDED(save(p, 1 /*JPG*/, sys, nullptr, nullptr));
    }
    if (sys) sys->Release();
    return ok;
}
void Rect(IDirect3DDevice9* d, float x0, float y0, float x1, float y1, DWORD color) {
    RECT r{ (LONG)x0, (LONG)y0, (LONG)x1, (LONG)y1 };
    Panel(d, r, color, nullptr, 0, 0);
}
}

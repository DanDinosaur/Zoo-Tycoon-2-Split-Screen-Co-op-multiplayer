#pragma once
#include <d3d9.h>
namespace overlay {
    // Delete-tool circle (the game's own recycle.dds, extracted by the installer) drawn at a screen point.
    void DrawMarker(IDirect3DDevice9* d, float cx, float cy, float diameterPx, float timeSec);
    bool Screenshot(IDirect3DDevice9* d, const wchar_t* path);   // backbuffer -> .jpg
    struct Line { const char* text; RECT box; DWORD color; DWORD bg; };
    void Panel(IDirect3DDevice9* d, const RECT& r, DWORD bg, const Line* lines, int n, int fontPx);
    void OnLost();
    // ZT18f: painted sky (gradient) in the CURRENT viewport, inside the scene, behind everything (no z write). pitch > 0 =
    // looking down; frT / frB = the camera frustum's top / bottom at unit distance.
    void Sky(IDirect3DDevice9* d, float pitch, float frT, float frB);
    // ZT18f: player 2's photo - copy a back-buffer rectangle now (inside the scene, before the UI), save it later outside it
    bool GrabRect(IDirect3DDevice9* d, const RECT& r);
    bool GrabPending();
    bool SaveGrab(IDirect3DDevice9* d, const wchar_t* path);   // JPG
    void Rect(IDirect3DDevice9* d, float x0, float y0, float x1, float y1, DWORD color);   // own Begin/EndScene
    // ZT18h: average colour of a back-buffer rectangle (scaled down to 16x4 by the GPU, read back) - title-screen checks
    bool SampleRect(IDirect3DDevice9* d, const RECT& r, float rgb[3]);
}

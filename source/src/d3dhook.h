#pragma once
#include <d3d9.h>
namespace d3dhook {
    void HookD3D(IDirect3D9* d3d);
    IDirect3DDevice9* Device();
    void RequestTrace();              // log every hooked D3D call of the next frame (with game return addresses)
    bool Tracing();
    HWND GameWindow();
    void ScreenSize(int& w, int& h);
}

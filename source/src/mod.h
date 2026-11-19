#pragma once
struct IDirect3D9; struct IDirect3DDevice9;
namespace mod {
    void Start();                       // DllMain attach (keep tiny)
    void OnD3DCreated(IDirect3D9* d3d);
    void OnFrame(IDirect3DDevice9* dev, void* hwnd);   // Present, game thread, before the real Present
    void OnDeviceLost();                // before Reset
}

#pragma once
#include <d3d9.h>
namespace padinput {
    // Player-1 controller -> the game's own keyboard/mouse bindings (UI/hotkeys/hotkeys.xml), every frame.
    void Frame(IDirect3DDevice9* dev, HWND hwnd, float dt, float now);
    void ReleaseAll();
    const char* RunningBuild();   // ZT19u: mod.cpp's ZT2MOD_BUILD
    // ZT18f: player 2's camera (photo view) in player 2's own first person - pad and keyboard+mouse (kbm2)
    void P2PhotoToggle(float now);
    bool P2Photo();
    void P2PhotoSnap(float now);
    void P2PhotoZoom(float factor);           // multiply the zoom (1..6)
    void P2PhotoOff();
    void P2FpBody(float dt, float now, bool moving);   // zookeeper body follows player 2's first person
    void PostWheel(HWND hwnd, int x, int y, int dir);  // posted mouse wheel at a client point (kbm2::kOurs)
}

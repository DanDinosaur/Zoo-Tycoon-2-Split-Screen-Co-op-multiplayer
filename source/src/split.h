#pragma once
// Split-screen: the game's whole world draw (SceneDraw 0x50310d) runs once per player with that player's
// camera pose and half-screen viewport (NiCamera port); the UI is drawn once afterwards.
namespace split {
    void ToggleP2First();
    void ToggleWide();
    void* MainCam();      // ZT10 test: zt2mod_wide.flag   // ZT10 test: zt2mod_swap.flag
    void Install();
    void SetActive(bool on);
    bool Active();
    // player 2's overhead camera (driven by pad 2): pan in the ground plane, rotate, zoom
    void P2Input(float panX, float panY, float rotate, float zoom, float dt, float tilt);
    bool TerrainZ(float x, float y, float& z);   // ground height under x,y (the game's ray->terrain test)
    bool P2Aim(float& x, float& y);              // player 2's look-at point on the ground
    void SetP2Pick(bool on);
    // ZT18e: player 2's OWN zookeeper first person (player 1 keeps the game's mode_super_staff at the same time):
    // player 2's camera stands 1.7 m above the ground, LS walks, RS looks. Off = the saved overhead view comes back.
    void P2FirstPerson(bool on);
    bool P2InFirstPerson();
    void P2FpInput(float lx, float ly, float rx, float ry, float dt, bool run);
    // ZT18f
    void P2FpLook(float dyaw, float dpitch);     // mouse look (kbm2) in player 2's first person
    bool P2Eye(float& x, float& y, float& z, float& yaw, float& pitch);   // player 2's camera eye (first person)
    bool WaterZ(float x, float y, float& z);     // water surface under x,y (the game's terrain vertex water data), false = dry
    void SetSolo(bool on);                       // the game's photo mode: player 1 full screen (split kept, not drawn)
    bool Solo();
    void SetP2Zoom(float z);                     // player 2's photo view zoom (1 = normal)
    void RequestP2Photo();                       // copy player 2's half after the next world draw (before the UI)
    void SetP2Pointer(int x, int y);             // pixel player 2's picks go through (-1 = middle of the half)                     // while on, world picks use player 2's camera + aim
    void* View();                // last render view (0x724994 arg): +0x70 NiCamera*
    void* CurrentPassCamera();   // NiCamera being drawn right now (nullptr outside SceneDraw)
    int  CurrentPass();          // -1 none, 0 = player 1 pass, 1 = player 2 pass
}

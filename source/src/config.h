#pragma once
// zt2mod.ini next to zt.exe (GetPrivateProfile*). Missing keys = defaults below.
struct Config {
    bool  log = true;
    bool  dumpImage = true;        // write zt2mod_dump.bin once per session (decrypted game image, for analysis)
    float deadzone = 0.24f;
    float trigger = 0.25f;
    float pointerSpeed = 400.f;
    float fpLookSpeed = 1400.f;   // ZT17c: guest walk look, mouse pixels per second at full right stick    // pixels per second at full stick
    bool  consoleStyle = true;     // centre-locked aim (true) or PC-style free pointer (false)
    int   relockMs = 700;          // console style: pointer re-centres this long after the right stick was last used
    bool  marker = true;           // draw the delete-tool circle at the aim point
    float markerSize = 0.11f;      // circle diameter as a fraction of screen height
    bool  padEnabled = true;
    bool  windowed = false;
    float animalWalk = 3.5f;       // ridden animal speeds (m/s) and turn rate (rad/s)
    float animalRun = 9.f;
    float animalTurn = 3.2f;
    float animalLoop = 0.f; float walkReloop = 4.6f;
    int   animalStrafe = 1;
    int   cullUnion = 1;         // ZT10: [split] cull_union - ground culled for both players (no black ground for player 2)
    int animalNodeRotate = 0;   // ZT17 [animal] node_rotate: 1 turn the model via its scene node, -1 opposite sign, 0 off
    float attackDamage = 25.f, defendDamage = 15.f, damageCap = 90.f, attackReach = 6.f;   // ZT12 riding attacks
    float attackMargin = 0.6f; int biteHitsSmall = 5, biteHitsBig = 10; char bigAnimals[1024] = "";   // ZT18d bites
    int   fenceBreak = 1, fenceHits = 5; float fenceMaxStrength = 9.f;                             // ZT12 fence break
    float maxZoom = 110.f;         // ZT10: highest camera (player 2; player 1 via world/cameras/overheadcam.xml in our z2f)        // ZT09: stick down/sideways = step back/sideways without turning (JPOG strafe_back)
    int   animalGround = 0;        // 0 = the engine fits the height (paths too), 1 = our terrain ray        // re-request the walk/run/idle sequence every N s (0 = only on change)
    int   sky = 1;               // ZT18f [split] sky: painted sky behind custom camera views (player 2, riding)
    int   keeperBody = 1;        // ZT18f [split] keeper_body: player 2's first person takes over the nearest zookeeper as a body
    int   splitMode = 1;             // 0 off, 1 auto (second pad connected), 2 always        // force a window instead of exclusive fullscreen (testing / alt-tab friendly)
};
namespace config {
    Config& Get();
    void Load();
    void SaveStyle();
    const wchar_t* GameDir();      // with trailing backslash
}

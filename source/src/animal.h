#pragma once
// Animal control for both players (JPOG dino control, ZT2 edition).
// Keyboard (player 1): T take/let go, I/J/K/L move, Shift run, Y attack, H special, O call, Z eat, B lie/sleep, V sniff.
// Take: RB + D-pad Up (pad) or T (keyboard, player 1) = the animal nearest the player's aim point.
// Ride (ZT14, JPOG layout): LS walk (camera-relative, LS click = run on/off), RS / D-pad = camera, LB/RB hold = turn
//       left/right, RT attack, LT special, Y iconic call (hold = second call), A eat/drink, X lie down / sleep / stand up, B let go.
// How (engine facts, ZT02 RE): Lua entity handle e._this -> [ud] -> [+4] -> [ ] = the C++ entity (vtbl 0x8f6d28).
//   0x6c3135(eax=entity) returns a pointer to its world position (x,y,z), 0x6c5156 caches the location component
//   at entity+0x68, node = [component+0xc]; after writing the position 0x770254(node) rebuilds the transform
//   (the same call the game's facing setter uses). Facing = BFG_SET_ENTITY_FACING_DIRECTION {x,y,z} (Lua),
//   animations = ANIM_REQUEST {seqToken, scale, time} (Lua; returns ANIM_REQUEST_SUCCESS / _FAILURE per species).
//   Height = the game's own ray->terrain test (split::TerrainZ).
class XPad;
struct IDirect3DDevice9;
namespace animal {
    // one player's controls this frame (pad and/or keyboard merged)
    struct In {
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        bool  down[14]{}, prev[14]{};     // PadButton order
        bool  runHeld = false;            // keyboard Shift = run while held
        bool D(int b) const { return down[b]; }
        bool P(int b) const { return down[b] && !prev[b]; }
        bool R(int b) const { return !down[b] && prev[b]; }
    };
    In FromPad(const XPad& pad);
    // Per frame, game thread. aimX/aimY = the player's aim point on the ground. Returns true while riding
    // (the caller must not use the pad for anything else then).
    bool Frame(int player, const In& pad, float dt, float now, bool haveAim, float aimX, float aimY, bool keyTake);
    bool Riding(int player);
    // chase camera of a riding player
    void ApplyTurn();
    void RideUI(float now);
    void KeepUI(float now);      // ZT17d: show the UI now (the non-riding player opens a menu)
    void MenuBusy(bool busy);    // ZT17d: the non-riding player is in a menu or using a build tool   // ZT17b: before each draw - the ridden animals' model faces their heading
    bool RidePose(int player, float& tx, float& ty, float& tz, float& yaw, float& pitch, float& dist);
    void ReleaseAll();
    void SelectionHint(float now);   // ZT14: "Ride this animal: RB + D-pad Up" when a player selects an animal
    // ZT12: short hint line over the riding player's half ("Fence: 3 / 5 hits", "Fence broken!")
    void Draw(IDirect3DDevice9* dev, float now, int W, int H, bool split);
    void Release(int player);
    void TestFar(bool on);
    // ZT18f: player 2's zookeeper body in player 2's own first person (feet position, heading); release when it ends
    void KeeperFrame(float x, float y, float feetZ, float yaw, bool moving, float now);
    void KeeperRelease();   // test harness: take ignores distance
}

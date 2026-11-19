#pragma once
// Player 2's build tools (JPOG builder2 for ZT2). Player 2 uses the game's OWN tools (mode_placement,
// mode_fence_placement, mode_path_placement, mode_deletion): the tool's world pick is made from player 2's camera
// through the middle of player 2's half (split::SetP2Pick around the tool's mouse-move/click messages), so the
// preview, validity colours, costs and placement rules are the vanilla ones.
// Pad 2: Y = build menu (D-pad up/down item, left/right category, A choose, B close), with a tool:
//        A = place (fences/paths: A start, A finish), D-pad left/right = rotate, B = stop the tool, BACK = delete tool.
// The game has one active tool: player 2 can start one while player 1 is in normal selection; if player 1 opens a
// tool of their own, player 2's tool closes with a message (JPOG rule "one player per tool").
#include <windows.h>
#include <d3d9.h>
class XPad;
namespace p2tools {
    // returns true when player 2's D-pad/A/B were used by the menu or a tool this frame
    bool Frame(IDirect3DDevice9* dev, const XPad& pad, float dt, float now, int W, int H, HWND hwnd, bool padBusy);
    const char* CurMode();                     // the game's current mode name (mode poll)
    void Adopt(const char* mode, const char* item, float now);   // player 2 started this tool from the game's own panel
    bool Busy();
    bool HasTool();
    bool GameToolIsP2();   // ZT17d: the game's one build tool is player 2's right now (player 1's B must not cancel it)   // player 2 is using a build tool (ZT17: triggers zoom then)
    // ZT15 both players keep their own tool (the game has one; it holds the tool of whoever acted last)
    void NoteP2Click(float now);                    // player 2 pressed a panel button
    void NoteP1Click(float now);                    // player 1 pressed a panel tab / button
    void NoteP1Pick(const char* item, float now);   // player 1 picked an item (object / fence / path / brush)
    bool P1Ready(float now);                        // player 1 presses in the world: false = their tool is going back in
    bool P1Mouse(bool down, int x, int y);          // player 1's real mouse button (window proc): true = held back
    float LastSelectAt();                     // ZT14: when player 2 last selected something with A
    void SetPointer(int x, int y);   // player 2's pointer pixel (middle of the half when locked)          // menu open or tool active
    void Reset();         // zoo unloaded / split off
    void SetModeName(const char* currentMode);   // fed by mod.cpp's mode poll
}

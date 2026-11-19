#pragma once
// Player 2 on a SECOND keyboard + mouse, vanilla PC controls (ZT07).
// Windows merges all keyboards/mice; Raw Input (WM_INPUT on the game window) tells which device sent each event.
// The game window procedure is subclassed: legacy key/mouse messages that come from player 2's devices are kept
// away from the game (only player 2's camera keys and mouse; player 2's other keys - Ctrl+C, Esc, Delete... -
// still reach the game, the menus are shared like the zoo). GetAsyncKeyState/GetKeyState hide player 2's camera keys.
// Player 2 gets an own pointer (drawn), own camera (W/A/S/D pan, arrows turn/tilt, +/- and wheel zoom,
// Ctrl + mouse = mouselook, screen edges scroll) and clicks: on the game's panels = a click there, in player 2's half
// = player 2's pick (select, place, build) through p2tools. T = take/let go of an animal, I/J/K/L ride (like player 1).
// Join: press F9 on the keyboard that becomes player 2's, then click with player 2's mouse (saved in zt2mod.ini
// [kbm] p2_keyboard / p2_mouse). F9 on player 2's keyboard again = player 2 back to the controller.
// Test: zt2mod_kbm2.txt = virtual player 2 keyboard + mouse (tokens: key names W A S D T UP DOWN LEFT RIGHT CTRL F9
// ESC DEL I J K L ..., LMB RMB, X=<px> Y=<px> pointer, WHEEL=<n>, FOR=<ms>).
#include <windows.h>
#include <d3d9.h>
class XPad;
namespace kbm2 {
    constexpr WPARAM kOurs = 0x4000;
    constexpr WPARAM kP2 = 0x8000;      // ZT16: with kOurs = player 2's message for their private tool    // wParam bit on mouse messages we post ourselves (never filtered)
    void Install(HWND hwnd);             // once, game thread
    bool Active();                       // player 2 plays with keyboard + mouse
    // per frame (split on): camera, pointer, clicks, riding. Returns true when it drove player 2 this frame.
    bool Frame(IDirect3DDevice9* dev, HWND hwnd, float dt, float now, int W, int H);
    void Draw(IDirect3DDevice9* dev, float now, int W, int H);
}

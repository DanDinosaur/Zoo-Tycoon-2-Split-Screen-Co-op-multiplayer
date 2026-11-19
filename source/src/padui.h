#pragma once
// Console-style pad navigation of the game's OWN UI (ZT07), JPOG-style and simple enough for a young kid:
// a big glowing frame sits on one of the game's buttons (tabs, item icons, panel buttons); D-pad or left stick
// jumps it to the next button in that direction, A presses it, B goes back to the world. Pushing past the end of
// an item list scrolls the list. Keyboard + mouse are untouched (vanilla).
// Buttons come from the live widget tree (UIRoot UI_GET_CHILDREN / UI_GET_SCREEN_RECT / UI_GET_LINEAGE_VISIBLE /
// UI_IS_DISABLED; 1024x768 UI space scaled to the window). Item lists (ZTUITypeList) scroll with UI_SCROLL(+-57).
#include <windows.h>
#include <d3d9.h>
class XPad;
namespace padui {
    enum Act { NONE = 0, CLICK = 1 };
    struct Result { bool used = false; Act act = NONE; int x = 0, y = 0; bool scrolled = false; int wheel = 0; };   // wheel: player 1 - turn the real mouse wheel (ZT18d)   // scrolled: RS scrolled a list (ZT18d)
    // player 0/1. W/H = client size. Returns what to do (CLICK at x,y = press the game's button there).
    Result Frame(int player, const XPad& pad, float now, int W, int H);
    // open one of the main panels (construction, animals, Zoofinance, staff ...) by its widget name and put the
    // player's frame into it. Returns the click to make.
    Result Open(int player, const char* mainButton, float now, int W, int H);
    bool SelPx(int player, int W, int H, int& x, int& y);   // centre of the highlighted button (pixels)
    bool Active(int player);          // the player's frame is in the menus (pad belongs to the menu)
    void Leave(int player);
    void EnterWindow(int player, float now, float x, float y, float w, float h);   // ZT19h: highlight into a window
    void Draw(IDirect3DDevice9* dev, float now, int W, int H);
    // keyboard + mouse player 2: is this pixel on the game's UI (name/item of the button under it)?
    bool OverUI(int px, int py, int W, int H, float now, char* name, int nameSize, bool* item);
    void NoteClick(int player, const char* name, bool item, float now);   // watch for a tool started by that click
    bool WatchTick(int player, float now);                                // true when the click started a tool
    void Reset();
    // ZT19w: the X (close) button of a window (UI space rect) -> its centre in UI space
    bool InfoX(float now, float rx, float ry, float rw, float rh, float& ux, float& uy);
    // ZT18e: the button this player pressed in THIS frame's Frame() was `name` -> forget the click (no tool watch, no list
    // jump), leave the menu, return true. The caller then does NOT post the click to the game.
    bool TakeClick(int player, const char* name, float now);
}

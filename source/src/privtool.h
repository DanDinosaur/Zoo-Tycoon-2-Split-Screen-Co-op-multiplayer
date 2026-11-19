#pragma once
// ZT16: player 2's PRIVATE copy of the game's own build tool (JPOG builder2 method).
// The game has one tool per kind inside mode_overhead (+0x12c = the active one). Player 2 gets a COPY of the game's
// configured ZTFenceMode (createComponent + the game instance's bytes, self-pointers relocated). Every frame, right after
// the game updates player 1's tool (overhead vfn 0x38 -> child vfn 0x38), the copy's own update runs with player 2's
// pick swapped in (split::SetP2Pick). Player 2's posted mouse messages (tag kbm2::kP2) reach the copy by swapping
// mode_overhead+0x12c around the window procedure call. Player 1 keeps the game's own tool the whole time.
#include <windows.h>
namespace privtool {
    void Install();                                  // once, game thread (hooks the overhead update)
    bool Start(const char* fenceType);               // player 2 starts a private fence tool; false = not possible now
    void Stop();
    void Hover();   // ZT18c: player 2's private fence / path tool re-aims at player 2 (game's UI_MOUSEMOVE)
    bool StartCurrent(const char* item);                             // ZT17f: player 2 gets a private copy of the game's CURRENT tool (terrain / path / fence)
    bool Active();
    bool NeedsSource();                              // ZT17f: player 2 can't get their own fence tool yet (player 1 never opened one)
    void NotePost(LPARAM l);                         // player 2 posted a mouse message at this lParam (for their copy)
    LRESULT Dispatch(WNDPROC orig, HWND h, UINT msg, WPARAM w, LPARAM l);   // a player-2 mouse message
}

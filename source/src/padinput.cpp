// Console-style pads for Zoo Tycoon 2, JPOG layout (ZT07), the same for player 1 and player 2:
//   LS pan, RS left/right turn + up/down zoom, D-pad up/down tilt, D-pad left/right or LB/RB turn the object,
//   A select / place / press, B back (close the menu, stop the tool, deselect), X Construction, LT Animals,
//   Y Zoo (finances, status), RT Staff, Back delete tool, Start pause menu (Esc), LS click guest walk (player 1),
//   RB + D-pad Up ride the animal at the aim. The menus are the game's own panels driven with a big highlight
//   (padui.cpp). Keyboard + mouse stay vanilla. Player 1's pad goes through the game's own bindings
//   (W/A/S/D move, arrows rotate/tilt, numpad -/+ zoom, ',' '.' rotate object, Del delete tool, Esc, G guest walk).
#include <windows.h>
#include <string>
#include <cmath>
#include <initializer_list>
#include <cstdlib>
#include "padinput.h"
#include "xinput_pad.h"
#include "overlay.h"
#include "config.h"
#include "log.h"
#include "split.h"
#include "animal.h"
#include "p2tools.h"
#include "padui.h"
#include "kbm2.h"
#include "luabridge.h"
#include "d3dhook.h"

namespace {
    XPad g_p1(0), g_p2(1);
    bool g_held[256]{};
    bool g_lmb = false, g_rmb = false;
    bool g_locked = true;        // console style: pointer pinned to the view centre (aim)
    bool g_fp = false;           // guest-walk (first person) mode, toggled by our own LS click / Esc
    float g_px = -1, g_py = -1;  // free pointer position (client px)
    float g_repeatAt[4]{};
    bool g_p2Logged = false;
    bool g_padActive = false;      // the pad drives the game only after it was used; a real mouse move hands control back
    POINT g_setPos{ -1, -1 };      // where we last put the pointer (screen)

    bool Extended(BYTE vk) {
        switch (vk) { case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DELETE: case VK_INSERT:
                      case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: return true; }
        return false;
    }
    void SendKey(BYTE vk, bool down) {
        INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (Extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof in);
    }
    void Key(BYTE vk, bool down) { if (g_held[vk] != down) { g_held[vk] = down; SendKey(vk, down); } }
    void Tap(BYTE vk, bool ctrl = false) {
        if (ctrl) SendKey(VK_CONTROL, true);
        SendKey(vk, true); SendKey(vk, false);
        if (ctrl) SendKey(VK_CONTROL, false);
    }
    void Mouse(bool left, bool down) {
        bool& st = left ? g_lmb : g_rmb; if (st == down) return; st = down;
        INPUT in{}; in.type = INPUT_MOUSE;
        in.mi.dwFlags = left ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        SendInput(1, &in, sizeof in);
    }
    // Analog stick -> digital key: hold for a share of each 120 ms period (full tilt = always held).
    bool Pwm(float v, float now) {
        v = fabsf(v); if (v < 0.05f) return false; if (v > 0.85f) return true;
        float ph = fmodf(now, 0.12f) / 0.12f; return ph < v;
    }
    // ZT17: right stick = zoom OR turn, never both. Anything that leans up/down zooms at full stick strength (smooth, no
    // stutter on a diagonal); the map only turns when the stick is pushed (almost) straight left or right.
    void SplitRS(float x, float y, float& rot, float& zoom) {
        float m = sqrtf(x * x + y * y); if (m > 1.f) m = 1.f;
        rot = 0.f; zoom = 0.f;
        if (m < 0.2f) return;
        if (m > 0.6f) m = 1.f;   // most of the way = full speed (a held key, no pulsing)
        if (fabsf(y) >= fabsf(x) * 0.6f) zoom = (y > 0.f ? 1.f : -1.f) * m;   // within ~60 deg of up/down
        else rot = (x > 0.f ? 1.f : -1.f) * m;
    }
    // the overhead camera's rotate arrows (shell.xml "Rotate Left" / "Rotate Right") send ZT_CAMERA_ROTATE left/right on
    // press and stop on release - the stick does the same
    int g_camRot = 0;
    // ZT17c: the game's own hotkey for its rotate buttons (tooltip: "Rotate View Clockwise (Hotkey: CTRL-Right Arrow Key)"):
    // Ctrl + arrow held while the right stick is pushed left / right
    void CamRotate(float v) {
        int d = v > 0.3f ? 1 : v < -0.3f ? -1 : 0;
        if (d == g_camRot) return;
        Key(VK_LEFT, false); Key(VK_RIGHT, false); Key(VK_CONTROL, false);
        g_camRot = d;
        if (d) { Key(VK_CONTROL, true); Key(d > 0 ? VK_RIGHT : VK_LEFT, true); }
        LOG("pad: map turn %s (Ctrl+arrow)", d > 0 ? "clockwise" : d < 0 ? "anticlockwise" : "stop");
    }
    // ZT10: square the stick so a diagonal holds both keys fully (W+D) - same speed in every direction, like JPOG
    void Square(float& x, float& y) {
        float m = sqrtf(x * x + y * y), a = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);
        if (a < 0.001f) return; if (m > 1.f) m = 1.f;
        float k = m / a; x *= k; y *= k;
    }
    void Axis(float v, BYTE neg, BYTE pos, float now) {
        bool on = Pwm(v, now);
        Key(neg, on && v < 0); Key(pos, on && v > 0);
    }
    // Press now, then auto-repeat while held (D-pad object rotation).
    bool Repeat(int i, bool down, bool pressed, float now) {
        if (pressed) { g_repeatAt[i] = now + 0.40f; return true; }
        if (down && now >= g_repeatAt[i]) { g_repeatAt[i] = now + 0.12f; return true; }
        return false;
    }
    // player 2 presses the game's own UI: a posted click at that pixel, then the pointer goes back to player 1's spot
    void PostClick(HWND hwnd, int x, int y) {
        PostMessageW(hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(x, y));
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON | kbm2::kOurs, MAKELPARAM(x, y));
        PostMessageW(hwnd, WM_LBUTTONUP, kbm2::kOurs, MAKELPARAM(x, y));
        POINT c; GetCursorPos(&c); ScreenToClient(hwnd, &c);
        PostMessageW(hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(c.x, c.y));
    }
    // ZT19w (owner photo: an animal's info window - B must close it like its X in the top right corner): the game's one
    // 'Entity Info Layout' window; split screen: only the player whose half it sits in closes it
    bool InfoClose(int player, float now, int W, int H, HWND hwnd) {
        char o[96]; float x, y, w, h;
        if (!luabridge::Ready() || !luabridge::Idle()) return false;
        if (!luabridge::Run("local w=queryObject('UIRoot'):UI_GET_CHILD('Entity Info Layout') if not w then return '' end "
                            "local ok,v=pcall(function() return w:UI_GET_LINEAGE_VISIBLE() end) if ok and not v then return '' end "
                            "local q=w:UI_GET_SCREEN_RECT() if not q then return '' end return string.format('%.0f %.0f %.0f %.0f',q.x,q.y,q.w,q.h)", o, sizeof o)
            || sscanf(o, "%f %f %f %f", &x, &y, &w, &h) != 4 || w <= 0.f) return false;
        if (split::Active() && ((player == 0) != (x + w * 0.5f < 512.f))) return false;
        float ux, uy; if (!padui::InfoX(now, x, y, w, h, ux, uy)) return false;
        int px = (int)(ux * W / 1024.f), py = (int)(uy * H / 768.f);
        PostClick(hwnd, px, py);
        LOG("pad: player %d B = close the info window (X at %d,%d)", player + 1, px, py);
        return true;
    }
    // X / LT / Y / RT open the game's main panels (trigger edges tracked per player)
    float g_prevLT[2]{}, g_prevRT[2]{};
    // ZT17 (owner 19:23): while a build tool is in use (fences, paths, objects, animals, terrain, delete) RT = zoom in,
    // LT = zoom out instead of opening the Animals / Staff panels
    bool ToolOn(int pl) {
        if (pl == 1) return p2tools::HasTool();
        std::string m = p2tools::CurMode();
        return m.compare(0, 5, "mode_") == 0 && m != "mode_selection" && m.compare(0, 9, "mode_main") && m != "mode_load" &&
               m != "mode_options" && m != "mode_download" && m != "mode_game" && m != "mode_overhead";
    }
    float TriggerZoom(int pl, const XPad& q) { return ToolOn(pl) ? q.RT() - q.LT() : 0.f; }
    const char* MainPanel(int pl, const XPad& q) {
        bool lt = q.LT() > 0.5f && g_prevLT[pl] <= 0.5f, rt = q.RT() > 0.5f && g_prevRT[pl] <= 0.5f;
        if (ToolOn(pl)) lt = rt = false;
        g_prevLT[pl] = q.LT(); g_prevRT[pl] = q.RT();
        if (q.Pressed(PAD_X)) return "construction";
        if (lt) return "animals";
        if (q.Pressed(PAD_Y)) return "terraform";      // ZT15: terrain brushes (water, grass, dirt) - finance stays on the panel
        if (rt) return "staff";
        return nullptr;
    }
    void SetPointer(HWND hwnd, int x, int y) {
        POINT p{ x, y }; ClientToScreen(hwnd, &p);
        POINT c; GetCursorPos(&c);
        if (c.x != p.x || c.y != p.y) SetCursorPos(p.x, p.y);
        g_setPos = p;
    }
}

namespace {
    // ZT17d: title-screen menus without a mouse. The game has no Lua before a zoo is loaded, so the buttons come from the
    // game's own layout files (ui/layout/mainmenu.xml, load.xml; 1024x768 UI space, stretched to the window). The real
    // pointer is put on the chosen button, so the game shows its own hover highlight; A clicks it, B goes back (Esc).
    struct MBtn { const char* name; float x, y, w, h; int next; int list; };   // next: screen after A (-1 stay, -2 leaves); list: row group
    // ZT18g: one table per title screen. Screens: main menu, load, map selection (freeform + challenge, x302 freeformselection.xml),
    // campaign (same shell, campaign lists), options (positions from the game's own screen - options.xml lives in x301), downloads,
    // profile dialog; MS_FREE = a screen we do not know (free pointer, B = Esc).
    enum { MS_MAIN = 0, MS_LOAD = 1, MS_FREE = 2, MS_MAP = 3, MS_CAMP = 4, MS_OPT = 5, MS_DL = 6, MS_PROF = 7 };
    const MBtn kMain[] = {
        { "Challenge", 446, 407, 97, 110, MS_MAP, -1 }, { "Freeform", 522, 407, 90, 107, MS_MAP, -1 }, { "Campaign", 400, 465, 109, 97, MS_CAMP, -1 },
        { "Options", 495, 492, 90, 97, MS_OPT, -1 },    { "Downloads", 563, 492, 80, 97, MS_DL, -1 }, { "Load Save Game", 410, 538, 105, 91, MS_LOAD, -1 },
        { "Exit", 498, 556, 96, 96, -1, -1 },           { "Profile", 380, 120, 50, 50, MS_PROF, -1 },
    };
    // Load Game Shell: LoadGameGUI at 256,192; SavedGamesList 23,72 238x149 with 16 px rows; Cancel / Delete / Load
    const int kLoadRows = 9;
    // a list = rows of one UIToggleSet: left x, width, first row top, row step, row height, visible rows; clickMove = a row is
    // clicked when the highlight moves onto it (the game's own selection, like a mouse click); aPlays = A on a row also presses Play
    struct MList { const char* name; float x, w, y0, step, h; int n; bool clickMove, aPlays; };
    struct MScreen { const char* name; const MBtn* btn; int nb; const MList* lst; int nl; const char* back; const char* start; };
    const MList kMapLists[] = { { "location", 18, 251, 80, 20, 20, 12, true, false }, { "map", 24, 234, 349, 19.2f, 19, 13, true, true } };
    const MBtn kMapBtn[] = { { "Cancel", 10, 681, 75, 75, MS_MAIN, -1 }, { "Play", 935, 680, 75, 75, -2, -1 },
                             { "Cash up", 920, 445, 37, 22, -1, -1 }, { "Cash down", 920, 463, 37, 22, -1, -1 } };
    const MList kCampLists[] = { { "campaign", 22, 247, 82, 24, 20, 9, true, false }, { "scenario", 22, 247, 350, 34.5f, 22, 7, true, true } };
    const MBtn kOptBtn[] = {
        { "Best Detail", 391, 165, 40, 34, -1, -1 }, { "Best Performance", 510, 165, 40, 34, -1, -1 }, { "Balanced Detail", 391, 215, 40, 34, -1, -1 },
        { "Advanced Settings", 510, 215, 40, 34, -1, -1 }, { "Full Screen", 382, 370, 40, 34, -1, -1 }, { "Windowed", 526, 370, 40, 34, -1, -1 },
        { "Mouse Look", 382, 431, 40, 34, -1, -1 }, { "Message of the Day", 382, 489, 40, 34, -1, -1 }, { "Feedback Options", 383, 545, 40, 34, -1, -1 },
        { "16-Bit", 72, 265, 40, 34, -1, -1 }, { "8-Bit", 217, 265, 40, 34, -1, -1 },
        { "Cancel", 405, 594, 54, 54, MS_MAIN, -1 }, { "Apply", 564, 594, 54, 54, MS_MAIN, -1 } };
    const MBtn kDlBtn[] = { { "Back", 10, 677, 75, 75, MS_MAIN, -1 } };
    const MList kProfLists[] = { { "profile", 270, 470, 322, 20, 20, 5, true, false } };
    const MBtn kProfBtn[] = { { "Add", 298, 437, 70, 70, -1, -1 }, { "Delete", 477, 437, 70, 70, -1, -1 },
                              { "Continue", 655, 437, 70, 70, MS_MAIN, -1 }, { "Close", 725, 246, 39, 39, MS_MAIN, -1 } };
    const MScreen kScr[] = {
        { "main", kMain, 8, nullptr, 0, nullptr, "Load Save Game" }, { "load", nullptr, 0, nullptr, 0, nullptr, nullptr },
        { "other", nullptr, 0, nullptr, 0, nullptr, nullptr },
        { "map selection", kMapBtn, 2, kMapLists, 2, "Cancel", "#1" }, { "campaign", kMapBtn, 2, kCampLists, 2, "Cancel", "#0" },
        { "options", kOptBtn, 13, nullptr, 0, "Cancel", "Apply" }, { "downloads", kDlBtn, 1, nullptr, 0, "Back", "Back" },
        { "profile", kProfBtn, 4, kProfLists, 1, "Close", "Continue" } };
    const int kMaxItems = 48;
    MBtn g_items[kMaxItems]; int g_nItems = 0, g_itemsFor = -1;
    int g_ms = MS_MAIN, g_msSel = 5, g_msLastDir = 0; float g_msRepeat = 0.f;
    // ZT18h: the first press of Freeform / Challenge after the game started loads the map screen (~10 s) but does not show it
    // (seen twice; the second press opens it). After A on a main-menu button that opens a full screen, the top strip of the
    // picture is compared before / after the load; unchanged = the press is repeated once.
    int g_vfBtn = -1, g_vfFrames = 0, g_vfTries = 0; DWORD g_vfAt = 0, g_vfMainAt = 0; float g_vfBefore[3] = {}, g_vfMain[3] = {};
    bool g_vfNeedBefore = false, g_vfMainOk = false;   // g_vfMain = the main menu's strip, sampled every 0.5 s while it shows
    DWORD g_msPointAt = 0; int g_msPointN = 0;   // ZT18g: pointer goes to the new screen's highlight a moment after the screen changed
                                                  // ZT18h: three times (0.4 / 1.5 / 3 s) with a 1 px nudge - a screen that loads slowly misses the first
    void BuildItems() {   // ZT18g: the current screen's buttons + list rows (rows first, in list order)
        if (g_itemsFor == g_ms) return;
        g_itemsFor = g_ms; g_nItems = 0;
        if (g_ms == MS_LOAD) {
            for (int i = 0; i < kLoadRows; ++i) g_items[g_nItems++] = MBtn{ "save", 279, 264.f + 16.f * i, 238, 16, -1, 0 };
            static const MBtn kB[3] = { { "Cancel", 308, 423, 64, 64, MS_MAIN, -1 }, { "Delete", 480, 423, 64, 64, -1, -1 }, { "Load", 650, 423, 64, 64, -1, -1 } };
            for (int i = 0; i < 3; ++i) g_items[g_nItems++] = kB[i];
            return;
        }
        if (g_ms < 0 || g_ms >= (int)(sizeof kScr / sizeof kScr[0])) return;
        const MScreen& s = kScr[g_ms];
        for (int l = 0; l < s.nl; ++l)
            for (int r = 0; r < s.lst[l].n && g_nItems < kMaxItems; ++r)
                g_items[g_nItems++] = MBtn{ s.lst[l].name, s.lst[l].x, s.lst[l].y0 + s.lst[l].step * r, s.lst[l].w, s.lst[l].h, -1, l };
        for (int b = 0; b < s.nb && g_nItems < kMaxItems; ++b) g_items[g_nItems++] = s.btn[b];
    }
    int MsCount() { BuildItems(); return g_nItems; }
    MBtn MsBtn(int i) { BuildItems(); return g_items[(i >= 0 && i < g_nItems) ? i : 0]; }
    int FindItem(const char* name) {   // "#n" = the list's first row of list n
        BuildItems();
        if (!name) return 0;
        if (name[0] == '#') { int l = atoi(name + 1); for (int i = 0; i < g_nItems; ++i) if (g_items[i].list == l) return i; return 0; }
        for (int i = 0; i < g_nItems; ++i) if (!strcmp(g_items[i].name, name)) return i;
        return 0;
    }
    const MList* ListOf(const MBtn& b) {
        if (b.list < 0 || g_ms == MS_LOAD || g_ms < 0 || g_ms >= (int)(sizeof kScr / sizeof kScr[0])) return nullptr;
        return b.list < kScr[g_ms].nl ? &kScr[g_ms].lst[b.list] : nullptr;
    }
    void GoScreen(int s) {
        g_ms = s; g_itemsFor = -1; g_msPointAt = GetTickCount() + 400; g_msPointN = 0;
        g_msSel = s == MS_LOAD ? 0 : (s >= 0 && s < (int)(sizeof kScr / sizeof kScr[0])) ? FindItem(kScr[s].start) : 0;
        LOG("pad: menus - screen %d (%s), highlight on '%s'", s, s == MS_LOAD ? "load" : kScr[s].name, MsBtn(g_msSel).name);
    }
    void MsPoint(HWND hwnd, int W, int H) {
        MBtn b = MsBtn(g_msSel);
        int x = (int)((b.x + b.w / 2) * W / 1024.f), y = (int)((b.y + b.h / 2) * H / 768.f);
        // ZT19p (owner video 27287 + log 12:17: at the start A did nothing until a menu was opened and closed): the game
        // lights / presses a main-menu button only after it saw the pointer MOVE onto it. A re-point to the same spot sent
        // no move at all -> wiggle 1 px so the game always gets a real move over the button
        SetPointer(hwnd, x + 1, y); SetPointer(hwnd, x, y);
    }
    // ZT18g: a click carrying its own absolute position, so two clicks in one frame (row + Play, back + pointer move) can not
    // land where the pointer was moved to afterwards
    // ZT19p: a click is sent only after the game has had the pointer over its target for a moment (hover first, then
    // press - the first A of a run went to a button the game had never seen the pointer on). Clicks wait in a small queue.
    struct PendClick { int x, y; DWORD due; };
    PendClick g_pc[6]; int g_npc = 0; HWND g_pcWnd = nullptr;
    void ClickNow(HWND hwnd, int x, int y);
    void ClickAbs(HWND hwnd, int x, int y) {
        POINT c; GetCursorPos(&c); POINT t{ x, y }; ClientToScreen(hwnd, &t);
        DWORD now = GetTickCount();
        bool hovered = g_npc == 0 && abs(c.x - t.x) <= 2 && abs(c.y - t.y) <= 2;
        // ZT19x (log 12:49:00 / 03: two main-menu A presses lost with the pointer already on the button for < 0.3 s): every
        // click now waits its hover time (the pointer wiggles onto the button first), also when the pointer is already there
        (void)hovered;
        if (g_npc == 0) { SetPointer(hwnd, x + 1, y); SetPointer(hwnd, x, y); }
        if (g_npc < 6) { DWORD d = now + 220; if (g_npc && g_pc[g_npc - 1].due + 160 > d) d = g_pc[g_npc - 1].due + 160; g_pc[g_npc++] = { x, y, d }; g_pcWnd = hwnd; }
    }
    void ClickTick() {   // every frame: send the queued clicks whose time has come (move onto the next one right after)
        while (g_npc && GetTickCount() >= g_pc[0].due) {
            PendClick k = g_pc[0]; for (int i = 1; i < g_npc; ++i) g_pc[i - 1] = g_pc[i]; --g_npc;
            ClickNow(g_pcWnd, k.x, k.y);
            if (g_npc) { SetPointer(g_pcWnd, g_pc[0].x + 1, g_pc[0].y); SetPointer(g_pcWnd, g_pc[0].x, g_pc[0].y); }
        }
    }
    void ClickNow(HWND hwnd, int x, int y) {
        SetPointer(hwnd, x, y);
        POINT p{ x, y }; ClientToScreen(hwnd, &p);
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN); if (sw < 2 || sh < 2) return;
        INPUT in[3]{}; for (auto& i : in) { i.type = INPUT_MOUSE; i.mi.dx = p.x * 65535 / (sw - 1); i.mi.dy = p.y * 65535 / (sh - 1); }
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(3, in, sizeof(INPUT));
    }
    void MsClick(HWND hwnd, int W, int H, int idx) {
        MBtn b = MsBtn(idx); ClickAbs(hwnd, (int)((b.x + b.w / 2) * W / 1024.f), (int)((b.y + b.h / 2) * H / 768.f));
    }
    void Wheel(int d) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = (DWORD)(d * WHEEL_DELTA); SendInput(1, &in, sizeof in); }
    // ZT18d (owner): over a list that scrolls with the mouse wheel, the RIGHT stick up/down is the mouse wheel - one notch at
    // once, then repeating faster the further the stick is pushed. Returns true while the stick is used for scrolling.
    float g_wheelAt = 0.f; int g_wheelDir = 0;
    bool WheelStick(float ry, float now) {
        int d = ry > 0.45f ? 1 : ry < -0.45f ? -1 : 0;   // stick up = wheel up (list moves up)
        if (!d) { g_wheelDir = 0; return fabsf(ry) > 0.2f; }
        if (d != g_wheelDir || now >= g_wheelAt) {
            Wheel(d);
            g_wheelAt = now + (d != g_wheelDir ? 0.35f : 0.30f - 0.20f * (fabsf(ry) - 0.45f) / 0.55f);
            g_wheelDir = d;
        }
        return true;
    }
    bool g_menuSeen = false;   // ZT19x
    bool g_titleDone = true; DWORD g_titleWait = 0;   // ZT19s: title guess off (it ate the first A on helium, which shows no title)
    DWORD g_picChangeAt = 0; int g_mainPend = -1; DWORD g_mainPendAt = 0; int g_verBtn = -1, g_verTries = 0; DWORD g_verAt = 0; float g_verBefore[3] = {};
    bool g_pauseNav = false; float g_pauseCheckAt = -1.f, g_pauseCheckUntil = 0.f;   // ZT19f: the title's 'Click to Continue' was clicked this run
    bool MenuNav(HWND hwnd, const XPad& p, float now, int W, int H) {
        if (g_ms == MS_FREE) {
            if (p.Pressed(PAD_B)) {   // ZT19o: unknown screen - its red X / back button spot first (Esc does not close all), then Esc
                ClickAbs(hwnd, (int)(47 * W / 1024.f), (int)(718 * H / 768.f)); Mouse(false, false); Tap(VK_ESCAPE);
                GoScreen(MS_MAIN); MsPoint(hwnd, W, H); LOG("pad: menus - B = red X spot + Esc, back to the main menu"); return true; }
            return false;   // a screen we do not know: sticks move the pointer, A clicks
        }
        BuildItems();
        if (g_msPointAt && GetTickCount() >= g_msPointAt) {
            ++g_msPointN; g_msPointAt = g_msPointN == 1 ? GetTickCount() + 1100 : g_msPointN == 2 ? GetTickCount() + 1500 : 0;
            MBtn b = MsBtn(g_msSel); int px = (int)((b.x + b.w / 2) * W / 1024.f), py = (int)((b.y + b.h / 2) * H / 768.f);
            SetPointer(hwnd, px + 1, py); SetPointer(hwnd, px, py);   // a real move, so the game draws its hover highlight
        }
        float sx = p.LX() + (p.Down(PAD_DRIGHT) ? 1.f : 0.f) - (p.Down(PAD_DLEFT) ? 1.f : 0.f);
        float sy = p.LY() + (p.Down(PAD_DUP) ? 1.f : 0.f) - (p.Down(PAD_DDOWN) ? 1.f : 0.f);
        int dir = 0;   // 1 right 2 left 3 up 4 down
        if (fabsf(sx) > 0.5f || fabsf(sy) > 0.5f) dir = fabsf(sx) > fabsf(sy) ? (sx > 0 ? 1 : 2) : (sy > 0 ? 3 : 4);
        bool step = dir && (dir != g_msLastDir || now >= g_msRepeat);
        if (step) { g_msRepeat = now + (dir != g_msLastDir ? 0.40f : 0.15f); }
        g_msLastDir = dir;
        if (step) {
            MBtn cur = MsBtn(g_msSel);
            if (g_ms == MS_LOAD && g_msSel < kLoadRows && (dir == 3 || dir == 4)) {
                int n = g_msSel + (dir == 4 ? 1 : -1);
                if (n < 0) Wheel(1);                       // past the top / bottom row: the game's list scrolls
                else if (n >= kLoadRows) Wheel(-1);
                else g_msSel = n;
                // ZT18d: the save rows highlight only when selected (toggle list) - click the row the stick moves to, like a
                // mouse click on it (selects + shows the preview; one click never loads)
                if (n >= 0 && n < kLoadRows) { MsPoint(hwnd, W, H); Mouse(true, true); Mouse(true, false); }
            } else if (cur.list >= 0 && g_ms != MS_LOAD && (dir == 3 || dir == 4) && g_msSel + (dir == 4 ? 1 : -1) >= 0 &&
                       g_msSel + (dir == 4 ? 1 : -1) < g_nItems && g_items[g_msSel + (dir == 4 ? 1 : -1)].list == cur.list) {
                g_msSel += dir == 4 ? 1 : -1;              // ZT18g: next row of the same list (RS up/down scrolls the list)
                const MList* l = ListOf(g_items[g_msSel]);
                if (l && l->clickMove) MsClick(hwnd, W, H, g_msSel);
            } else {
                float cx = cur.x + cur.w / 2, cy = cur.y + cur.h / 2;
                float dx0 = dir == 1 ? 1.f : dir == 2 ? -1.f : 0.f, dy0 = dir == 4 ? 1.f : dir == 3 ? -1.f : 0.f;   // UI y grows down
                int best = -1; float bs = 1e9f;
                for (int i = 0; i < g_nItems; ++i) {
                    if (i == g_msSel || (cur.list >= 0 && g_items[i].list == cur.list)) continue;   // a list is left by its edge
                    MBtn b = g_items[i]; float dx = b.x + b.w / 2 - cx, dy = b.y + b.h / 2 - cy;
                    float along = dx * dx0 + dy * dy0; if (along <= 4.f) continue;
                    float perp = fabsf(dx * dy0) + fabsf(dy * dx0);
                    float s = along + 2.f * perp; if (s < bs) { bs = s; best = i; }
                }
                if (g_ms == MS_LOAD && g_msSel >= kLoadRows && dir == 3 && best < 0) best = 0;
                if (best >= 0) {
                    g_msSel = best;
                    const MList* l = ListOf(g_items[best]);
                    if (l && l->clickMove) MsClick(hwnd, W, H, best);
                    LOG("pad: menus - highlight '%s' (%d)", g_items[best].name, best);
                    g_menuSeen = true;   // ZT19x: the player moves the highlight = they see the menu
                }
            }
        }
        // ZT18d: right stick up/down = mouse wheel (lists; the pointer stays on the highlighted row); left/right hands over to
        // the free pointer
        if (fabsf(p.RY()) > fabsf(p.RX()) && WheelStick(p.RY(), now)) { Mouse(false, false); return true; }
        if (fabsf(p.RX()) > 0.f) return false;
        if (step || p.Pressed(PAD_A)) MsPoint(hwnd, W, H);
        if (p.Pressed(PAD_A) && g_ms == MS_LOAD && g_msSel < kLoadRows) {   // ZT18d: A on a save = the Load button
            MBtn r = MsBtn(g_msSel);
            MsPoint(hwnd, W, H); Mouse(true, true); Mouse(true, false);   // make sure this row is the selected one
            MBtn ld = MsBtn(kLoadRows + 2);
            SetPointer(hwnd, (int)((ld.x + ld.w / 2) * W / 1024.f), (int)((ld.y + ld.h / 2) * H / 768.f));
            Mouse(true, true); Mouse(true, false); Mouse(false, false);
            LOG("pad: menus - A on save row %d (%s) -> Load", g_msSel, r.name);
            return true;
        }
        // ZT19f: the colour check (ZT19b) read sky / a white flash (log 14:33: 254 254 254 and 77 123 146 both 'main menu') -
        // the game ALWAYS opens on the title after its movies, so the pad's first A of the run is the 'Click to Continue'
        // click (on an empty spot of the main menu, so it is harmless if the title was already clicked with the mouse)
        {   // ZT19v (owner, again: 'you still have to click something and back out'; log 12:40:26 - the first A came 0.5 s
            // after the first frame, during the intro movies; ZT19s' 'still for 1 s' check had no picture history yet and
            // pressed 'Load Save Game' at once, the pad then thought it was on the load screen while the real screen was the
            // main menu - backing out only fixed the pad's idea of the screen). Now the middle of the screen is watched from
            // the start: until it has been still for 1.5 s (movies, title, the menu flying in) an A is only a click on an
            // empty spot (skips the movie / title) and the pad stays on the main menu.
            static DWORD s_t0 = 0, s_sampAt = 0, s_chgAt = 0; static int s_n = 0; static float s_prev[3]; static bool s_ready = false;
            DWORD t = GetTickCount(); if (!s_t0) { s_t0 = t; s_chgAt = t; }
            IDirect3DDevice9* d0 = d3dhook::Device();
            RECT m0{ (LONG)(260 * W / 1024), (LONG)(200 * H / 768), (LONG)(760 * W / 1024), (LONG)(560 * H / 768) };
            if (!s_ready && g_menuSeen && t - s_t0 >= 1500) { s_ready = true; g_titleDone = true; LOG("pad: menus - the highlight was moved, the pad drives the main menu"); }
            if (!s_ready && d0 && t - s_sampAt >= 250) {
                s_sampAt = t; float c[3];
                if (overlay::SampleRect(d0, m0, c)) {
                    if (s_n > 0 && fabsf(c[0] - s_prev[0]) + fabsf(c[1] - s_prev[1]) + fabsf(c[2] - s_prev[2]) > 25.f) s_chgAt = t;
                    for (int i = 0; i < 3; ++i) s_prev[i] = c[i]; ++s_n;
                }
                if ((s_n >= 4 && t - s_chgAt >= 1500) || t - s_t0 >= 30000) {
                    s_ready = true; g_titleDone = true; g_msPointAt = t + 100; g_msPointN = 0;
                    LOG("pad: menus - the main menu is still now, the pad drives it (highlight on '%s')", MsBtn(g_msSel).name);
                }
            }
            if (!s_ready) {
                if (p.Pressed(PAD_A)) { ClickAbs(hwnd, (int)(W * 0.04f), H / 2); Mouse(false, false); s_chgAt = t;
                    LOG("pad: menus - A while the intro / title shows = click to skip (the pad stays on the main menu)"); }
                Mouse(false, false);
                return true;
            }
        }
        if (!g_titleDone && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { g_titleDone = true; LOG("pad: menus - title clicked with the mouse"); }
        if (p.Pressed(PAD_A) && GetTickCount() < g_titleWait) { LOG("pad: menus - A ignored (main menu still appearing)"); return true; }
        if (p.Pressed(PAD_A) && g_ms == MS_MAIN && !g_titleDone) {
            g_titleDone = true; g_titleWait = GetTickCount() + 500;   // ZT19g: log 14:40:05/06 - a 2nd A 0.5 s later hit the fading menu
            // ZT19 (owner: right after the game starts the pad 'did not work' until many buttons were pressed): the game
            // opens on the 'Click to Continue' title, but the pad already drove the main menu - A pressed 'Load Save Game'
            // underneath, the pad thought it was on the load screen while the real screen was the main menu. Now A on the
            // title only clicks 'Click to Continue' (the pad stays on the main menu, highlight on its button).
            ClickAbs(hwnd, (int)(W * 0.04f), H / 2); Mouse(false, false);
            g_msPointAt = GetTickCount() + 600; g_msPointN = 0;
            LOG("pad: menus - title screen: A = Click to Continue");
            return true;
        }
        // ZT19s (owner: the main menu still ignored the first A; logs 12:17 / 12:28 - the menu was still flying in / animating,
        // 'picture changed' 0.5 s before each lost press): an A on the main menu waits until the picture has been still
        // for 1 s (at most 4 s), then presses; 1.5 s later the middle of the screen is compared - nothing changed = the
        // press was lost: the pad stays on the main menu and presses once more by itself.
        RECT mid{ (LONG)(260 * W / 1024), (LONG)(200 * H / 768), (LONG)(760 * W / 1024), (LONG)(560 * H / 768) };
        IDirect3DDevice9* dv = d3dhook::Device();
        if (g_ms == MS_MAIN && p.Pressed(PAD_A)) { g_mainPend = g_msSel; g_mainPendAt = GetTickCount(); LOG("pad: menus - A on '%s' (waiting for a still menu)", MsBtn(g_msSel).name); }
        bool doA = false;
        if (g_mainPend >= 0 && g_ms == MS_MAIN) {
            DWORD t = GetTickCount();
            if (t - g_picChangeAt >= 1000 || t - g_mainPendAt >= 4000) { g_msSel = g_mainPend; g_mainPend = -1; doA = true;
                g_verBtn = g_msSel; g_verTries = 0; g_verAt = t + 1500; if (!dv || !overlay::SampleRect(dv, mid, g_verBefore)) g_verBtn = -1; }
        } else if (g_mainPend >= 0) g_mainPend = -1;
        if (g_verBtn >= 0 && GetTickCount() >= g_verAt) {
            float a[3]; int idx = g_verBtn; g_verBtn = -1;
            if (dv && overlay::SampleRect(dv, mid, a)) {
                float dd = fabsf(a[0] - g_verBefore[0]) + fabsf(a[1] - g_verBefore[1]) + fabsf(a[2] - g_verBefore[2]);
                MBtn k = MsBtn(idx);
                LOG("pad: menus - '%s' check: middle of the screen changed %.1f", k.name, dd);
                if (dd < 10.f && k.next != -1) {
                    GoScreen(MS_MAIN); g_msSel = idx; MsPoint(hwnd, W, H);
                    if (g_verTries < 1) { ++g_verTries; MsClick(hwnd, W, H, idx); Mouse(false, false); if (k.next >= 0) GoScreen(k.next);
                        g_verBtn = idx; g_verAt = GetTickCount() + 1500; for (int i = 0; i < 3; ++i) g_verBefore[i] = a[i];
                        LOG("pad: menus - the press was lost, pressing '%s' again", k.name); }
                    else LOG("pad: menus - still nothing - the pad stays on the main menu");
                }
            }
        }
        if ((g_ms != MS_MAIN && p.Pressed(PAD_A)) || doA) {
            MBtn b = MsBtn(g_msSel);
            MsClick(hwnd, W, H, g_msSel);
            LOG("pad: menus - A on '%s'", b.name);
            const MList* l = ListOf(b);
            if (l && l->aPlays) {   // ZT18g: A on a map / scenario row = that row, then the game's own Play button
                int pl = FindItem("Play"); MsClick(hwnd, W, H, pl); Mouse(false, false);
                LOG("pad: menus - A on a %s row -> Play", l->name); GoScreen(MS_MAIN); return true;
            }
            if (g_ms == MS_MAIN && (b.next == MS_MAP || b.next == MS_CAMP || b.next == MS_OPT || b.next == MS_DL)) {
                // ZT19s: ZT18h's top-strip re-press replaced by the middle-of-screen check above (g_verBtn)
            }
            if (b.next == -2) GoScreen(MS_MAIN);          // Play: the zoo loads (menus left)
            else if (b.next >= 0) GoScreen(b.next);   // pointer moves to the new screen's highlight with the next step
        }
        if (p.Pressed(PAD_B)) {
            g_verBtn = -1; g_mainPend = -1;   // ZT19x: the player backed out - no automatic re-press
            if (g_ms == MS_MAIN) {   // ZT19o (owner photo: Challenge Mode - B must work like the big red X): if the pad lost
                // track of the screen, B still presses the spot every shell screen keeps its red X / back button on (bottom
                // left, UI 10,681 75x75); on the real main menu that spot is empty background
                ClickAbs(hwnd, (int)(47 * W / 1024.f), (int)(718 * H / 768.f)); Mouse(false, false);
                g_msPointAt = GetTickCount() + 500; g_msPointN = 0;
                LOG("pad: menus - B on the main menu = the red X spot (in case another screen shows)");
            }
            else if (g_ms == MS_LOAD) { Tap(VK_ESCAPE); GoScreen(MS_MAIN); MsPoint(hwnd, W, H); LOG("pad: menus - back to the main menu"); }
            else if (g_ms != MS_MAIN && kScr[g_ms].back) {   // ZT18g: B = the screen's own back / cancel button (Esc does not close all)
                MsClick(hwnd, W, H, FindItem(kScr[g_ms].back)); Mouse(false, false);
                LOG("pad: menus - B = '%s' -> main menu", kScr[g_ms].back);
                GoScreen(MS_MAIN);   // the pointer moves with the next step (not in this frame)
            }
        }
        Mouse(false, false);
        return true;
    }
}
namespace {
    // ZT18e: zookeeper first person for BOTH players at once. Player 1 = the game's own mode_super_staff (guest-walk pad
    // layout g_fp). Player 2 = an own eye-level camera in player 2's half (split::P2FirstPerson): their press of the
    // 'firstperson' button is kept from the game, so the game's one first-person mode stays player 1's.
    bool g_gameFp = false, g_gamePhoto = false;
    // ZT18f: player 2's own camera (photo view). The game's photo mode (mode_photo_safari) belongs to its first person =
    // player 1; player 2's camera button gives player 2 a photo view in their own first person instead: frame, RT/LT zoom
    // (narrower frustum in player 2's pass), A = photo of player 2's half (grabbed right after the world draw, before the
    // UI) saved as a JPG in Pictures\Zoo Tycoon 2 co-op photos, B = back to walking.
    bool g_p2Photo = false, g_p2PhotoFp = false; float g_p2Zoom = 1.f, g_flashAt = -10.f, g_savedAt = -10.f; char g_savedName[64] = "";
}
namespace {
    // ZT19: the game uses the Windows pointer (ui/cursor .cur files, SetCursor) - hide it with the display counter, undoing
    // exactly our own decrements when it should show again
    int g_curDec = 0; bool g_curHidden = false;
    void CursorHide(bool h) {
        // ZT19m (owner video 11-19: the arrow stays under the circle, it only goes away while the view turns): the game
        // draws the D3D hardware cursor (IDirect3DDevice9::ShowCursor), not only the Windows one - hide / show that too
        if (IDirect3DDevice9* d = d3dhook::Device()) { static bool s_d3dHidden = false;
            if (h) { d->ShowCursor(FALSE); s_d3dHidden = true; } else if (s_d3dHidden) { d->ShowCursor(TRUE); s_d3dHidden = false; } }
        if (h) {
            CURSORINFO ci{}; ci.cbSize = sizeof ci;
            if (!g_curHidden || (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))) {
                int guard = 0; while (ShowCursor(FALSE) >= 0 && ++guard < 50) ++g_curDec; ++g_curDec;
                if (!g_curHidden) LOG("pad: pointer hidden (controller style / riding)");
                g_curHidden = true;
            }
        } else if (g_curHidden) {
            while (g_curDec > 0) { ShowCursor(TRUE); --g_curDec; }
            g_curHidden = false; LOG("pad: pointer shown");
        }
    }
}
namespace padinput {
static HWND g_gameWnd = nullptr;
void ReleaseAll() {
    CamRotate(0.f);
    // ZT11+: the game's camera hotkeys are message based (hotkeys.xml: key down = ZT_SET_COMMAND_STATE, key up = CLEAR).
    // SendInput's key-up goes to the FOREGROUND window: after a focus change it never reached the game and the held
    // command stuck ('+' = camera zoomed in to the minimum by itself). Post the key-ups to the game window too.
    for (int i = 0; i < 256; ++i) if (g_held[i]) {
        if (g_gameWnd) PostMessageW(g_gameWnd, WM_KEYUP, (WPARAM)i, (LPARAM)(1 | (MapVirtualKeyW((UINT)i, MAPVK_VK_TO_VSC) << 16) | 0xC0000000u));
        Key((BYTE)i, false);
    }
    Mouse(true, false); Mouse(false, false);
}

void P2PhotoOff() {
    if (!g_p2Photo) return;
    g_p2Photo = false; g_p2Zoom = 1.f; split::SetP2Zoom(1.f);
    if (g_p2PhotoFp && split::P2InFirstPerson()) split::P2FirstPerson(false);   // the camera button started the walk: end it too
    g_p2PhotoFp = false;
    LOG("pad: player 2's camera off");
}
bool P2Photo() { return g_p2Photo; }
void P2PhotoToggle(float now) {
    if (g_p2Photo) { P2PhotoOff(); return; }
    if (animal::Riding(1)) return;
    g_p2PhotoFp = !split::P2InFirstPerson();
    if (g_p2PhotoFp) split::P2FirstPerson(true);
    if (!split::P2InFirstPerson()) { g_p2PhotoFp = false; return; }
    g_p2Photo = true; g_p2Zoom = 1.f; split::SetP2Zoom(1.f); g_savedAt = -10.f; (void)now;
    LOG("pad: player 2's camera on (own photo view)");
}
void P2PhotoSnap(float now) {
    if (!g_p2Photo) return;
    split::RequestP2Photo(); g_flashAt = now;
}
void P2PhotoZoom(float f) { if (!g_p2Photo) return; g_p2Zoom *= f; if (g_p2Zoom < 1.f) g_p2Zoom = 1.f; if (g_p2Zoom > 6.f) g_p2Zoom = 6.f; split::SetP2Zoom(g_p2Zoom); }
void P2FpBody(float dt, float now, bool moving) {
    (void)dt;
    float x, y, z, yaw, pitch;
    if (split::P2InFirstPerson() && split::P2Eye(x, y, z, yaw, pitch)) animal::KeeperFrame(x, y, z - 1.7f, yaw, moving, now);
}
void PostWheel(HWND hwnd, int x, int y, int dir) {
    POINT sp{ x, y }; ClientToScreen(hwnd, &sp);
    PostMessageW(hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(x, y));
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(kbm2::kOurs, (WORD)(short)(dir * WHEEL_DELTA)), MAKELPARAM(sp.x, sp.y));
    POINT c; GetCursorPos(&c); ScreenToClient(hwnd, &c);
    PostMessageW(hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(c.x, c.y));
    LOG("pad: player 2 list scroll - posted mouse wheel %+d at %d,%d", dir, x, y);
}
namespace {
    // photo view frame + hint + flash in player 2's half; saves a grabbed photo (outside the scene)
    void P2PhotoDraw(IDirect3DDevice9* dev, float now, int W, int H) {
        if (overlay::GrabPending()) {
            wchar_t dir[MAX_PATH] = L""; DWORD n = GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH);
            if (!n || n >= MAX_PATH) swprintf(dir, MAX_PATH, L"%ls", config::GameDir()); else wcscat(dir, L"\\Pictures");
            CreateDirectoryW(dir, nullptr); wcscat(dir, L"\\Zoo Tycoon 2 co-op photos"); CreateDirectoryW(dir, nullptr);
            SYSTEMTIME t; GetLocalTime(&t);
            wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\Player2_%04d-%02d-%02d_%02d-%02d-%02d.jpg", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            bool ok = overlay::SaveGrab(dev, path);
            snprintf(g_savedName, sizeof g_savedName, ok ? "Photo saved!" : "Photo could not be saved");
            g_savedAt = now;
            LOG("pad: player 2's photo %ls -> %s", path, ok ? "saved" : "FAILED");
        }
        if (!g_p2Photo) return;
        const float x0 = W * 0.5f, x1 = (float)W, cx = W * 0.75f, cy = H * 0.5f;
        const float fw = W * 0.5f * 0.80f, fh = H * 0.80f, L = H * 0.06f, T = H * 0.005f + 2.f;
        const float l = cx - fw / 2, r = cx + fw / 2, t = cy - fh / 2, b = cy + fh / 2; const DWORD w = 0xE0FFFFFF;
        overlay::Rect(dev, l, t, l + L, t + T, w); overlay::Rect(dev, l, t, l + T, t + L, w);
        overlay::Rect(dev, r - L, t, r, t + T, w); overlay::Rect(dev, r - T, t, r, t + L, w);
        overlay::Rect(dev, l, b - T, l + L, b, w); overlay::Rect(dev, l, b - L, l + T, b, w);
        overlay::Rect(dev, r - L, b - T, r, b, w); overlay::Rect(dev, r - T, b - L, r, b, w);
        overlay::Rect(dev, cx - T * 3, cy - T / 2, cx + T * 3, cy + T / 2, 0x90FFFFFF); overlay::Rect(dev, cx - T / 2, cy - T * 3, cx + T / 2, cy + T * 3, 0x90FFFFFF);
        float f = now - g_flashAt;
        if (f >= 0.f && f < 0.3f) overlay::Rect(dev, x0, 0.f, x1, (float)H, D3DCOLOR_ARGB((int)(220 * (1.f - f / 0.3f)), 255, 255, 255));
        char z[160];
        if (now - g_savedAt < 2.5f) snprintf(z, sizeof z, "%s", g_savedName);
        else snprintf(z, sizeof z, "Camera  x%.1f   A / click: photo   RT LT / wheel: zoom   B / right click: back", g_p2Zoom);
        int fp = H / 40; RECT box{ (LONG)(x0 + W * 0.02f), (LONG)(H - fp * 2.6f), (LONG)(x1 - W * 0.02f), (LONG)(H - fp * 1.0f) };
        overlay::Line ln{ z, box, 0xFFFFFFFF, 0xA0000000 };
        overlay::Panel(dev, box, 0, &ln, 1, fp);
    }
}
void Frame(IDirect3DDevice9* dev, HWND hwnd, float dt, float now) {
    Config& c = config::Get();
    if (!c.padEnabled || !hwnd) return;
    g_gameWnd = hwnd;
    g_p1.Poll(c.deadzone, c.trigger);
    g_p2.Poll(c.deadzone, c.trigger);
    {   // ZT12 diagnostic: real (non-virtual) stick values past the deadzone, at most every 5 s (stick drift = camera creep)
        static float s_at = 0.f;
        if (now >= s_at) {
            s_at = now + 5.f;
            for (int i = 0; i < 2; ++i) {
                const XPad& q = i ? g_p2 : g_p1;
                if (!q.Connected() || virtualpad::Get(i).active) continue;
                if (fabsf(q.LX()) > 0.01f || fabsf(q.LY()) > 0.01f || fabsf(q.RX()) > 0.01f || fabsf(q.RY()) > 0.01f)
                    LOG("pad: player %d sticks L %.2f %.2f R %.2f %.2f (deadzoned) fg=%d", i + 1, q.LX(), q.LY(), q.RX(), q.RY(), GetForegroundWindow() == hwnd);
            }
        }
    }
    RECT rcw; GetClientRect(hwnd, &rcw);
    const int W = rcw.right, H = rcw.bottom; if (W <= 0 || H <= 0) return;
    if (g_p2.Connected() != g_p2Logged) { g_p2Logged = g_p2.Connected();
        LOG("pad: player 2 %s", g_p2Logged ? "connected" : "gone"); }
    padui::Draw(dev, now, W, H);   // both players' highlights (last frame's selection)
    kbm2::Install(hwnd);
    kbm2::Draw(dev, now, W, H);
    {   // ZT19 (owner): in controller (aim circle) style and while riding with the pad, the Windows pointer is not shown at
        // all - only the circle. A real mouse move hands control back (g_padActive false) and the pointer comes back.
        const char* m = p2tools::CurMode();
        bool menus = !m[0] || !strcmp(m, "nil") || !strncmp(m, "mode_main", 9) || !strcmp(m, "mode_load") ||
                     !strcmp(m, "mode_options") || !strcmp(m, "mode_download");
        CursorHide(!menus && g_padActive && GetForegroundWindow() == hwnd && (c.consoleStyle || animal::Riding(0)));
    }
    animal::SelectionHint(now);
    animal::MenuBusy(padui::Active(0) || padui::Active(1) || ToolOn(0) || ToolOn(1));
    animal::RideUI(now);
    animal::Draw(dev, now, W, H, split::Active());
    {   // ZT19u (owner: 'why does it keep running older builds'): new builds can only be swapped in when the game starts (the
        // running game holds d3d9.dll). Every 5 s the mod reads zt2mod_builds\install_next.txt; when it names a newer build
        // than the one running, a banner at the top says so: close the game and start it from the launcher.
        static float s_at = 0.f; static char s_next[32] = "";
        if (now >= s_at) {
            s_at = now + 5.f; s_next[0] = 0;
            wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%lszt2mod_builds\\install_next.txt", config::GameDir());
            if (FILE* f = _wfopen(path, L"rb")) { size_t n = fread(s_next, 1, sizeof s_next - 1, f); s_next[n] = 0; fclose(f);
                for (char* c = s_next; *c; ++c) if (*c == '\r' || *c == '\n' || *c == ' ') { *c = 0; break; } }
        }
        if (s_next[0] && strcmp(s_next, padinput::RunningBuild())) {
            static char msg[128]; snprintf(msg, sizeof msg, "New build %s is ready - close the game and start it again (running %s)", s_next, padinput::RunningBuild());
            const int fpx = H / 34;
            RECT b = { W / 6, H / 60, W - W / 6, H / 60 + fpx * 2 };
            overlay::Line l = { msg, b, D3DCOLOR_ARGB(255, 255, 255, 255), D3DCOLOR_ARGB(200, 160, 30, 30) };
            overlay::Panel(dev, b, 0, &l, 1, fpx);
        }
    }
    {   // ZT18e: the game's zookeeper first person (mode_super_staff) is player 1's
        const char* fm = p2tools::CurMode(); g_gamePhoto = !strcmp(fm, "mode_photo_safari");
        bool fpMode = !strcmp(fm, "mode_super_staff") || g_gamePhoto;   // ZT18f: the game's photo mode walks the same way
        if (fpMode != g_gameFp) {
            g_gameFp = fpMode; g_fp = fpMode;
            if (fpMode) padui::Leave(0);
            else { Key('W', false); Key('A', false); Key('S', false); Key('D', false); Key(VK_SHIFT, false); Mouse(true, false); }
            LOG("pad: zookeeper first person (game) %s for player 1", fpMode ? "on" : "off");
        }
    }
    {   // ZT18f: the game's photo mode (mode_photo_safari, part of its first person = player 1): player 1 full screen so the
        // game's photo frame fits; player 2 waits for it (one player at a time with the game's camera)
        bool photo = !strcmp(p2tools::CurMode(), "mode_photo_safari");
        split::SetSolo(photo && split::Active());
    }
    // ZT18f: player 2's own first person also on keyboard + mouse (kbm2); it ends when split ends or player 2 has no input
    if (split::P2InFirstPerson() && (!split::Active() || (!kbm2::Active() && !g_p2.Connected()))) split::P2FirstPerson(false);
    if (!split::P2InFirstPerson()) { if (g_p2Photo) P2PhotoOff(); animal::KeeperRelease(); }
    if (split::Active() && split::Solo()) {
        // player 2 waits while player 1 has the game's photo mode (player 2's half is not drawn)
    } else if (split::Active() && g_pauseNav) {
        // ZT19u (owner: player 2 could still move while player 1 paused): the pause window holds player 2 too - no camera,
        // no tools; a ridden animal gets no input (it stands)
        if (animal::Riding(1)) animal::Frame(1, animal::In(), 0.f, now, false, 0.f, 0.f, false);
    } else if (split::Active() && kbm2::Active()) {
        kbm2::Frame(dev, hwnd, dt, now, W, H);   // player 2 on keyboard + mouse (their pad is ignored)
    } else if (split::Active() && g_p2.Connected()) {
        const XPad& q = g_p2;
        p2tools::SetPointer(W * 3 / 4, H / 2);   // player 2 aims with the middle of the right half
        float ax = 0, ay = 0; bool aim = split::P2Aim(ax, ay);
        bool menu2 = padui::Active(1);
        if (!menu2 && !animal::Riding(1) && !p2tools::Busy() && !split::P2InFirstPerson() && q.Pressed(PAD_B)) InfoClose(1, now, W, H, hwnd);   // ZT19w
        bool riding2 = animal::Riding(1) || (!menu2 && !p2tools::Busy() && !split::P2InFirstPerson() && animal::Frame(1, animal::FromPad(q), dt, now, aim, ax, ay, false));
        if (riding2 && animal::Riding(1)) animal::Frame(1, animal::FromPad(q), dt, now, aim, ax, ay, false);
        if (!animal::Riding(1)) {
            // the game's own panels: X / LT / Y / RT open one, the highlight takes the pad
            if (const char* mp = g_p2Photo ? nullptr : MainPanel(1, q)) {   // ZT18f: LT / RT zoom in player 2's camera
            animal::KeepUI(now);   // ZT17d: the menu buttons may be hidden in a rider's half
                padui::Result r = padui::Open(1, mp, now, W, H);
                if (r.act == padui::CLICK) PostClick(hwnd, r.x, r.y);
            }
            padui::Result r = padui::Frame(1, q, now, W, H);
            if (r.act == padui::CLICK) {
                // ZT18e: player 2's zookeeper button starts player 2's OWN first person (the game's mode stays player 1's)
                if (padui::TakeClick(1, "firstperson", now)) { if (!animal::Riding(1)) { P2PhotoOff(); split::P2FirstPerson(!split::P2InFirstPerson()); } }
                else if (padui::TakeClick(1, "photo", now)) P2PhotoToggle(now);   // ZT18f: player 2's own camera
                else PostClick(hwnd, r.x, r.y);
            }
            if (r.wheel) { int sx, sy; if (padui::SelPx(1, W, H, sx, sy)) PostWheel(hwnd, sx, sy, r.wheel); }   // ZT18f: list scroll bar follows
            menu2 = padui::Active(1) || r.used;
            bool toolUsed = g_p2Photo ? false : p2tools::Frame(dev, q, dt, now, W, H, hwnd, menu2);
            if (split::P2InFirstPerson()) {   // ZT18e: LS walk (full push = run), RS look, B / Start / LS click = leave
                if (g_p2Photo && !menu2) {   // ZT18f: player 2's camera - A photo, RT / LT zoom, B back to walking, Start = all off
                    if (q.Pressed(PAD_A)) P2PhotoSnap(now);
                    float zr = q.RT() - q.LT(); if (fabsf(zr) > 0.1f) P2PhotoZoom(1.f + zr * 1.6f * dt);
                    if (q.Pressed(PAD_START)) { P2PhotoOff(); split::P2FirstPerson(false); }
                    else if (q.Pressed(PAD_B)) P2PhotoOff();
                    else { split::P2FpInput(q.LX() * 0.5f, q.LY() * 0.5f, q.RX() / g_p2Zoom, q.RY() / g_p2Zoom, dt, false);
                           P2FpBody(dt, now, fabsf(q.LX()) > 0.1f || fabsf(q.LY()) > 0.1f); }
                    goto p2done;
                }
                bool leave = !menu2 && (q.Pressed(PAD_START) || q.Pressed(PAD_LS) || (q.Pressed(PAD_B) && !toolUsed && !p2tools::Busy()));
                if (leave) split::P2FirstPerson(false);
                else if (!menu2 && !toolUsed) { split::P2FpInput(q.LX(), q.LY(), q.RX(), q.RY(), dt, fabsf(q.LX()) > 0.95f || fabsf(q.LY()) > 0.95f);
                                                P2FpBody(dt, now, fabsf(q.LX()) > 0.1f || fabsf(q.LY()) > 0.1f); }
                else P2FpBody(dt, now, false);
                if (c.marker && !menu2) overlay::DrawMarker(dev, W * 0.75f, H * 0.5f, c.markerSize * H * 0.4f, now);
                goto p2done;
            }
            {
            float rot, zoom; SplitRS(q.RX(), q.RY(), rot, zoom);
            { float tz = TriggerZoom(1, q); if (fabsf(tz) > 0.2f) zoom = tz > 0.f ? 1.f : -1.f; }
            zoom = -zoom;   // RS: left/right turn, up = zoom in
            float tilt = (q.Down(PAD_DUP) ? 1.f : 0.f) - (q.Down(PAD_DDOWN) ? 1.f : 0.f);
            if (menu2 || toolUsed || q.Down(PAD_RB)) tilt = 0.f;
            if (menu2) split::P2Input(0.f, 0.f, rot, zoom, dt, 0.f);
            else split::P2Input(q.LX(), q.LY(), rot, zoom, dt, tilt);
            if (c.marker && !menu2) overlay::DrawMarker(dev, W * 0.75f, H * 0.5f, c.markerSize * H, now);
            }
            p2done:;
        }
    } else { if (animal::Riding(1)) animal::Release(1); if (p2tools::Busy()) p2tools::Reset(); if (padui::Active(1)) padui::Leave(1); }
    if (!split::Solo()) P2PhotoDraw(dev, now, W, H);   // ZT18f
    {   // player 1: RB + D-pad Up on the pad, or T on the keyboard = take / let go of the animal at the aim
        static bool s_tPrev = false;
        bool tNow = GetForegroundWindow() == hwnd && (GetAsyncKeyState('T') & 0x8001);
        bool keyT = tNow && !s_tPrev; s_tPrev = tNow;
        // keyboard riding (player 1): I/K forward/back, J/L left/right, Shift run, Y attack, H special, O call,
        // Z eat/drink, B lie/sleep/stand, V sniff (none of them is a game hotkey, also with Shift)
        animal::In in = animal::FromPad(g_p1);
        if (padui::Active(0)) in = animal::In();   // the pad is in the menus
        if (animal::Riding(0) && GetForegroundWindow() == hwnd) {
            static bool s_k[8];
            auto K = [](int vk) { SHORT st = GetAsyncKeyState(vk); return (st & 0x8001) != 0; };   // held, or tapped since the last poll
            in.ly += (K('I') ? 1.f : 0.f) - (K('K') ? 1.f : 0.f);
            in.lx += (K('L') ? 1.f : 0.f) - (K('J') ? 1.f : 0.f);
            in.runHeld = K(VK_SHIFT);
            const int keys[6] = { 'Y', 'H', 'O', 'Z', 'B', 'V' };
            const int btns[6] = { -1, -2, PAD_Y, PAD_A, PAD_X, -3 };   // ZT14: B key = lie/sleep (pad X); V unused
            for (int i = 0; i < 6; ++i) {
                bool d = K(keys[i]);
                if (btns[i] >= 0) { in.down[btns[i]] |= d; in.prev[btns[i]] |= s_k[i]; }
                else if (btns[i] == -1) { if (d) in.rt = 1.f; }
                else if (btns[i] == -2) { if (d) in.lt = 1.f; }
                s_k[i] = d;
            }
        }
        if (animal::Frame(0, in, dt, now, false, 0.f, 0.f, keyT)) { ReleaseAll(); padui::Leave(0); return; }
    }
    if (!g_p1.Connected() || GetForegroundWindow() != hwnd) { ReleaseAll(); padui::Leave(0); return; }
    const XPad& p = g_p1;
    {   // who is in control: pad or mouse/keyboard
        bool any = p.LX() || p.LY() || p.RX() || p.RY() || p.LT() || p.RT();
        for (int b = 0; b < PAD_COUNT && !any; ++b) any = p.Down((PadButton)b);
        POINT cur; GetCursorPos(&cur);
        if (g_padActive && g_setPos.x >= 0 && (abs(cur.x - g_setPos.x) > 2 || abs(cur.y - g_setPos.y) > 2) && !any) {
            g_padActive = false; ReleaseAll(); padui::Leave(0); LOG("pad: mouse moved - mouse/keyboard in control");
        }
        if (any && !g_padActive) { g_padActive = true; g_setPos = {-1, -1}; LOG("pad: controller in control"); }
        if (!g_padActive) return;
    }

    // ZT14: menu screens (title, main menu, load, options, map choice - no zoo loaded): the game has no Lua there, so the
    // frame can't list buttons. Left stick / D-pad move the real pointer (the game's own hover highlight follows it),
    // A = click, B / Start = back (Esc). Works in both pad styles.
    {
        const char* m = p2tools::CurMode();
        bool menus = !m[0] || !strcmp(m, "nil") || !strncmp(m, "mode_main", 9) || !strcmp(m, "mode_load") ||
                     !strcmp(m, "mode_options") || !strcmp(m, "mode_download");
        if (g_pauseNav && g_pauseCheckAt < 0.f && strcmp(m, "mode_options")) { g_pauseNav = false; padui::Leave(0); }   // ZT19n: pause window closed
        // ZT19u (owner video: the pause highlight blinked on and off): the zoo's pause keeps its mode, so the line above
        // cleared the ZT19r highlight every frame and the check put it back every 0.3 s - it now only handles the mode_options pause
        { static bool s_was = false; if (menus != s_was) { s_was = menus; if (menus) GoScreen(MS_MAIN); LOG("pad: menu screens %s (mode '%s')", menus ? "- sticks move the pointer, A clicks, B back" : "left", m); } }
        ClickTick();   // ZT19p
        if (menus) {
            { static const int kvk[10] = { 'W', 'A', 'S', 'D', VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_ADD, VK_SUBTRACT }; for (int vk : kvk) Key((BYTE)vk, false); }
            {   // ZT18h: did the screen open? (see g_vfBtn)
                RECT top{ (LONG)(200 * W / 1024), (LONG)(3 * H / 768), (LONG)(820 * W / 1024), (LONG)(26 * H / 768) };
                if (g_ms == MS_MAIN && g_vfBtn < 0 && GetTickCount() - g_vfMainAt >= 500) {
                    float prev[3] = { g_vfMain[0], g_vfMain[1], g_vfMain[2] }; bool had = g_vfMainOk;
                    g_vfMainAt = GetTickCount(); g_vfMainOk = overlay::SampleRect(dev, top, g_vfMain);
                    // ZT19m (owner videos 11-19: at the start the highlight was not on the main menu's buttons until a
                    // menu was opened and closed): the pointer was placed while the intro / title still showed, and the
                    // game only lights a button when the pointer moves over it. When the picture changes (movie ->
                    // title -> main menu) the highlight is put on its button again.
                    if (had && g_vfMainOk && fabsf(prev[0] - g_vfMain[0]) + fabsf(prev[1] - g_vfMain[1]) + fabsf(prev[2] - g_vfMain[2]) > 25.f) {
                        g_picChangeAt = GetTickCount();
                        g_msPointAt = GetTickCount() + 300; g_msPointN = 0; LOG("pad: menus - the picture changed, highlight put back on '%s'", MsBtn(g_msSel).name); }
                }
                if (g_vfBtn >= 0 && g_vfNeedBefore) {   // the press was this frame or the last: use the last main-menu sample
                    g_vfNeedBefore = false;
                    if (g_vfMainOk) for (int i = 0; i < 3; ++i) g_vfBefore[i] = g_vfMain[i]; else g_vfBtn = -1;
                    g_vfMainOk = false; }
                else if (g_vfBtn >= 0 && ++g_vfFrames >= 45 && GetTickCount() - g_vfAt >= 3000) {
                    float a[3]; int idx = g_vfBtn; g_vfBtn = -1;
                    if (overlay::SampleRect(dev, top, a)) {
                        float dd = fabsf(a[0] - g_vfBefore[0]) + fabsf(a[1] - g_vfBefore[1]) + fabsf(a[2] - g_vfBefore[2]);
                        LOG("pad: menus - '%s' screen check: top strip change %.1f", kMain[idx].name, dd);
                        if (dd < 12.f && g_vfTries < 1 && (g_ms == kMain[idx].next)) {
                            ++g_vfTries; g_vfBtn = idx; g_vfNeedBefore = false; g_vfFrames = 0; g_vfAt = GetTickCount();
                            MBtn k = kMain[idx]; ClickAbs(hwnd, (int)((k.x + k.w / 2) * W / 1024.f), (int)((k.y + k.h / 2) * H / 768.f));
                            Mouse(false, false); g_msPointAt = GetTickCount() + 400; g_msPointN = 0;
                            LOG("pad: menus - the screen did not open, pressing '%s' again", k.name);
                        }
                    }
                }
            }
            // ZT19n (owner photo 11-19: Start in the zoo shows the pause window - Return to Main Menu / Save / Load / Exit /
            // Return to Game - and the pad only moved a free pointer): in the zoo the game's Lua is alive, so the pad's
            // highlight goes into the window on its TOP button; stick / D-pad move it, A presses, B = Return to Game
            if (!strcmp(m, "mode_options") && luabridge::Ready()) {
                static float s_tryAt = 0.f;
                if (!padui::Active(0) && now >= s_tryAt) {
                    s_tryAt = now + 0.5f; char o[96]; float x, y, w, h;
                    if (luabridge::Run("local w=queryObject('UIRoot'):UI_GET_CHILD('In Game Options') if not w then return '' end "
                                       "local q=w:UI_GET_SCREEN_RECT() if not q then return '' end return string.format('%.0f %.0f %.0f %.0f',q.x,q.y,q.w,q.h)", o, sizeof o)
                        && sscanf(o, "%f %f %f %f", &x, &y, &w, &h) == 4 && w > 0.f)
                        { padui::EnterWindow(0, now, x, y, w, h); g_pauseNav = true; }
                }
                if (padui::Active(0)) {
                    padui::Result r = padui::Frame(0, p, now, W, H);
                    if (r.act == padui::CLICK) { ReleaseAll(); SetPointer(hwnd, r.x, r.y); Mouse(true, true); Mouse(true, false); }
                    return;
                }
            }
            if (MenuNav(hwnd, p, now, W, H)) return;   // ZT17d: highlight jumps between the game's own buttons
            float sx = p.LX() + (p.Down(PAD_DRIGHT) ? 1.f : 0.f) - (p.Down(PAD_DLEFT) ? 1.f : 0.f);
            float sy = p.LY() + (p.Down(PAD_DUP) ? 1.f : 0.f) - (p.Down(PAD_DDOWN) ? 1.f : 0.f);
            // ZT18d: right stick up/down = mouse wheel on the menu screens (map / save lists), left/right moves the pointer
            bool wheel = fabsf(p.RY()) > fabsf(p.RX()) && WheelStick(p.RY(), now);
            if (!wheel && fabsf(p.RX()) > fabsf(sx)) sx = p.RX();
            POINT cur; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
            if (g_px < 0 || (sx == 0.f && sy == 0.f)) { g_px = (float)cur.x; g_py = (float)cur.y; }
            if (sx != 0.f || sy != 0.f) {
                float mag = sqrtf(sx * sx + sy * sy); if (mag > 1.f) { sx /= mag; sy /= mag; mag = 1.f; }
                float sp = 650.f * (H / 1080.f) * (0.2f + 0.8f * mag * mag);   // ZT17: calmer, fine control near the middle
                g_px += sx * sp * dt; g_py -= sy * sp * dt;
                if (g_px < 0) g_px = 0; if (g_py < 0) g_py = 0; if (g_px > W - 1) g_px = (float)W - 1; if (g_py > H - 1) g_py = (float)H - 1;
                SetPointer(hwnd, (int)g_px, (int)g_py);
            }
            Mouse(true, p.Down(PAD_A));
            Mouse(false, false);
            if (p.Pressed(PAD_B) && !p.Pressed(PAD_START)) Tap(VK_ESCAPE);
            return;
        }
    }
    if (g_gameFp) g_fp = true;   // ZT18e: player 1 in the game's zookeeper first person
    const int cx = split::Active() ? W / 4 : W / 2, cy = H / 2;   // split: player 1 aims at the middle of the left half

    // --- style toggle (right-stick click) ---
    // ZT19 (owner: the pointer showed up in controller mode): a quick right-stick click while turning the camera switched to
    // PC style by accident (log 11-18: 4 switches in one session) - now the right stick must be held in for 0.4 s (ZT19d: 1 s felt too long)
    static float s_rsAt = -1.f;
    if (p.Pressed(PAD_RS)) s_rsAt = now;
    if (!p.Down(PAD_RS)) s_rsAt = -1.f;
    if (s_rsAt >= 0.f && now - s_rsAt >= 0.4f) { s_rsAt = -1.f; c.consoleStyle = !c.consoleStyle; g_locked = c.consoleStyle; padui::Leave(0);
        LOG("pad: %s style", c.consoleStyle ? "console (aim circle)" : "PC (free pointer)"); }

    // --- guest walk (first person) ---
    if (p.Pressed(PAD_LS)) { Tap('G'); g_fp = !g_fp; padui::Leave(0); LOG("pad: guest walk %s", g_fp ? "on" : "off"); }
    if (p.Pressed(PAD_START)) { Tap(VK_ESCAPE); padui::Leave(0); g_pauseNav = false; g_pauseCheckAt = now + 0.35f; g_pauseCheckUntil = now + 2.f; if (g_fp) { g_fp = false; LOG("pad: guest walk off (Esc)"); } }
    // ZT19r (owner photo: pause = 'Game Options' window, the highlight only came after LT / RT): ZT19n waited for
    // mode_options, but the zoo's pause keeps its mode. After Start the window itself is checked; when it shows, the
    // highlight goes onto its TOP button. When it closes, the pad is back in the world.
    // ZT20 (owner: ZT19z - the pad stuck in the menus, map would not move; log 12:59: 'dialog (Confirm)' at 0,0 1024x768 over
    // and over - the hidden Confirm Dialog counted as shown): dialogs only when really visible AND smaller than the screen.
    // ZT19y (owner photo: after Exit Game in the pause menu, the 'Do you wish to Save and Exit?' dialog's three buttons got
    // no highlight - the pause window closed, the pad left it): the game's pause window AND its dialogs (Confirm / Alert)
    // are watched all the time (0.3 s after Start / while one is open, else every 0.6 s); whichever shows gets the highlight
    {
        static int s_dlg = -1;
        if (g_pauseCheckAt < 0.f) g_pauseCheckAt = now + 0.6f;
        if (now >= g_pauseCheckAt && luabridge::Ready() && luabridge::Idle()) {
            g_pauseCheckAt = now + ((g_pauseNav || now < g_pauseCheckUntil) ? 0.3f : 0.6f);
            char o[96]; float x = 0, y = 0, w = 0, h = 0; int idx = -1;
            bool shown = luabridge::Run(
                "local r=queryObject('UIRoot') if not r then return '' end "
                "for i,n in ipairs({'Confirm Dialog','Alert Dialog','In Game Options'}) do local w=r:UI_GET_CHILD(n) "
                " if w then local ok,v=pcall(function() return w:UI_GET_LINEAGE_VISIBLE() end) "
                "  if (i<3 and ok and v==true) or (i==3 and (not ok or v)) then local q=w:UI_GET_SCREEN_RECT() if q and q.w and q.w>0 and (i==3 or q.w<1000) then "
                "   return string.format('%d %.0f %.0f %.0f %.0f',i,q.x,q.y,q.w,q.h) end end end end return ''", o, sizeof o)
                && sscanf(o, "%d %f %f %f %f", &idx, &x, &y, &w, &h) == 5 && w > 0.f;
            static const char* kN[4] = { "", "dialog (Confirm)", "dialog (Alert)", "pause window" };
            if (shown && (!g_pauseNav || idx != s_dlg || !padui::Active(0))) {
                padui::EnterWindow(0, now, x, y, w, h); g_pauseNav = true; s_dlg = idx;
                LOG("pad: %s - highlight on its buttons", kN[idx > 0 && idx < 4 ? idx : 0]);
            } else if (!shown && g_pauseNav) { g_pauseNav = false; s_dlg = -1; padui::Leave(0); LOG("pad: pause window / dialog closed"); }
        }
    }

    if (c.consoleStyle && !g_fp) {
        // ===== console style (JPOG layout) =====
        if (const char* mp = MainPanel(0, p)) {
            animal::KeepUI(now);   // ZT17d: the menu buttons may be hidden in a rider's half
            padui::Result r = padui::Open(0, mp, now, W, H);
            if (r.act == padui::CLICK) { SetPointer(hwnd, r.x, r.y); Mouse(true, true); Mouse(true, false); }
        }
        padui::Result r = padui::Frame(0, p, now, W, H);
        bool menu = padui::Active(0) || r.used;
        if (r.act == padui::CLICK) { ReleaseAll(); SetPointer(hwnd, r.x, r.y); Mouse(true, true); Mouse(true, false); }
        if (r.wheel) { int sx, sy; if (padui::SelPx(0, W, H, sx, sy)) SetPointer(hwnd, sx, sy); Wheel(r.wheel); }   // ZT18d
        // camera: LS pan (not while the highlight uses it), RS turn + zoom, D-pad up/down tilt
        { float px = menu ? 0.f : p.LX(), py = menu ? 0.f : p.LY(); Square(px, py);
          if (g_camRot) { px = 0.f; py = 0.f; }   // Ctrl is held while the map turns: no Ctrl+W/A/S/D
          Axis(px, 'A', 'D', now); Axis(-py, 'W', 'S', now); }
        // ZT18d: in a menu the right stick up/down scrolls the list under the highlight (mouse wheel) instead of zooming
        bool rsWheel = menu && (r.scrolled || (fabsf(p.RY()) > fabsf(p.RX()) && WheelStick(p.RY(), now)));   // padui scrolls lists itself
        { float rr, zz; SplitRS(rsWheel ? 0.f : p.RX(), rsWheel ? 0.f : p.RY(), rr, zz);
          float tz = TriggerZoom(0, p); if (fabsf(tz) > 0.2f) zz = tz > 0.f ? 1.f : -1.f;
          CamRotate(rr);   // ZT17b: the game's own rotate buttons' message (arrow keys did not turn the map)
          Axis(zz, VK_SUBTRACT, VK_ADD, now); }         // stick up = zoom in ('+' = camera down)
        bool dpadCam = !menu && !p.Down(PAD_RB);
        Key(VK_UP, dpadCam && p.Down(PAD_DUP)); Key(VK_DOWN, dpadCam && p.Down(PAD_DDOWN));
        Key(VK_SHIFT, false);
        if (!menu) {
            // rotate the object being placed: D-pad left/right or LB/RB
            if (Repeat(0, p.Down(PAD_DLEFT) || p.Down(PAD_LB), p.Pressed(PAD_DLEFT) || p.Pressed(PAD_LB), now)) Tap(VK_OEM_COMMA);
            if (Repeat(1, (p.Down(PAD_DRIGHT) || p.Down(PAD_RB)) && !p.Down(PAD_DUP), (p.Pressed(PAD_DRIGHT) || p.Pressed(PAD_RB)) && !p.Down(PAD_DUP), now)) Tap(VK_OEM_PERIOD);
            SetPointer(hwnd, cx, cy);                    // aim = middle of the player's view
            // ZT17: the pointer stays in the middle while the stick moves the camera, so Windows sends no mouse moves and the
            // tools (fence / path end point, object ghost, terrain brush) never follow the circle. Tell the game every frame.
            PostMessageW(hwnd, WM_MOUSEMOVE, kbm2::kOurs | (p.Down(PAD_A) ? MK_LBUTTON : 0), MAKELPARAM(cx, cy));
            Mouse(true, p.Down(PAD_A));                  // hold A = drag (paths, fences, terrain)
            // B = right click = cancel the tool / deselect - but never player 2's tool (ZT17d: it cancelled their fence)
            static bool s_bInfo = false;   // ZT19w: B closes an open info window first (no right click for that press)
            if (p.Pressed(PAD_B) && InfoClose(0, now, W, H, hwnd)) s_bInfo = true;
            if (!p.Down(PAD_B)) s_bInfo = false;
            Mouse(false, p.Down(PAD_B) && !s_bInfo && !p2tools::GameToolIsP2());
            if (p.Pressed(PAD_BACK)) Tap(VK_DELETE);     // delete tool
            if (c.marker) overlay::DrawMarker(dev, (float)cx, (float)cy, c.markerSize * H, now);
        } else {
            Mouse(false, false);
            // the real pointer sits on the highlighted button (the game's own hover + tooltip)
            if (r.act != padui::CLICK && padui::SelPx(0, W, H, r.x, r.y)) SetPointer(hwnd, r.x, r.y);
        }
        return;
    }

    // ===== PC style (right-stick click toggles): LS = pointer, RS = camera; guest walk =====
    float mvx, mvy, ptx, pty;   // camera-move stick, pointer stick
    if (g_fp) { mvx = p.LX(); mvy = p.LY(); ptx = p.RX(); pty = p.RY(); }
    else      { mvx = p.RX(); mvy = p.RY(); ptx = p.LX(); pty = p.LY(); }
    // ZT18d: PC style, the pointer on the game's UI (a list / panel): right stick up/down = mouse wheel, not the camera
    if (!g_fp) { POINT pc; GetCursorPos(&pc); ScreenToClient(hwnd, &pc);
        if (padui::OverUI(pc.x, pc.y, W, H, now, nullptr, 0, nullptr) && fabsf(mvy) > fabsf(mvx) && WheelStick(mvy, now)) { mvx = 0.f; mvy = 0.f; } }
    Square(mvx, mvy);
    Axis(mvx, 'A', 'D', now); Axis(-mvy, 'W', 'S', now);   // stick up = forward (W)
    Key(VK_SHIFT, g_fp && (fabsf(mvx) > 0.95f || fabsf(mvy) > 0.95f));   // run when pushed all the way (guest walk)
    if (g_fp) {
        // ZT17c: shooter-style look - RS moves the mouse like a real mouse would (the game's own first-person mouse look),
        // smooth and analog with a curve, instead of tapping the arrow keys at one speed
        float m = sqrtf(ptx * ptx + pty * pty);
        if (m > 0.f) {
            float k = (m > 1.f ? 1.f : m); k = k * k / (m > 0.001f ? m : 1.f);
            static float s_fx, s_fy;
            float sp = c.fpLookSpeed * (H / 1080.f) * dt;
            s_fx += ptx * k * sp; s_fy += -pty * k * sp;
            int ix = (int)s_fx, iy = (int)s_fy; s_fx -= ix; s_fy -= iy;
            if (ix || iy) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dx = ix; in.mi.dy = iy; in.mi.dwFlags = MOUSEEVENTF_MOVE; SendInput(1, &in, sizeof in); }
        }
        Key(VK_LEFT, false); Key(VK_RIGHT, false); Key(VK_UP, false); Key(VK_DOWN, false);
    } else {
        bool pointerMoved = fabsf(ptx) > 0.f || fabsf(pty) > 0.f;
        POINT cur; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
        if (g_px < 0 || !pointerMoved) { g_px = (float)cur.x; g_py = (float)cur.y; }   // follow a real mouse too
        if (pointerMoved) {
            float sp = c.pointerSpeed * (H / 1080.f);
            float pm = sqrtf(ptx * ptx + pty * pty); if (pm > 1.f) pm = 1.f;   // ZT10: same speed in every direction
            pm = pm * pm;   // ZT17: curve - small pushes move the pointer slowly
            g_px += ptx * pm * sp * dt; g_py -= pty * pm * sp * dt;
            if (g_px < 0) g_px = 0; if (g_py < 0) g_py = 0; if (g_px > W - 1) g_px = (float)W - 1; if (g_py > H - 1) g_py = (float)H - 1;
            SetPointer(hwnd, (int)g_px, (int)g_py);
        }
        Key(VK_LEFT, p.LT() > 0.f && Pwm(p.LT(), now)); Key(VK_RIGHT, p.RT() > 0.f && Pwm(p.RT(), now));
        Key(VK_UP, p.Down(PAD_DUP)); Key(VK_DOWN, p.Down(PAD_DDOWN));
    }
    Key(VK_SUBTRACT, p.Down(PAD_LB)); Key(VK_ADD, p.Down(PAD_RB));
    if (Repeat(0, p.Down(PAD_DLEFT), p.Pressed(PAD_DLEFT), now)) Tap(VK_OEM_COMMA);
    if (Repeat(1, p.Down(PAD_DRIGHT), p.Pressed(PAD_DRIGHT), now)) Tap(VK_OEM_PERIOD);
    if (g_gamePhoto) {   // ZT18f: the game's photo mode (hotkeys.xml photomode): A / RT = Space (take the photo), B = Esc (back),
        // LB / RB = '-' / '+' zoom (above), no mouse buttons
        static bool s_rt = false; bool rt = p.RT() > 0.5f;
        if (p.Pressed(PAD_A) || (rt && !s_rt)) Tap(VK_SPACE);
        s_rt = rt;
        if (p.Pressed(PAD_B)) Tap(VK_ESCAPE);
        Mouse(true, false); Mouse(false, false);
    } else {
    Mouse(true, p.Down(PAD_A) || (g_fp && p.RT() > 0.5f));   // guest walk: RT = click too
    static bool s_bInfoPc = false;   // ZT19w
    if (!g_fp && p.Pressed(PAD_B) && InfoClose(0, now, W, H, hwnd)) s_bInfoPc = true;
    if (!p.Down(PAD_B)) s_bInfoPc = false;
    Mouse(false, p.Down(PAD_B) && !s_bInfoPc && !p2tools::GameToolIsP2());
    }
    if (p.Pressed(PAD_X)) Tap('C', true);      // Construction
    if (p.Pressed(PAD_Y)) Tap('M', true);      // Animals
    if (p.Pressed(PAD_BACK)) Tap(VK_DELETE);
}
}

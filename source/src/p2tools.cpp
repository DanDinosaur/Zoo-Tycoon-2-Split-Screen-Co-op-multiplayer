#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "p2tools.h"
#include "xinput_pad.h"
#include "overlay.h"
#include "luabridge.h"
#include "split.h"
#include "log.h"
#include "kbm2.h"
#include "padui.h"
#include "privtool.h"

namespace {
    enum Kind { K_PLACE, K_FENCE, K_PATH, K_DELETE, K_PAINT };   // K_PAINT = terrain brush / land shaping (hold A)
    struct Cat { const char* label; const char* base; Kind kind; const char* filter; std::vector<std::string> items; bool loaded = false; };
    Cat g_cats[] = {
        { "Animals",   "animal",     K_PLACE,  "_Adult_" },
        { "Buildings", "building",   K_PLACE,  nullptr },
        { "Scenery",   "scenery",    K_PLACE,  nullptr },
        { "Plants",    "foliage",    K_PLACE,  nullptr },
        { "Rocks",     "rock",       K_PLACE,  nullptr },
        { "Enrichment","enrichment", K_PLACE,  nullptr },
        { "Food",      "food",       K_PLACE,  nullptr },
        { "Fences",    "fence",      K_FENCE,  nullptr },
        { "Paths",     "path",       K_PATH,   nullptr },
        { "Delete",    nullptr,      K_DELETE, nullptr },
    };
    constexpr int kCats = sizeof g_cats / sizeof g_cats[0];

    bool g_menu = false;
    int g_cat = 0, g_item[kCats] = {};
    // tool state
    bool g_tool = false; Kind g_kind = K_PLACE; std::string g_name; const char* g_mode = "";
    int g_startStep = 0;
    bool g_aimHeld = false;   // ZT08: player 2 owns the game's tool -> every pick of the tool uses player 2's aim
    bool g_seenMode = false;      // the mode poll has seen our tool active once (before that a mismatch is just poll lag)          // 1 = mode set, waiting a frame for the object; 2 = running
    bool g_drag = false;          // fence / path: started with A, finished with the next A
    HWND g_hwnd = nullptr;
    int g_cx = 0, g_cy = 0;       // middle of player 2's half (client px)
    bool g_ptrSet = false;
    int g_pickHold = 0;           // frames to keep player 2's pick on after the drag ends
    // Paths / fences read the held mouse button (not the scripted UI messages): drag with real window messages
    // while player 2's pick is held on, so the drag runs from player 2's aim.
    bool g_privPost = false;
    bool g_aArmed = false;     // ZT18b: A let go since player 2's tool started
    float s_privTry = 0.f;     // next private-copy attempt (ZT18b: reset by a new pick)
    void PostMouse(UINT msg, WPARAM w) { if (!g_hwnd) return; if (g_privPost) privtool::NotePost(MAKELPARAM(g_cx, g_cy));
        PostMessageW(g_hwnd, msg, w | kbm2::kOurs | (g_privPost ? kbm2::kP2 : 0), MAKELPARAM(g_cx, g_cy)); }
    float g_rot = 0.f;
    char g_curMode[64] = "";
    char g_hint[160] = ""; float g_hintUntil = 0.f;

    bool Lua(const char* code, char* out = nullptr, int n = 0);
    bool LuaRaw(const char* code) { return Lua(code); }
    bool Lua(const char* code, char* out, int n) {
        static char scratch[256];
        if (!out) { out = scratch; n = sizeof scratch; }
        if (!luabridge::Ready() || !luabridge::Idle()) return false;
        bool ok = luabridge::Run(code, out, n);
        if (!ok) LOG("p2tools: lua error: %s", out);
        return ok;
    }
    // ===== ZT15: both players keep their own build tool =====
    // ZT2 has ONE tool (mode) for the whole game. Each player's tool is remembered (mode + item name); the game holds
    // the tool of whoever acted last. When the other player presses A / clicks in the world, their tool goes back in
    // first (mode, one frame later the item - engine rule) and the press follows a few frames later. Nobody is kicked out.
    struct Rec { std::string mode, name; };
    Rec g_p1;                       // player 1's tool ("" / mode_selection = none)
    int g_owner = 0;                // whose tool the game holds: 0 = player 1, 1 = player 2
    int g_p1Swap = 0;               // player 1's tool going back in: 1 send mode, 2 send item, 3.. wait for the mode poll
    float g_p1SwapAt = 0.f;
    std::string g_expect;           // a mode WE asked for: the mode poll must not count it as a player's choice
    float g_p2ActAt = -100.f;       // player 2's last world click / panel click (a mode change right after is theirs)
    float g_p1ActAt = -100.f;
    std::string g_p1Pending; float g_p1PendingAt = -100.f;   // item player 1 just clicked in the panel
    bool g_resume = false; int g_resumeStep = 0;            // player 2's tool going back in
    bool g_pendingA = false;
    bool g_priv = false;            // ZT16: player 2's fence runs on their own private copy of the game's fence tool        // player 2 pressed A while the game held player 1's tool
    float g_now = 0.f; int g_W = 1920, g_H = 1080;
    bool g_defer = false, g_deferUp = false; int g_deferX = 0, g_deferY = 0, g_deferWait = 0;   // player 1's world click held back
    int g_selPending = 0;           // player 2's select click waits for selection mode
    POINT g_lastCur{ -1, -1 };
    bool IsTool(const std::string& m) { return !m.empty() && m != "nil" && m != "mode_selection" && m.compare(0, 9, "mode_main") && m != "mode_load"; }
    const char* ItemMsg(const std::string& m) {
        return m == "mode_placement" ? "ZT_SETPLACEMENTOBJECT" : m == "mode_fence_placement" ? "ZT_SETPLACEMENTFENCE" :
               m == "mode_path_placement" ? "ZT_SETPLACEMENTPATH" : nullptr;
    }
    bool LuaRaw(const char* code);
    void SendMode(const std::string& m) {
        char code[200]; snprintf(code, sizeof code, "local m=queryObject('ZTMode') if m then m:sendMessage('ZT_SETMODE','%s') end", m.c_str());
        g_expect = m; LuaRaw(code);
    }
    void SendItem(const std::string& m, const std::string& name) {
        const char* msg = ItemMsg(m); if (!msg || name.empty()) return;
        char code[300]; snprintf(code, sizeof code, "local m=queryObject('ZTMode') if m then m:sendMessage('%s','%s') %s end", msg, name.c_str(),
                                 m == "mode_fence_placement" ? "m:sendMessage('ZT_SETFENCEPLACEMENTMODE','rubber-band')" : "");
        LuaRaw(code);
    }
    void Hint(float now, const char* fmt, const char* a = "") { snprintf(g_hint, sizeof g_hint, fmt, a); g_hintUntil = now + 3.5f; LOG("p2tools: %s", g_hint); }

    void Load(Cat& c) {
        if (c.loaded || !c.base) { c.loaded = true; return; }
        static char out[60000];
        char code[512];
        snprintf(code, sizeof code,
            "local m=queryObject('BFGManager') local r=m and m:BFG_GET_DERIVED_LEAF_TYPES('%s') local t={} "
            "if type(r)=='table' then for k,v in pairs(r) do if type(k)=='number' then local s=type(v)=='table' and v.value or v "
            "if type(s)=='string' %s then table.insert(t,s) end end end end "
            "table.sort(t) return table.concat(t,'|')",
            c.base, c.filter ? "and string.find(s,'_Adult_',1,true)" : "");
        if (!Lua(code, out, sizeof out)) return;
        char* nl = strchr(out, '\n'); if (nl) *nl = 0;
        c.items.clear();
        for (char* s = out; *s;) { char* e = strchr(s, '|'); std::string it = e ? std::string(s, e - s) : std::string(s);
            if (!it.empty() && it != "BFStringVector") c.items.push_back(it); if (!e) break; s = e + 1; }
        c.loaded = true;
        LOG("p2tools: %s = %d types", c.label, (int)c.items.size());
    }

    // the tool's mouse move / click run with player 2's pick (camera + aim) swapped in
    void ToolMsg(const char* msgs) {
        char code[400];
        snprintf(code, sizeof code, "local mm=queryObject('ZTMainMode') if mm then %s end", msgs);
        split::SetP2Pick(true);
        Lua(code);
        if (!g_aimHeld) split::SetP2Pick(false);
    }
    void StopTool(float now, const char* why) {
        if (!g_tool) return;
        if (g_drag) { PostMouse(WM_LBUTTONUP, 0); PostMouse(WM_RBUTTONDOWN, MK_RBUTTON); PostMouse(WM_RBUTTONUP, 0); g_pickHold = 3; }
        if (g_priv) { privtool::Stop(); g_priv = false; g_privPost = false; g_tool = false; g_drag = false; g_startStep = 0; if (why) Hint(now, "%s", why); return; }
        if (g_owner == 1 && (!strcmp(g_curMode, g_mode) || !g_seenMode)) {
            if (IsTool(g_p1.mode)) { g_p1Swap = 1; g_p1SwapAt = now; }   // player 1's own tool goes back in
            else { SendMode("mode_selection"); strcpy(g_curMode, "mode_selection"); g_owner = 0; }
        }
        g_tool = false; g_drag = false; g_startStep = 0; g_pendingA = false; g_resume = false;
        if (why) Hint(now, "%s", why);
    }
    void StartTool(float now, Kind k, const std::string& name) {
        if (k == K_FENCE && privtool::Start(name.c_str())) {
            g_kind = k; g_name = name; g_drag = false; g_mode = "mode_fence_placement";
            g_tool = true; g_priv = true; g_privPost = true; g_drag = false; g_aArmed = false; g_startStep = 2; g_seenMode = true;
            Hint(now, "Player 2: %s - hold A and move to draw, let go, tap A to build, B stop", name.c_str());
            return;
        }
        g_kind = k; g_name = name; g_rot = 0.f; g_drag = false;
        g_mode = k == K_PLACE ? "mode_placement" : k == K_FENCE ? "mode_fence_placement" : k == K_PATH ? "mode_path_placement" : "mode_deletion";
        SendMode(g_mode);
        g_tool = true; g_startStep = 1; g_seenMode = false; g_owner = 1; g_p1Swap = 0; g_p2ActAt = now;
        Hint(now, k == K_DELETE ? "Player 2: Delete - A removes what you aim at, B stops" :
                  k == K_PLACE ? "Player 2: %s - A place, D-pad left/right turn, B stop" :
                                 "Player 2: %s - hold A and move to draw, then tap A to build, B stop", name.c_str());
    }
}

static float g_lastSelectAt = -100.f;
namespace p2tools {
float LastSelectAt() { return g_lastSelectAt; }
const char* CurMode() { return g_curMode; }
bool GameToolIsP2() { return g_tool && g_owner == 1; }   // ZT17d
void NoteP2Click(float now) { g_p2ActAt = now; }
void NoteP1Click(float now) { g_p1ActAt = now; }
// player 1 clicked an item in the game's panel (pad highlight or mouse): the game now holds player 1's pick
void NoteP1Pick(const char* name, float now) {
    g_p1Pending = name ? name : ""; g_p1PendingAt = now; g_p1ActAt = now;
    if (g_owner != 0) { g_owner = 0; g_p1Swap = 0; LOG("p2tools: player 1 picked '%s' - the game's tool is player 1's", g_p1Pending.c_str()); }
}
// player 1 is about to press in the world: true = the game holds player 1's tool (go ahead), false = it is going back in
bool P1Ready(float now) {
    g_p1ActAt = now;
    if (g_owner == 0 && g_p1Swap == 0) return true;
    if (g_drag || g_resumeStep) return false;              // player 2 is in the middle of a drag / swap
    if (g_p1Swap == 0) { g_p1Swap = 1; g_p1SwapAt = now; LOG("p2tools: player 1's tool (%s %s) goes back in", g_p1.mode.c_str(), g_p1.name.c_str()); }
    return false;
}
// player 1's real mouse button (also the pad's SendInput clicks) - true = swallow it for now
bool P1Mouse(bool down, int x, int y) {
    if (!split::Active()) return false;
    if (!down) { if (g_defer) { g_deferUp = true; return true; } return false; }
    char name[96]; bool item = false;
    if (padui::OverUI(x, y, g_W, g_H, g_now, name, sizeof name, &item)) {
        if (item) NoteP1Pick(name, g_now); else NoteP1Click(g_now);
        return false;
    }
    if (P1Ready(g_now)) return false;
    g_defer = true; g_deferUp = false; g_deferX = x; g_deferY = y; g_deferWait = 8;
    return true;
}
void Adopt(const char* mode, const char* item, float now) {
    if (!strcmp(mode, "mode_fence_placement")) {
        std::string nm = item && *item ? item : "";
        if (nm == "construction" || nm == "fences") nm.clear();
        if (privtool::Start(nm.c_str())) {
            g_kind = K_FENCE; g_name = nm.empty() ? "fence" : nm; g_drag = false; g_mode = "mode_fence_placement";
            g_tool = true; g_priv = true; g_privPost = true; g_drag = false; g_aArmed = false; g_startStep = 2; g_seenMode = true; g_pendingA = false;
            // the game's own tool goes straight back to player 1 (player 2's panel click had switched it)
            if (IsTool(g_p1.mode)) { g_p1Swap = 1; g_p1SwapAt = now; } else { SendMode("mode_selection"); g_owner = 0; }
            Hint(now, "Player 2: hold A and move to draw, let go, tap A to build - B stops%s");
            return;
        }
    }
    // ZT18b: a new pick (another brush / land-shaping item) while player 2 runs a private copy: drop it, so the copy is
    // made again from the game's tool that now holds the new item (SetModeName -> StartCurrent)
    if (g_priv) { privtool::Stop(); g_priv = false; g_privPost = false; g_drag = false; s_privTry = 0.f; }
    bool sharedFence = !strcmp(mode, "mode_fence_placement") && privtool::NeedsSource();
    g_owner = 1; g_p1Swap = 0; g_p2ActAt = now; g_pendingA = false; g_resume = false; g_resumeStep = 0;
    static std::string s_mode; s_mode = mode;
    g_mode = s_mode.c_str();
    g_kind = !strcmp(mode, "mode_fence_placement") ? K_FENCE : !strcmp(mode, "mode_path_placement") ? K_PATH :
             !strcmp(mode, "mode_deletion") ? K_DELETE : (!strcmp(mode, "mode_biome") || !strcmp(mode, "mode_deformation")) ? K_PAINT : K_PLACE;
    g_name = item && *item ? item : "";
    if (g_name.empty() || g_name == "construction" || g_name == "animals" || g_name == "staff" || g_name == "terraform")
        g_name = g_kind == K_FENCE ? "fences" : g_kind == K_PATH ? "paths" : g_kind == K_DELETE ? "delete" : "building";
    g_rot = 0.f; g_drag = false;
    g_tool = true; g_startStep = 2; g_seenMode = true;
    Hint(now, g_kind == K_PAINT ? "Player 2: hold A and move to paint the ground - B stops%s" :
              g_kind == K_FENCE || g_kind == K_PATH ? "Player 2: hold A and move to draw, let go, tap A to build - B stops%s" :
              g_kind == K_DELETE ? "Player 2: Delete - A removes what you aim at, B stops%s" : "Player 2: A puts it down, LB/RB turn it, B stops%s");
    if (sharedFence) {   // ZT17f (owner 11:40): say why the fence is still the shared one
        Hint(now, "Fences are shared for now - Player 1: open any fence once, then you can both build fences at the same time%s");
        g_hintUntil = now + 7.f;
    }
}
bool Busy() { return g_menu || g_tool; }
bool HasTool() { return g_tool; }
void SetPointer(int x, int y) { g_cx = x; g_cy = y; g_ptrSet = true; split::SetP2Pointer(x, y); }
void SetModeName(const char* m) {
    std::string nm = m ? m : "";
    bool changed = nm != g_curMode;
    strncpy(g_curMode, nm.c_str(), sizeof g_curMode - 1);
    if (!changed) return;
    if (!g_expect.empty() && nm == g_expect) { g_expect.clear(); return; }   // one we asked for
    // a player's choice: player 2 if they clicked in the last moment, else player 1 (mouse, hotkeys, pad)
    if (g_now - g_p2ActAt < 1.2f && g_p2ActAt > g_p1ActAt) {
        if (g_tool && nm != g_mode && !g_seenMode) return;
        if (g_tool && nm != g_mode) { LOG("p2tools: player 2's tool ended (%s)", nm.c_str()); g_tool = false; g_drag = false; g_startStep = 0; }
        g_owner = 1;
        return;
    }
    g_p1.mode = nm;
    if (g_now - g_p1PendingAt < 2.f) g_p1.name = g_p1Pending; else if (!IsTool(nm)) g_p1.name.clear();
    g_owner = 0; g_p1Swap = 0;
    LOG("p2tools: player 1's tool = %s %s", nm.c_str(), g_p1.name.c_str());
}
void Reset() { if (g_drag || g_pickHold || g_aimHeld) split::SetP2Pick(false); g_aimHeld = false; g_pickHold = 0; g_menu = false; g_tool = false; g_drag = false; g_startStep = 0; for (auto& c : g_cats) { c.loaded = false; c.items.clear(); } }

bool Frame(IDirect3DDevice9* dev, const XPad& pad, float dt, float now, int W, int H, HWND hwnd, bool padBusy) {
    (void)dt;
    g_hwnd = hwnd; if (!g_ptrSet) { g_cx = W * 3 / 4; g_cy = H / 2; }
    if (!g_tool && g_pickHold > 0 && --g_pickHold == 0) split::SetP2Pick(false);
    bool used = false;
    g_now = now; g_W = W; g_H = H;
    // player 1's item name: a panel pick while player 1 holds the game (also when the mode stays the same, e.g. bench -> bin)
    if (!g_p1Pending.empty() && now - g_p1PendingAt > 0.3f && g_owner == 0 && g_p1Swap == 0) {
        if (IsTool(g_curMode)) { g_p1.mode = g_curMode; g_p1.name = g_p1Pending; LOG("p2tools: player 1's tool = %s %s", g_curMode, g_p1.name.c_str()); }
        g_p1Pending.clear();
    }
    // player 1's held-back click goes through once their tool is back
    if (g_defer && g_owner == 0 && g_p1Swap == 0 && g_hwnd && g_deferWait > 0) {   // the tool's preview follows the pointer first
        PostMessageW(g_hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(g_deferX, g_deferY)); --g_deferWait;
    } else if (g_defer && g_owner == 0 && g_p1Swap == 0 && g_hwnd) {
        PostMessageW(g_hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(g_deferX, g_deferY));
        PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON | kbm2::kOurs, MAKELPARAM(g_deferX, g_deferY));
        if (g_deferUp) PostMessageW(g_hwnd, WM_LBUTTONUP, kbm2::kOurs, MAKELPARAM(g_deferX, g_deferY));
        LOG("p2tools: player 1's click goes through at %d,%d", g_deferX, g_deferY);
        g_defer = false;
    }
    // player 1 moves the pointer over their half while player 2 has been idle: player 1's tool (and its preview) goes back in
    { POINT c; GetCursorPos(&c); ScreenToClient(hwnd, &c);
      bool moved = g_lastCur.x >= 0 && (abs(c.x - g_lastCur.x) > 8 || abs(c.y - g_lastCur.y) > 8); g_lastCur = c;
      if (moved && split::Active() && c.x < W / 2 && g_owner == 1 && !g_drag && g_p1Swap == 0 && g_resumeStep == 0 && !g_resume &&
          now - g_p2ActAt > 2.f && IsTool(g_p1.mode)) P1Ready(now); }
    // player 2's select click: needs the game's selection mode (not player 1's bench tool)
    if (g_selPending > 0 && --g_selPending == 0) {
        split::SetP2Pick(true); PostMouse(WM_MOUSEMOVE, 0); PostMouse(WM_LBUTTONDOWN, MK_LBUTTON); PostMouse(WM_LBUTTONUP, 0);
        g_pickHold = 4;
    }
    // player 1's tool going back in
    if (g_p1Swap) {
        std::string m = IsTool(g_p1.mode) ? g_p1.mode : std::string("mode_selection");
        if (g_p1Swap == 1) { SendMode(m); g_p1Swap = 2; }
        else if (g_p1Swap == 2) { SendItem(m, g_p1.name); g_p1Swap = 3; }
        else if (++g_p1Swap >= 6 && (m == g_curMode || now - g_p1SwapAt > 0.6f)) { g_p1Swap = 0; g_owner = 0; g_aimHeld = false; split::SetP2Pick(false); }
    }
    // --- tool running ---
    if (g_tool) {
        if (g_startStep == 1) {   // the tool's object goes in one frame after the mode switch (engine rule)
            char code[300];
            const char* msg = g_kind == K_PLACE ? "ZT_SETPLACEMENTOBJECT" : g_kind == K_FENCE ? "ZT_SETPLACEMENTFENCE" : g_kind == K_PATH ? "ZT_SETPLACEMENTPATH" : nullptr;
            if (msg) {
                snprintf(code, sizeof code, "local m=queryObject('ZTMode') if m then m:sendMessage('%s','%s') %s end", msg, g_name.c_str(),
                         g_kind == K_FENCE ? "m:sendMessage('ZT_SETFENCEPLACEMENTMODE','rubber-band')" : "");
                Lua(code);
            }
            g_startStep = 2; g_owner = 1;
            if (g_resume) { g_resume = false; g_resumeStep = 4; }
        } else if (!strcmp(g_curMode, g_mode)) g_seenMode = true;
        // ZT15: player 1 took the game's tool -> player 2's tool waits; A puts it back in, then the press happens
        bool live = g_priv || (g_owner == 1 && g_p1Swap == 0 && g_startStep == 2);
        if (g_resumeStep > 0) { --g_resumeStep; live = false; }
        if (g_tool && live && !g_priv) { split::SetP2Pick(true); g_aimHeld = true; }
        else if (g_aimHeld && g_pickHold == 0) { split::SetP2Pick(false); g_aimHeld = false; }
        // ZT17f: player 2 got the shared fence tool because the game's own fence tool had not been seen yet - now it has
        // (player 2's pick just opened it): move player 2 onto their own private copy
        // ZT17f: player 2 holds the game's ONE tool (fence / path / terrain) - move them onto their own private copy of it
        // (JPOG method), so the game's tool goes back to player 1 and both can build at the same time
        if (g_tool && (g_kind == K_FENCE || g_kind == K_PATH || g_kind == K_PAINT) && !g_priv && !g_drag && g_owner == 1 &&
            g_startStep == 2 && now >= s_privTry && now - g_p2ActAt > 0.3f) {
            s_privTry = now + 2.f;
            if (privtool::StartCurrent(g_name.c_str())) {
                g_priv = true; g_privPost = true; g_drag = false; g_aArmed = false; g_seenMode = true; g_pendingA = false;
                if (IsTool(g_p1.mode)) { g_p1Swap = 1; g_p1SwapAt = now; } else { SendMode("mode_selection"); g_owner = 0; }
                Hint(now, g_kind == K_PAINT ? "Player 2: your own terrain tool - hold A and move to paint, B stops%s"
                                            : "Player 2: your own tool - hold A and move to draw, let go, tap A to build, B stops%s");
            }
        }
        if (g_tool && g_startStep == 2 && !live && !g_menu && !padBusy) {
            if (pad.Pressed(PAD_A) && !g_resume && g_resumeStep == 0 && g_p1Swap == 0) {
                used = true; g_pendingA = true; g_resume = true; g_p2ActAt = now;
                LOG("p2tools: player 2's tool (%s %s) goes back in", g_mode, g_name.c_str());
                SendMode(g_mode); g_startStep = 1; g_seenMode = false;
            }
            if (pad.Pressed(PAD_B)) { used = true; StopTool(now, "Player 2: tool stopped"); }
        }
        if (g_tool && live) {
            // A pressed while the tool was going back in counts as pressed now (fences / paths / terrain: only if still held)
            bool aP = pad.Pressed(PAD_A), aR = pad.Released(PAD_A);
            // ZT18b: fences / paths / terrain follow the HELD state of A (edges were lost: the first hold after picking gave
            // only 'up', a tap only 'down'). Down while not dragging = press, up while dragging = let go.
            if (g_kind == K_FENCE || g_kind == K_PATH || g_kind == K_PAINT) {   // g_aArmed: the A that picked the item must be let go first
                bool d = pad.Down(PAD_A); if (!d) g_aArmed = true;
                aP = g_aArmed && d && !g_drag; aR = !d && g_drag; }
            if (g_pendingA) { g_pendingA = false; bool fenceLike = g_kind == K_FENCE || g_kind == K_PATH || g_kind == K_PAINT;
                              if (!fenceLike || pad.Down(PAD_A)) aP = true; }
            if (aP || aR) g_p2ActAt = now;
            if (g_priv) { if (g_drag) PostMouse(WM_MOUSEMOVE, MK_LBUTTON); else privtool::Hover(); }   // private tool: aims for player 2
            else if (g_drag) { split::SetP2Pick(true); PostMouse(WM_MOUSEMOVE, MK_LBUTTON); }
            else if (g_pickHold > 0) { --g_pickHold; }
            else ToolMsg("mm:sendMessage('UI_MOUSEMOVE',{})");
            if (!g_menu && !padBusy) {
                if ((g_kind == K_FENCE || g_kind == K_PATH || g_kind == K_PAINT) && (aP || aR)) {
                    // like player 1 holding the mouse button: hold A = paint / drag, let go = finish
                    used = true;
                    { static int s_al = 0; if (g_priv && s_al < 24) { ++s_al; LOG("p2tools: player 2 A %s%s (drag %d)", aP ? "down" : "", aR ? "up" : "", (int)g_drag); } }
                    if (aP && !g_drag) { if (!g_priv) split::SetP2Pick(true); PostMouse(WM_MOUSEMOVE, 0); PostMouse(WM_LBUTTONDOWN, MK_LBUTTON); g_drag = true; }
                    // ZT18b: not 'else' - a tap (press + release in one poll) also lets go
                    if (aR && g_drag) { PostMouse(WM_MOUSEMOVE, MK_LBUTTON); PostMouse(WM_LBUTTONUP, 0); g_drag = false; g_pickHold = 4; }
                } else if (aP && g_kind == K_DELETE) {
                    used = true;   // the delete tool reads the real click, like paths
                    split::SetP2Pick(true); PostMouse(WM_MOUSEMOVE, 0); PostMouse(WM_LBUTTONDOWN, MK_LBUTTON); PostMouse(WM_LBUTTONUP, 0);
                    g_pickHold = 4;
                } else if (aP) {
                    used = true;
                    if (false) {
                    } else {   // a real click at player 2's pointer: world (placement) or a panel button under it
                        split::SetP2Pick(true); PostMouse(WM_MOUSEMOVE, 0); PostMouse(WM_LBUTTONDOWN, MK_LBUTTON); PostMouse(WM_LBUTTONUP, 0);
                        g_pickHold = 4;
                    }
                }
                bool rl = pad.Pressed(PAD_DLEFT) || pad.Pressed(PAD_LB), rr = pad.Pressed(PAD_DRIGHT) || pad.Pressed(PAD_RB);
                if (g_kind == K_PLACE && (rl || rr) && !pad.Down(PAD_DUP)) {
                    used = true;
                    g_rot += rr ? 45.f : -45.f; while (g_rot >= 360.f) g_rot -= 360.f; while (g_rot < 0.f) g_rot += 360.f;
                    char code[200]; snprintf(code, sizeof code, "local m=queryObject('ZTMode') if m then m:sendMessage('ZT_OBJECT_ROTATE','%d') end", (int)g_rot);
                    Lua(code);
                }
                if (pad.Pressed(PAD_B)) { used = true; StopTool(now, "Player 2: tool stopped"); }
            }
        }
    }
    // no tool, no menu: A = select what player 2 aims at (the game's own selection + info panel), real click
    if (!padBusy && !g_tool && !g_menu && pad.Pressed(PAD_A)) {
        used = true;
        if (IsTool(g_curMode) && g_p1Swap == 0) {   // player 1 is building: select in selection mode, their tool comes back on their next click
            SendMode("mode_selection"); g_owner = 1; g_selPending = 4;
        } else if (g_p1Swap == 0) {
            split::SetP2Pick(true); PostMouse(WM_MOUSEMOVE, 0); PostMouse(WM_LBUTTONDOWN, MK_LBUTTON); PostMouse(WM_LBUTTONUP, 0);
            g_pickHold = 4;
        }
        LOG("p2tools: player 2 selects at the aim"); g_lastSelectAt = now; g_p2ActAt = now;
    }
    if (!padBusy && pad.Pressed(PAD_BACK) && !g_menu) {
        used = true;
        if (g_tool && g_kind == K_DELETE) StopTool(now, "Player 2: Delete off");
        else { StopTool(now, nullptr); StartTool(now, K_DELETE, "Delete"); }
    }
    // --- hint line (player 2's half, bottom) ---
    if (now < g_hintUntil || (g_tool && g_startStep == 2)) {
        const int fpx = H / 40;
        char txt[200];
        if (now < g_hintUntil) snprintf(txt, sizeof txt, "%s", g_hint);
        else snprintf(txt, sizeof txt, "Player 2: %s%s", g_kind == K_DELETE ? "Delete" : g_name.c_str(), g_drag ? "  (building...)" : "");
        RECT b = { W / 2 + W / 40, H / 14, W - W / 40, H / 14 + fpx * 2 };   // top of player 2's half (the game's panels sit at the bottom)
        overlay::Line l = { txt, b, D3DCOLOR_ARGB(255, 255, 255, 255), D3DCOLOR_ARGB(150, 0, 0, 0) };
        overlay::Panel(dev, b, 0, &l, 1, fpx);
    }
    return used;
}
}

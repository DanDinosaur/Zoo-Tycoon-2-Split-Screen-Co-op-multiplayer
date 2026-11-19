#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include "padui.h"
#include "xinput_pad.h"
#include "overlay.h"
#include "luabridge.h"
#include "p2tools.h"
#include "log.h"
#include "split.h"
#include <algorithm>

namespace {
    struct Btn { float x, y, w, h; int list; std::string name; std::string type; };
    struct List { float x, y, w, h; };
    std::vector<Btn> g_btns;
    std::vector<Btn> g_allBtns;     // every visible button (also behind a dialog): the main panel buttons for X/LT/Y/RT
    bool g_modal = false;
    std::vector<List> g_lists;
    std::vector<List> g_solid;      // visible panel parts (backgrounds, texts): a click there is a UI click
    float g_lastScan = -10.f;
    bool g_dirty = true;

    struct P {
        bool active = false;
        std::string selName; float selX = -1, selY = -1;   // remembered selection (name + centre in UI space)
        int sel = -1;
        float repeatAt = 0.f; int lastDir = -1;
        std::string opened;            // main panel this player opened (B closes it again)
        float wantFirstAt = -1.f;      // after opening: jump to the first item when the panel has appeared
        bool toolNoList = false;
        float rsAt = 0.f; int rsDir = 0;   // ZT18d: right stick list scrolling (repeat)
        int pendWheel = 0;                  // ZT18d: player 1 scrolls with the REAL mouse wheel (the game moves the list's bar too)       // ZT18d: a button started a tool - if no item list shows up, back to the world
        float settleUntil = 0.f;       // after a click: rescan before moving
        std::string modeAtClick, clickName; float watchModeUntil = 0.f;
        bool itemClick = false; float clickAt = 0.f;   // last click was an item of an item list
        int wList = 0, wAmount = 0; float wAt = -1.f; std::string wSig;   // ZT18f: player 2's posted wheel - checked once it ran
        float winAt = -1.f, wx = 0, wy = 0, ww = 0, wh = 0;   // ZT19h: jump into a window (crate info) once it is scanned
        // ZT19l: the A-Z text list of the animal menu (View button): game type names + shown names, sorted by name
        bool tabFirst = false;   // ZT19m: the construction panel opens on its tabs, not inside the fence list
        bool alpha = false; int aList = 0, aSel = 0, aTop = 0, aDir = -1; float aRep = 0.f;
        std::vector<std::string> aType, aName;
    } g_p[2];
    // ZT18f: player 2's list scrolling = a mouse wheel message posted at player 2's highlighted icon (the game moves the
    // list's own scroll bar too, like player 1's real wheel). -1 not known yet, 1 the posted wheel scrolls, 0 it does not
    // (then the game's UI_SCROLL as before - list moves, bar stays).
    int g_p2Wheel = -1;
    bool ListSig(int list, std::string& out) {
        char code[600]; static char o[1024];
        snprintf(code, sizeof code, "local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if not l then return '' end local s='' "
            "local ch=l:UI_GET_CHILDREN() if type(ch)=='table' then for i,v in ipairs(ch) do local c=resolveTable(v.value) "
            "if c then local r=c:UI_GET_SCREEN_RECT() s=s..(c:UI_GET_NAME() or '?')..math.floor(r and r.x or 0)..','..math.floor(r and r.y or 0)..';' end end end "
            "return string.sub(s,1,900)", list);
        if (!luabridge::Run(code, o, sizeof o)) return false;
        out = o; return true;
    }
    // player 2: post the wheel (padinput does it with the returned r.wheel) and look later whether the list moved
    bool P2Wheel(P& p, int list, int dir, int amount) {
        if (g_p2Wheel == 0 || !list) return false;
        std::string sg; if (g_p2Wheel < 0 && (!ListSig(list, sg) || sg.empty())) return false;
        p.pendWheel = dir; p.wList = list; p.wAmount = amount; p.wSig = sg; p.wAt = g_p2Wheel < 0 ? -2.f : -1.f;   // -2: check after the post
        return true;
    }

    // The collector: every visible, enabled button of the game's UI. Modal windows (dialogs, options, zoo status...)
    // take the pad alone while they are open. Item lists clip their icons; hidden icons are skipped.
    const char* kScan =
        "local r=queryObject('UIRoot') if not r then return '' end "
        "local out={} local lists={} _ZT2MOD_LISTS=lists _ZT2MOD_V={} "
        "local skip={['Edge Scroller']=1,['Message bar layout']=1,['Cursor Money Container']=1,['Sound Tester Output layout']=1,['VINCE Layout']=1} "
        "local modal={['Confirm Dialog']=1,['Alert Dialog']=1,['In Game Options Shell']=1,['pause layout']=1,['profile_manager_dialog_shell']=1,"
        "['zoopedia layout']=1,['photoalbum']=1,['overview']=1,['zoo status']=1,['Map Selection Shell']=1,['Load Game Shell']=1,['Save Game Shell']=1} "
        "local isbtn={UIButton=1,UIToggleButton=1,UIHoverButton=1} "
        "local function walk(o,grp,clip,li,d) "
        "  if d>14 then return end "
        "  local ok,vis=pcall(function() return o:UI_GET_LINEAGE_VISIBLE() end) if not ok or not vis then return end "
        "  local nm=o:UI_GET_NAME() or '' "
        "  if skip[nm] then return end "
        "  local t=o._type local q=o:UI_GET_SCREEN_RECT() "
        "  if t=='ZTUITypeList' or t=='UIListBox' then table.insert(lists,o) li=table.getn(lists) clip={q.x,q.y,q.x+q.w,q.y+q.h} "
        "    table.insert(out,'L '..li..' '..q.x..' '..q.y..' '..q.w..' '..q.h) end "
        // ZT15: the terrain brushes (deep water, shallow water, ground, mix, ground cover) and land-shaping tools are toggle sets
        "  if t=='UIToggleSet' and (nm=='Biome' or nm=='Terrain Deformation') then table.insert(lists,o) li=table.getn(lists) clip=nil "
        "    table.insert(out,'L '..li..' '..q.x..' '..q.y..' '..q.w..' '..q.h) end "
        // ZT18d: the in-game load / save lists (load.xml SavedGamesList, save.xml SavedGamesList1): only the rows shown
        "  if t=='UIToggleSet' and (nm=='SavedGamesList' or nm=='SavedGamesList1') then table.insert(lists,o) li=table.getn(lists) _ZT2MOD_V[li]=1 clip={q.x,q.y,q.x+q.w,q.y+q.h} "
        "    table.insert(out,'L '..li..' '..q.x..' '..q.y..' '..q.w..' '..q.h) end "
        "  if not isbtn[t] and t~='UILayout' and t~='UIRoot' and q.w>0 and q.h>0 and q.w*q.h<300000 then table.insert(out,'S '..q.x..' '..q.y..' '..q.w..' '..q.h) end "
        "  if isbtn[t] and q.w>=12 and q.h>=12 and not o:UI_IS_DISABLED() then "
        "    local cx,cy=q.x+q.w/2,q.y+q.h/2 "
        "    if not clip or (q.x>=clip[1]-2 and q.x+q.w<=clip[3]+2 and cy>=clip[2] and cy<=clip[4]) then "
        "      table.insert(out,'B '..grp..' '..(li or 0)..' '..q.x..' '..q.y..' '..q.w..' '..q.h..' '..t..' '..nm) end "
        "  end "
        "  local ch=o:UI_GET_CHILDREN() if type(ch)=='table' then for i,v in ipairs(ch) do local c=resolveTable(v.value) if c then walk(c,grp,clip,li,d+1) end end end "
        "end "
        "local ch=r:UI_GET_CHILDREN() "
        "for i,v in ipairs(ch) do local c=resolveTable(v.value) if c then local nm=c:UI_GET_NAME() or '' "
        "  walk(c,(modal[nm] and 'M' or 'N'),nil,nil,0) end end "
        "return table.concat(out,'\\n')";

    void Scan(float now) {
        if (!luabridge::Ready() || !luabridge::Idle()) return;
        static char out[120000];
        if (!luabridge::Run(kScan, out, sizeof out)) { LOG("padui: scan error: %.200s", out); g_lastScan = now; return; }
        g_lastScan = now; g_dirty = false;
        std::vector<Btn> all, modal; g_lists.clear(); g_solid.clear();
        for (char* s = out; *s;) {
            char* e = strchr(s, '\n'); if (e) *e = 0;
            if (s[0] == 'L') { int id; List l; if (sscanf(s, "L %d %f %f %f %f", &id, &l.x, &l.y, &l.w, &l.h) == 5) {
                    if ((int)g_lists.size() < id) g_lists.resize(id); g_lists[id - 1] = l; } }
            else if (s[0] == 'S') { List l; if (sscanf(s, "S %f %f %f %f", &l.x, &l.y, &l.w, &l.h) == 4) g_solid.push_back(l); }
            else if (s[0] == 'B') {
                char grp; Btn b; char type[40]; int n = 0;
                if (sscanf(s, "B %c %d %f %f %f %f %39s %n", &grp, &b.list, &b.x, &b.y, &b.w, &b.h, type, &n) >= 7) {
                    b.type = type; b.name = n ? s + n : "";
                    (grp == 'M' ? modal : all).push_back(b);
                }
            }
            if (!e) break; s = e + 1;
        }
        g_btns = modal.empty() ? all : modal;
        g_modal = !modal.empty();
        g_allBtns = all; g_allBtns.insert(g_allBtns.end(), modal.begin(), modal.end());
    }
    void EnsureScan(float now) { if (g_dirty || now - g_lastScan > 0.35f) Scan(now); }

    float Cx(const Btn& b) { return b.x + b.w * 0.5f; }
    float Cy(const Btn& b) { return b.y + b.h * 0.5f; }

    // keep the selection on the same button after a rescan (name first, then the nearest centre)
    void Resolve(P& p) {
        p.sel = -1; if (g_btns.empty()) return;
        float best = 1e9f;
        for (size_t i = 0; i < g_btns.size(); ++i) {
            const Btn& b = g_btns[i];
            float d = fabsf(Cx(b) - p.selX) + fabsf(Cy(b) - p.selY);
            if (b.name == p.selName) d *= 0.1f;
            if (d < best) { best = d; p.sel = (int)i; }
        }
        if (p.sel >= 0) { p.selName = g_btns[p.sel].name; p.selX = Cx(g_btns[p.sel]); p.selY = Cy(g_btns[p.sel]); }
    }
    void Select(P& p, int i) { p.sel = i; p.selName = g_btns[i].name; p.selX = Cx(g_btns[i]); p.selY = Cy(g_btns[i]); }

    // first thing to look at after a panel opens: the top-left icon of an item list, else the button nearest the
    // panel's main button
    void SelectFirst(P& p) {
        int best = -1; float bs = 1e9f;
        for (size_t i = 0; i < g_btns.size(); ++i) {
            const Btn& b = g_btns[i]; if (!b.list) continue;
            float s = b.x * 2 + b.y; if (s < bs) { bs = s; best = (int)i; }
        }
        if (best >= 0) { Select(p, best); return; }
        Resolve(p);
    }

    // jump: direction 0 right, 1 left, 2 up, 3 down
    bool Move(P& p, int dir) {
        if (p.sel < 0 || p.sel >= (int)g_btns.size()) { Resolve(p); return p.sel >= 0; }
        const Btn& c = g_btns[p.sel];
        float cx = Cx(c), cy = Cy(c);
        float vx = dir == 0 ? 1.f : dir == 1 ? -1.f : 0.f, vy = dir == 3 ? 1.f : dir == 2 ? -1.f : 0.f;
        int best = -1; float bs = 1e9f;
        for (size_t i = 0; i < g_btns.size(); ++i) {
            if ((int)i == p.sel) continue;
            const Btn& b = g_btns[i];
            float dx = Cx(b) - cx, dy = Cy(b) - cy;
            float along = dx * vx + dy * vy, perp = fabsf(dx * vy - dy * vx);
            if (along < 4.f) continue;
            if (perp > along * 2.5f + 20.f) continue;     // roughly in that direction
            float s = along + perp * 2.2f;
            if (b.list && b.list == c.list) s *= 0.8f;     // stay inside the same item list
            if (s < bs) { bs = s; best = (int)i; }
        }
        // ZT18d: in a vertical list (the save / load rows) up/down at the visible edge scrolls the list one row (16 px)
        if (c.list && (dir == 2 || dir == 3) && c.list <= (int)g_lists.size() && !(best >= 0 && g_btns[best].list == c.list)) {
            if (&p == &g_p[0]) {   // ZT18d: player 1 - real wheel (the save list's own scroll bar follows)
                p.pendWheel = dir == 3 ? -1 : 1; p.selName.clear(); g_dirty = true;
                LOG("padui: player 1 scrolls the save list %s (mouse wheel)", dir == 3 ? "down" : "up");
                return true;
            }
            if (P2Wheel(p, c.list, dir == 3 ? -1 : 1, dir == 3 ? -16 : 16)) { p.selName.clear(); g_dirty = true; return true; }   // ZT18f
            static char o[64]; char probe[700];
            snprintf(probe, sizeof probe,
                "local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if not l or not (_ZT2MOD_V and _ZT2MOD_V[%d]) then return 0 end "
                "local function sig() local s='' local ch=l:UI_GET_CHILDREN() if type(ch)=='table' then for i,v in ipairs(ch) do "
                "local c=resolveTable(v.value) if c then local r=c:UI_GET_SCREEN_RECT() s=s..math.floor(r and r.y or 0)..';' end end end return s end "
                "local a=sig() l:UI_SCROLL(%d) local b=sig() return a~=b and 1 or 0", c.list, c.list, dir == 3 ? -16 : 16);
            if (luabridge::Run(probe, o, sizeof o) && atoi(o) > 0) {
                LOG("padui: player %d scrolls the save list %s", (int)(&p - g_p) + 1, dir == 3 ? "down" : "up");
                p.selName.clear(); g_dirty = true; return true;
            }
        }
        // inside an item list, left/right at the edge: scroll the list one column when more icons are hidden there
        if (c.list && (dir == 0 || dir == 1) && c.list <= (int)g_lists.size()) {
            bool nextInList = best >= 0 && g_btns[best].list == c.list;
            if (!nextInList) {
                char code[200];
                snprintf(code, sizeof code, "local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if l then l:UI_SCROLL(%d) end", c.list, dir == 0 ? -57 : 57);
                // ZT17: the type lists only keep the visible icons as children, so ask the list itself: scroll, then see
                // whether the first visible icon changed (at the end of the list nothing moves and the highlight stays)
                static char o[128];
                char probe[700];
                snprintf(probe, sizeof probe,
                    "local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if not l then return 0 end "
                    "local function sig() local s='' local ch=l:UI_GET_CHILDREN() if type(ch)=='table' then for i,v in ipairs(ch) do "
                    "local c=resolveTable(v.value) if c then local r=c:UI_GET_SCREEN_RECT() s=s..(c:UI_GET_NAME() or '?')..math.floor(r and r.x or 0)..';' end end end return s end "
                    "local a=sig() l:UI_SCROLL(%d) local b=sig() return a~=b and 1 or 0", c.list, dir == 0 ? -57 : 57);
                (void)code;
                if (&p == &g_p[0]) {   // ZT18d: player 1 - the real wheel on the list, so the game's scroll bar follows too
                    p.pendWheel = dir == 0 ? -1 : 1; p.selName.clear(); g_dirty = true;
                    LOG("padui: player 1 scrolls the list %s (mouse wheel)", dir == 0 ? "right" : "left");
                    return true;
                }
                if (P2Wheel(p, c.list, dir == 0 ? -1 : 1, dir == 0 ? -57 : 57)) { p.selName.clear(); g_dirty = true; return true; }   // ZT18f
                if (luabridge::Run(probe, o, sizeof o) && atoi(o) > 0) {
                    LOG("padui: player %d scrolls the list %s", (int)(&p - g_p) + 1, dir == 0 ? "right" : "left");
                    p.selName.clear();    // the same screen spot now shows the next column: keep the spot, not the name
                    g_dirty = true;
                    return true;
                }
            }
        }
        if (best < 0) return false;
        Select(p, best);
        return true;
    }

    int ToPxX(float x, int W) { return (int)(x * W / 1024.f); }
    int ToPxY(float y, int H) { return (int)(y * H / 768.f); }

    // ZT19l: shown name of an animal type: the game's own species name when it has one, else 'FlamingoGreater_Adult_F' ->
    // 'Flamingo Greater (female)'
    std::string NiceName(const std::string& type, const std::string& game) {
        std::string sp = type.substr(0, type.find('_')), out;
        if (!game.empty() && game != "nil" && game.find("ERROR") == std::string::npos) out = game;
        else for (size_t i = 0; i < sp.size(); ++i) {
            if (i && isupper((unsigned char)sp[i]) && !isupper((unsigned char)sp[i - 1])) out += ' ';
            out += sp[i]; }
        if (type.find("_Young") != std::string::npos) out += " (young)";
        if (type.size() > 2 && type.compare(type.size() - 2, 2, "_F") == 0) out += " (female)";
        else if (type.size() > 2 && type.compare(type.size() - 2, 2, "_M") == 0) out += " (male)";
        return out;
    }
    const char* kAlphaFns =
        "function zt2mod_lnames(l) local t={} local ch=l:UI_GET_CHILDREN() if type(ch)=='table' then for i,v in ipairs(ch) do "
        " local c=resolveTable(v.value) if c then local ok,n=pcall(function() return c:UI_GET_NAME() end) if ok and n then table.insert(t,{n=n,c=c}) end end end end return t end "
        "function zt2mod_lsig(l) local s='' for _,e in ipairs(zt2mod_lnames(l)) do s=s..e.n..';' end return s end "
        "function zt2mod_lstart(l) for i=1,150 do local a=zt2mod_lsig(l) l:UI_SCROLL(57) if zt2mod_lsig(l)==a then break end end end ";
    bool AlphaOpen(P& p, int player) {
        int list = 0;
        if (p.sel >= 0 && p.sel < (int)g_btns.size() && g_btns[p.sel].list) list = g_btns[p.sel].list;
        if (!list) for (const Btn& b : g_btns) if (b.list && (b.name.find("_Adult") != std::string::npos || b.name.find("_Young") != std::string::npos)) { list = b.list; break; }
        if (!list) { LOG("padui: player %d A-Z list - no animal list on screen", player + 1); return false; }
        static char code[2400]; static char out[24000];
        snprintf(code, sizeof code, "%s local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if not l then return '' end "
            "zt2mod_lstart(l) local seen={} local o={} "
            "for i=1,150 do for _,e in ipairs(zt2mod_lnames(l)) do if not seen[e.n] then seen[e.n]=1 "
            "   local sp=string.gsub(e.n,'_.*','') local g='' pcall(function() g=getSpeciesName(sp) end) if type(g)~='string' then g='' end "
            "   table.insert(o,e.n..'\\t'..g) end end "
            " local a=zt2mod_lsig(l) l:UI_SCROLL(-57) if zt2mod_lsig(l)==a then break end end "
            "zt2mod_lstart(l) return table.concat(o,'\\n')", kAlphaFns, list);
        if (!luabridge::Run(code, out, sizeof out)) { LOG("padui: A-Z list error: %.150s", out); return false; }
        p.aType.clear(); p.aName.clear();
        std::vector<std::pair<std::string, std::string>> v;
        for (char* s2 = out; *s2;) { char* e = strchr(s2, '\n'); if (e) *e = 0;
            char* t = strchr(s2, '\t'); std::string ty = t ? std::string(s2, t - s2) : std::string(s2), gn = t ? std::string(t + 1) : "";
            if (ty.find('_') != std::string::npos && ty.find(' ') == std::string::npos) v.push_back({ NiceName(ty, gn), ty });
            if (!e) break; s2 = e + 1; }
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
            return _stricmp(a.first.c_str(), b.first.c_str()) < 0; });
        for (auto& x : v) { p.aName.push_back(x.first); p.aType.push_back(x.second); }
        if (p.aType.empty()) { LOG("padui: player %d A-Z list - the list has no animals", player + 1); return false; }
        p.alpha = true; p.aList = list; p.aSel = 0; p.aTop = 0; p.aDir = -1;
        LOG("padui: player %d A-Z list opened (%u animals)", player + 1, (unsigned)p.aType.size());
        return true;
    }
    // scroll the game's list until the picked animal's icon is on screen; its rect (UI space) -> the click
    bool AlphaFind(P& p, float& x, float& y, float& w, float& h) {
        static char code[2400]; char out[160];
        snprintf(code, sizeof code, "%s local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if not l then return 'none' end zt2mod_lstart(l) "
            "for i=1,150 do for _,e in ipairs(zt2mod_lnames(l)) do if e.n=='%s' then local q=e.c:UI_GET_SCREEN_RECT() "
            "  if q then return string.format('%%.1f %%.1f %%.1f %%.1f',q.x,q.y,q.w,q.h) end end end "
            " local a=zt2mod_lsig(l) l:UI_SCROLL(-57) if zt2mod_lsig(l)==a then break end end return 'none'",
            kAlphaFns, p.aList, p.aType[p.aSel].c_str());
        return luabridge::Run(code, out, sizeof out) && sscanf(out, "%f %f %f %f", &x, &y, &w, &h) == 4;
    }
}

namespace padui {
bool SelPx(int pl, int W, int H, int& x, int& y) {
    const P& p = g_p[pl]; if (!p.active || p.sel < 0 || p.sel >= (int)g_btns.size()) return false;
    x = ToPxX(Cx(g_btns[p.sel]), W); y = ToPxY(Cy(g_btns[p.sel]), H); return true;
}
bool Active(int player) { return g_p[player].active; }
void EnterWindow(int player, float now, float x, float y, float w, float h) {   // ZT19h
    P& p = g_p[player];
    p.active = true; p.opened.clear(); p.wantFirstAt = -1.f; p.winAt = now + 0.35f; p.wx = x; p.wy = y; p.ww = w; p.wh = h;
    p.sel = -1; p.selName.clear(); g_dirty = true;
    LOG("padui: player %d - the pad goes into the window at %.0f,%.0f %.0fx%.0f", player + 1, x, y, w, h);
}
bool InfoX(float now, float rx, float ry, float rw, float rh, float& ux, float& uy) {   // ZT19w
    EnsureScan(now);
    const Btn* best = nullptr;
    for (const Btn& b : g_allBtns) {
        float cx = Cx(b), cy = Cy(b);
        if (cx < rx || cx > rx + rw || cy < ry || cy > ry + rh * 0.2f) continue;   // the window's top strip (tabs + X)
        if (!best || cx > Cx(*best)) best = &b;
    }
    if (best && Cx(*best) > rx + rw * 0.8f) { ux = Cx(*best); uy = Cy(*best); LOG("padui: info window X = '%s'", best->name.c_str()); return true; }
    ux = rx + rw - 26.f; uy = ry + 22.f; LOG("padui: info window X not found among the buttons - its corner"); return true;
}
void Leave(int player) { g_p[player].active = false; g_p[player].wantFirstAt = -1.f; g_p[player].alpha = false; }
void Reset() { for (auto& p : g_p) p = P(); g_btns.clear(); g_dirty = true; }
bool TakeClick(int player, const char* name, float now) {
    P& p = g_p[player];
    if (p.clickAt != now || p.clickName != name) return false;
    p.watchModeUntil = 0.f; p.itemClick = false; p.clickName.clear(); p.toolNoList = false;
    Leave(player);
    LOG("padui: player %d's '%s' kept from the game", player + 1, name);
    return true;
}

Result Open(int player, const char* mainButton, float now, int W, int H) {
    Result r; r.used = true;
    P& p = g_p[player];
    EnsureScan(now);
    for (const Btn& b : g_allBtns) if (b.name == mainButton) {
        // already open (its toggle is on): don't press it again (that would close it), just jump in
        char code[200], o[64];
        snprintf(code, sizeof code, "local r=queryObject('UIRoot') local b=r and r:UI_GET_CHILD('%s') return b and b:UI_IS_ACTIVATED()", mainButton);
        bool already = luabridge::Run(code, o, sizeof o) && !strncmp(o, "true", 4);
        p.active = true; p.opened = mainButton; p.wantFirstAt = now + 0.35f; g_dirty = true; p.tabFirst = !strcmp(mainButton, "construction");
        p.selName = b.name; p.selX = Cx(b); p.selY = Cy(b);
        if (!already) { r.act = CLICK; r.x = ToPxX(Cx(b), W); r.y = ToPxY(Cy(b), H); }
        if (player == 1) p2tools::NoteP2Click(now); else p2tools::NoteP1Click(now);
        LOG("padui: player %d opens '%s'", player + 1, mainButton);
        return r;
    }
    LOG("padui: player %d: no '%s' button on screen", player + 1, mainButton);
    return r;
}

bool WatchTick(int player, float now) {
    P& p = g_p[player];
    if (p.watchModeUntil <= now) return false;
    const char* m = p2tools::CurMode();
    bool tool = m[0] && strcmp(m, "mode_selection");
    // ZT14: only an ITEM (an animal icon, a terrain brush, an object) hands the pad back to the world. A tab / category
    // button that starts a tool (Construction -> fences, Terraform -> mode_biome) keeps the frame in the menu and moves it
    // to the first item of the list that just opened (owner: the stick must reach deep water / grass / the animals).
    if (tool && p.itemClick && now > p.clickAt + 0.25f) {
        LOG("padui: player %d picked '%s' (%s) - back to the world", player + 1, p.clickName.c_str(), m);
        p.watchModeUntil = 0.f; Leave(player);
        if (player == 1) p2tools::Adopt(m, p.clickName.c_str(), now);
        return true;
    }
    if (tool && !p.itemClick && p.modeAtClick != m) {
        p.modeAtClick = m; g_dirty = true;
        if (player == 1) p2tools::Adopt(m, p.clickName.c_str(), now);
        // ZT18d (owner: demolish must go straight to the aim circle, "plug and play for all menu options"): tools without
        // an item list (demolish = mode_deletion, move, ...) hand the pad back to the world at once; tools with a list
        // get the frame on their first item, and if no list appears after all, the pad goes back to the world too.
        if (!strcmp(m, "mode_deletion")) {
            LOG("padui: player %d picked '%s' (%s) - back to the world", player + 1, p.clickName.c_str(), m);
            p.watchModeUntil = 0.f; Leave(player); return true;
        }
        LOG("padui: player %d opened %s - frame goes to its list", player + 1, m);
        p.wantFirstAt = now + 0.3f; p.toolNoList = true;
        return false;
    }
    return false;
}
bool OverUI(int px, int py, int W, int H, float now, char* name, int nameSize, bool* item) {
    EnsureScan(now);
    float x = px * 1024.f / W, y = py * 768.f / H;
    for (const Btn& b : g_btns) if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
        if (name) snprintf(name, nameSize, "%s", b.name.c_str()); if (item) *item = b.list != 0; return true; }
    if (name && nameSize) name[0] = 0; if (item) *item = false;
    for (const List& l : g_solid) if (x >= l.x && x < l.x + l.w && y >= l.y && y < l.y + l.h) return true;
    return false;
}
void NoteClick(int player, const char* name, bool item, float now) {
    P& p = g_p[player];
    p.modeAtClick = p2tools::CurMode(); p.watchModeUntil = now + 1.0f;
    p.itemClick = item; p.clickAt = now; p.clickName = name ? name : "";
    g_dirty = true;
    if (player == 1) p2tools::NoteP2Click(now); else if (item) p2tools::NoteP1Pick(name, now); else p2tools::NoteP1Click(now);
}
Result Frame(int player, const XPad& pad, float now, int W, int H) {
    Result r;
    P& p = g_p[player];
    if (!p.active) return r;
    r.used = true;
    if (p.pendWheel) { r.wheel = p.pendWheel; p.pendWheel = 0; if (player == 1 && p.wAt == -2.f) p.wAt = now + 0.2f; }
    if (player == 1 && p.wAt > 0.f && now >= p.wAt) {   // ZT18f: did player 2's posted wheel move the list?
        p.wAt = -1.f; std::string sg;
        if (ListSig(p.wList, sg)) {
            if (sg != p.wSig) { g_p2Wheel = 1; LOG("padui: player 2's posted mouse wheel scrolls lists (scroll bar follows)"); }
            else {
                g_p2Wheel = 0;
                char code[200], o[16]; snprintf(code, sizeof code, "local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if l then l:UI_SCROLL(%d) end return 1", p.wList, p.wAmount);
                luabridge::Run(code, o, sizeof o);
                LOG("padui: player 2's posted mouse wheel did not scroll (list %d) - UI_SCROLL from now on", p.wList);
            }
            g_dirty = true;
        }
    }
    EnsureScan(now);
    if (p.alpha) {   // ZT19l: the A-Z list owns the pad - stick / D-pad up-down, LB / RB a page, A places, B / View back
        int dir = (pad.Down(PAD_DUP) || pad.LY() > 0.6f) ? 2 : (pad.Down(PAD_DDOWN) || pad.LY() < -0.6f) ? 3 : -1;
        if (dir < 0) p.aDir = -1;
        else if (dir != p.aDir || now >= p.aRep) { p.aRep = now + (dir != p.aDir ? 0.35f : 0.12f); p.aDir = dir; p.aSel += dir == 3 ? 1 : -1; }
        if (pad.Pressed(PAD_RB)) p.aSel += 10; if (pad.Pressed(PAD_LB)) p.aSel -= 10;
        int n = (int)p.aType.size(); if (p.aSel < 0) p.aSel = 0; if (p.aSel >= n) p.aSel = n - 1;
        if (pad.Pressed(PAD_B) || pad.Pressed(PAD_BACK)) { p.alpha = false; g_dirty = true; LOG("padui: player %d A-Z list closed", player + 1); return r; }
        if (pad.Pressed(PAD_A) && n > 0) {
            float x, y, w, h;
            if (AlphaFind(p, x, y, w, h)) {
                r.act = CLICK; r.x = ToPxX(x + w / 2, W); r.y = ToPxY(y + h / 2, H);
                p.modeAtClick = p2tools::CurMode(); p.watchModeUntil = now + 1.0f; p.itemClick = true; p.clickAt = now; p.clickName = p.aType[p.aSel];
                if (player == 1) p2tools::NoteP2Click(now); else p2tools::NoteP1Pick(p.aType[p.aSel].c_str(), now);
                LOG("padui: player %d A-Z list picks '%s' (%s)", player + 1, p.aName[p.aSel].c_str(), p.aType[p.aSel].c_str());
                p.alpha = false; g_dirty = true;
            } else LOG("padui: player %d A-Z list - '%s' not found in the game's list", player + 1, p.aType[p.aSel].c_str());
        }
        return r;
    }
    if (pad.Pressed(PAD_BACK) && p.sel >= 0 && !g_modal) { if (AlphaOpen(p, player)) return r; }
    if (p.winAt > 0 && now >= p.winAt) {   // ZT19h: first button inside the window (top-left first)
        p.winAt = -1.f; Scan(now);
        int best = -1; float bs = 1e9f;
        for (size_t i = 0; i < g_btns.size(); ++i) { const Btn& b = g_btns[i];
            float cx = Cx(b), cy = Cy(b);
            if (cx < p.wx || cx > p.wx + p.ww || cy < p.wy || cy > p.wy + p.wh) continue;
            if (b.name.find("lose") != std::string::npos || b.name.find("Shadow") != std::string::npos) continue;   // ZT19r: not the window's X
            float sc = cy * 2 + cx; if (sc < bs) { bs = sc; best = (int)i; } }
        if (best >= 0) { Select(p, best); LOG("padui: player %d in the window - highlight '%s'", player + 1, g_btns[best].name.c_str()); }
        else { LOG("padui: player %d - no button in the window", player + 1); Leave(player); return r; }
    }
    else if (p.wantFirstAt > 0 && now >= p.wantFirstAt) {
        p.wantFirstAt = -1.f; Scan(now);
        if (p.tabFirst) {   // ZT19m (owner: X should show the construction panel, not jump into the fences first)
            p.tabFirst = false; int best = -1; float bs = 1e9f;
            for (size_t i = 0; i < g_btns.size(); ++i) { const Btn& b = g_btns[i];
                if (b.list || b.name.find("Tab") == std::string::npos) continue;
                float sc = Cy(b) * 2 + Cx(b); if (sc < bs) { bs = sc; best = (int)i; } }
            if (best >= 0) { Select(p, best); LOG("padui: player %d - construction tabs, highlight '%s'", player + 1, g_btns[best].name.c_str()); return r; }
        }
        bool hasList = false; for (const Btn& b : g_btns) if (b.list) { hasList = true; break; }
        const char* m = p2tools::CurMode();
        if (p.toolNoList && !hasList && !g_modal && m[0] && strcmp(m, "mode_selection")) {   // ZT18d: a tool with nothing to pick
            p.toolNoList = false;
            LOG("padui: player %d - %s has no item list - back to the world", player + 1, m);
            Leave(player); r.act = NONE; return r;
        }
        p.toolNoList = false;
        SelectFirst(p);
    }
    else Resolve(p);
    if (p.sel < 0) { if (pad.Pressed(PAD_B)) Leave(player); return r; }

    // a click that started one of the game's tools hands the pad back to the world (A places, B stops)
    if (WatchTick(player, now)) return r;

    // ZT18d (owner): the right stick scrolls the list the highlight is in, like the mouse wheel - animal / object icon
    // lists sideways (stick right or down = further along), the save lists up / down. The game's own UI_SCROLL.
    {   float rx = pad.RX(), ry = pad.RY(); int d = 0;
        if (fabsf(ry) >= fabsf(rx)) d = ry < -0.45f ? 1 : ry > 0.45f ? -1 : 0; else d = rx > 0.45f ? 1 : rx < -0.45f ? -1 : 0;
        const Btn* c = (p.sel >= 0 && p.sel < (int)g_btns.size()) ? &g_btns[p.sel] : nullptr;
        if (!d) p.rsDir = 0;
        else if (c && c->list && c->list <= (int)g_lists.size()) {
            r.scrolled = true;
            if (d != p.rsDir || now >= p.rsAt) {
                float m = fabsf(fabsf(ry) >= fabsf(rx) ? ry : rx);
                p.rsAt = now + (d != p.rsDir ? 0.35f : 0.28f - 0.18f * (m - 0.45f) / 0.55f); p.rsDir = d;
                if (player == 0) { r.wheel = d > 0 ? -1 : 1; p.selName.clear(); g_dirty = true; return r; }   // real wheel
                {   // ZT18f: player 2 - a posted wheel at the highlighted icon (scroll bar follows), UI_SCROLL if that does nothing
                    bool vert = false; { char vc[120], vo[16]; snprintf(vc, sizeof vc, "return (_ZT2MOD_V and _ZT2MOD_V[%d]) and 1 or 0", c->list);
                        vert = luabridge::Run(vc, vo, sizeof vo) && atoi(vo) > 0; }
                    if (P2Wheel(p, c->list, d > 0 ? -1 : 1, vert ? (d > 0 ? -16 : 16) : (d > 0 ? -57 : 57))) {
                        r.wheel = p.pendWheel; p.pendWheel = 0; if (p.wAt == -2.f) p.wAt = now + 0.2f; p.selName.clear(); g_dirty = true; return r; }
                }
                char code[300], o[32];
                snprintf(code, sizeof code, "local l=_ZT2MOD_LISTS and _ZT2MOD_LISTS[%d] if not l then return 0 end "
                         "local v=_ZT2MOD_V and _ZT2MOD_V[%d] l:UI_SCROLL(v and %d or %d) return 1", c->list, c->list, d > 0 ? -16 : 16, d > 0 ? -57 : 57);
                if (luabridge::Run(code, o, sizeof o)) { LOG("padui: player %d scrolls the list with the right stick (%s)", player + 1, d > 0 ? "on" : "back");
                    p.selName.clear(); g_dirty = true; }
            }
            return r;
        }
    }

    // D-pad or left stick: jump (repeat while held)
    int dir = -1;
    if (pad.Down(PAD_DRIGHT) || pad.LX() > 0.6f) dir = 0;
    else if (pad.Down(PAD_DLEFT) || pad.LX() < -0.6f) dir = 1;
    else if (pad.Down(PAD_DUP) || pad.LY() > 0.6f) dir = 2;
    else if (pad.Down(PAD_DDOWN) || pad.LY() < -0.6f) dir = 3;
    if (dir < 0) p.lastDir = -1;
    else if (dir != p.lastDir || now >= p.repeatAt) {
        p.repeatAt = now + (dir != p.lastDir ? 0.40f : 0.18f);
        p.lastDir = dir;
        if (Move(p, dir)) { if (g_dirty) { Scan(now); Resolve(p); } }
    }
    if (pad.Pressed(PAD_A) && p.sel >= 0) {
        const Btn& b = g_btns[p.sel];
        r.act = CLICK; r.x = ToPxX(Cx(b), W); r.y = ToPxY(Cy(b), H);
        p.modeAtClick = p2tools::CurMode(); p.watchModeUntil = now + 1.0f;
        // ZT18b: the land-shaping buttons (cliff, valley, smooth ...) are items too - the scan does not find their toggle set
        static bool s_deformTab = false;   // the Terrain Deformation tab is the one showing
        if (b.name.find("Tab") != std::string::npos) s_deformTab = b.name == "Terrain Deformation Tab";
        p.itemClick = (b.list != 0 && !g_modal) || (s_deformTab && b.type == "UIToggleButton" &&
                                      b.name.find("Tab") == std::string::npos && b.name.find(' ') == std::string::npos);
        p.clickAt = now; p.clickName = b.name;
        if (player == 1) p2tools::NoteP2Click(now); else if (p.itemClick) p2tools::NoteP1Pick(b.name.c_str(), now); else p2tools::NoteP1Click(now);
        if (!p.itemClick) p.wantFirstAt = now + 0.35f;   // a tab: jump into the list it shows
        g_dirty = true;
        LOG("padui: player %d presses '%s' (%s)", player + 1, b.name.c_str(), b.type.c_str());
    }
    if (pad.Pressed(PAD_B)) {
        // B: close the panel this player opened (press its main button again), back to the world
        // a window (zoo finances, dialogs): press its own close / OK button
        const Btn* close = nullptr;
        if (g_modal) for (const Btn& b : g_btns) {
            std::string n = b.name; for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
            // exact names only ("close zoo toggle button" closes the ZOO, not the window - seen in the ZT10 test)
            if (n == "close" || n == "close button" || n == "ok" || n == "ok button" || n == "done" || n == "cancel" || n == "exit" || n == "x") { close = &b; break; }
        }
        // ZT19 (owner): B in an item list (animals, food, toys...) first goes up to the menu's own options (its tabs, else
        // the other buttons), so another part of the menu can be picked; B again (on a tab / button) = back to the world
        int up = -1;
        if (!close && p.sel >= 0 && p.sel < (int)g_btns.size() && g_btns[p.sel].list) {
            const Btn& c = g_btns[p.sel]; float bd = 1e9f, bd2 = 1e9f; int up2 = -1;
            for (size_t i = 0; i < g_btns.size(); ++i) { const Btn& b = g_btns[i]; if (b.list) continue;
                float dx = Cx(b) - Cx(c), dy = Cy(b) - Cy(c), d = dx * dx + dy * dy;
                if (b.name.find("Tab") != std::string::npos) { if (d < bd) { bd = d; up = (int)i; } }
                else if (d < bd2) { bd2 = d; up2 = (int)i; } }
            if (up < 0) up = up2;
        }
        if (up >= 0) {
            const Btn& b = g_btns[up];
            p.sel = up; p.selName = b.name; p.selX = Cx(b); p.selY = Cy(b); p.lastDir = -1;
            LOG("padui: player %d B -> up to the menu ('%s'); B again = back to the world", player + 1, b.name.c_str());
            return r;
        }
        if (close) { r.act = CLICK; r.x = ToPxX(Cx(*close), W); r.y = ToPxY(Cy(*close), H); LOG("padui: player %d closes the window ('%s')", player + 1, close->name.c_str()); }
        else if (!p.opened.empty()) {
            // ZT14: a tool picked from this panel is running (brush, object): keep it and the panel, just go to the world
            const char* m = p2tools::CurMode();
            bool tool = m[0] && strcmp(m, "mode_selection") && strncmp(m, "mode_fence", 10);
            if (!tool) for (const Btn& b : g_allBtns) if (b.name == p.opened) { r.act = CLICK; r.x = ToPxX(Cx(b), W); r.y = ToPxY(Cy(b), H); break; }
        }
        p.opened.clear();
        Leave(player);
        LOG("padui: player %d back to the world", player + 1);
    }
    return r;
}

void Draw(IDirect3DDevice9* dev, float now, int W, int H) {
    for (int pl = 0; pl < 2; ++pl) {   // ZT19l: the A-Z list, in the game's own menu colours (light panel, green text)
        P& p = g_p[pl];
        if (!p.active || !p.alpha || p.aType.empty()) continue;
        bool half = split::Active();
        int x0 = half ? (pl ? W / 2 : 0) : W / 4, x1 = half ? (pl ? W : W / 2) : W * 3 / 4;
        const int rows = 14, fpx = H / 34, rh = fpx + fpx / 2;
        int n = (int)p.aType.size();
        if (p.aSel < p.aTop) p.aTop = p.aSel; if (p.aSel >= p.aTop + rows) p.aTop = p.aSel - rows + 1;
        RECT box{ x0 + W / 60, H / 8, x1 - W / 60, H / 8 + rh * (rows + 2) + fpx };
        overlay::Panel(dev, box, D3DCOLOR_ARGB(235, 214, 218, 230), nullptr, 0, 0);
        overlay::Line ln[rows + 2]; int k = 0; static char title[96], rowtxt[rows][96];
        snprintf(title, sizeof title, "Adopt Animals  A-Z   (%d / %d)", p.aSel + 1, n);
        ln[k++] = { title, RECT{ box.left + fpx / 2, box.top + fpx / 3, box.right - fpx / 2, box.top + fpx / 3 + rh }, D3DCOLOR_ARGB(255, 30, 95, 45), 0 };
        for (int i = 0; i < rows && p.aTop + i < n; ++i) {
            int idx = p.aTop + i; snprintf(rowtxt[i], sizeof rowtxt[i], "%s", p.aName[idx].c_str());
            RECT rr{ box.left + fpx / 2, box.top + rh * (i + 1) + fpx / 2, box.right - fpx / 2, box.top + rh * (i + 2) + fpx / 2 };
            bool sel = idx == p.aSel;
            ln[k++] = { rowtxt[i], rr, sel ? D3DCOLOR_ARGB(255, 20, 20, 20) : D3DCOLOR_ARGB(255, 30, 70, 40),
                        sel ? (pl ? D3DCOLOR_ARGB(255, 60, 220, 255) : D3DCOLOR_ARGB(255, 255, 220, 40)) : 0 };
        }
        static char help[] = "A = place   B = back   LB / RB = page";
        ln[k++] = { help, RECT{ box.left + fpx / 2, box.bottom - rh, box.right - fpx / 2, box.bottom - fpx / 4 }, D3DCOLOR_ARGB(255, 60, 60, 60), 0 };
        overlay::Panel(dev, box, 0, ln, k, fpx);
    }
    for (int pl = 0; pl < 2; ++pl) {
        P& p = g_p[pl];
        if (p.alpha) continue;
        if (!p.active || p.sel < 0 || p.sel >= (int)g_btns.size()) continue;
        const Btn& b = g_btns[p.sel];
        float pulse = 0.5f + 0.5f * sinf(now * 6.f);
        int grow = (int)(H * 0.006f + pulse * H * 0.004f);
        RECT o = { ToPxX(b.x, W) - grow, ToPxY(b.y, H) - grow, ToPxX(b.x + b.w, W) + grow, ToPxY(b.y + b.h, H) + grow };
        int t = H / 160 + 2;   // frame thickness
        DWORD col = pl == 0 ? D3DCOLOR_ARGB(255, 255, 220, 40) : D3DCOLOR_ARGB(255, 60, 220, 255);
        DWORD fill = pl == 0 ? D3DCOLOR_ARGB((int)(50 + 40 * pulse), 255, 220, 40) : D3DCOLOR_ARGB((int)(50 + 40 * pulse), 60, 220, 255);
        overlay::Panel(dev, o, fill, nullptr, 0, 0);
        RECT e[4] = { { o.left, o.top, o.right, o.top + t }, { o.left, o.bottom - t, o.right, o.bottom },
                      { o.left, o.top, o.left + t, o.bottom }, { o.right - t, o.top, o.right, o.bottom } };
        for (auto& r : e) overlay::Panel(dev, r, col, nullptr, 0, 0);
        // little player tag
        const int fpx = H / 45;
        RECT tag = { o.left, o.top - fpx - 4, o.left + fpx * 2, o.top - 2 };
        overlay::Line l = { pl == 0 ? "P1" : "P2", tag, D3DCOLOR_ARGB(255, 20, 20, 20), col };
        overlay::Panel(dev, tag, 0, &l, 1, fpx);
    }
}
}

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "privtool.h"
#include "hook.h"
#include "guard.h"
#include "split.h"
#include "luabridge.h"
#include "log.h"

namespace {
    constexpr bool kEnabled = true;   // ZT17e: with the game's own enter / exit calls (SetActiveChild 0x441c23)
    constexpr DWORD kRelease = 0x409292;
    typedef int (__fastcall* Upd_t)(void* self, void* edx, void* ret, int arg);
    typedef void (__fastcall* Rel_t)(void* self, void* edx);
    typedef void (__fastcall* Vf0_t)(void* self, void* edx);
    typedef DWORD (__cdecl* Create_t)();
    // ZT17f: the game's own create functions (push size; call alloc 0x5c7095; push eax; call ctor; ret) per tool vtable
    struct Kind { DWORD vt, size, create; const char* name; DWORD game; };
    Kind g_kinds[] = {
        { 0x8c1f48, 0x300, 0x426f0f, "fence", 0 },      // ZTFenceMode
        { 0x8c55b0, 0x244, 0x449da6, "path", 0 },       // ZTPathMode
        { 0x8bf7e0, 0x320, 0x406643, "terrain paint", 0 },   // ZT18b: ctor 0x401211 writes vt 0x8bf7e0 = mode_biome (log)
        { 0x8c96c8, 0x1f4, 0x4713f0, "land shaping", 0 },   // ZT18b: mode_deformation (log), create 0x4713f0 -> ctor 0x46f4f6
        { 0x8c04b0, 0x3bc, 0x40e204, "tool 8c04b0", 0 },
        { 0x8c19a0, 0x248, 0x41d960, "tool 8c19a0", 0 },
    };
    Kind* KindOf(DWORD obj) { for (auto& k : g_kinds) if (*(DWORD*)obj == k.vt) return &k; return nullptr; }
    Upd_t oOvUpd = nullptr;
    DWORD g_ov = 0;          // mode_overhead
    DWORD g_priv = 0;        // player 2's copy (of g_privKind)
    Kind* g_privKind = nullptr;
    bool g_on = false, g_entered = false;
    int g_errs = 0;
    DWORD g_lastAct = 0;
    bool g_p2Stroke = false; // ZT18b: player 2 holds A on their own tool (event 258 .. 25c)

    // The game's SetActiveChild (0x441c23): old->vfn 0x22c (leave), [0x935224] = old, parent+0x12c = new, new->vfn 0x228 (enter).
    bool CallEnterExit(DWORD ov, DWORD tool, DWORD slotOff) {
        DWORD slot = ov + 0x12c, keep = *(DWORD*)slot, prev = *(DWORD*)0x935224;
        *(DWORD*)slot = tool; split::SetP2Pick(true);
        bool ok = GUARDED("privtool:enterexit", { DWORD vt = *(DWORD*)tool; ((Vf0_t)*(DWORD*)(vt + slotOff))((void*)tool, nullptr); });
        split::SetP2Pick(false); *(DWORD*)slot = keep; *(DWORD*)0x935224 = prev;
        return ok;
    }
    // ZT16 research: the overhead's input forwarders (slot 0x258..0x268 -> active child's same slot), arg = event
    typedef int (__fastcall* Ev_t)(void* self, void* edx, void* ret, DWORD arg);
    Ev_t oEv[5] = {};
    const DWORD kEvAddr[5] = { 0x443ea1, 0x443f18, 0x443ff7, 0x443f7b, 0x4441b0 };
    const DWORD kEvSlot[5] = { 0x258, 0x25c, 0x260, 0x264, 0x268 };
    // player 2's posted mouse messages: the event the overhead forwards carries the message's lParam (event word 4,
    // verified: 0x028a0190 = x 400 y 650). Events whose lParam matches a recent player-2 post go to player 2's copy.
    struct Post { LPARAM l; DWORD t; } g_posts[16]; int g_postN = 0;
    bool IsP2(DWORD arg) {
        // event object = *arg, its data = [ev+16]: data+8 = x, data+12 = y (verified for button down / up)
        DWORD lp = 0; bool ok = GUARDED("privtool:ev", { DWORD ev = *(DWORD*)arg; DWORD data = *(DWORD*)(ev + 16);
                                                        lp = MAKELPARAM(*(DWORD*)(data + 8), *(DWORD*)(data + 12)); });
        if (!ok) return false;
        DWORD now = GetTickCount();
        for (auto& p : g_posts) if (p.t && now - p.t < 700 && (DWORD)p.l == lp) return true;
        return false;
    }
    // ZT18b: the fence tool's own create-preview (0x422266: eax = tool, push 1, ret 4). It builds the preview entity from the
    // item name at +0x24c when the +0x22c handle (a weak handle: [h] = object) is null or its object is gone, assigns it
    // (0x5aed75) and adds it to the world (+0x1c4 = 1). Same call the game's update makes at 0x420ebb.
    void EnsurePreview(Kind* k) {
        if (!g_priv || !k || k->vt != 0x8c1f48) return;
        DWORD h = *(DWORD*)(g_priv + 0x22c);
        if (h && *(DWORD*)h) return;
        DWORD fn = 0x422266, t = g_priv;
        bool ok = GUARDED("privtool:preview", {
            __asm__ __volatile__("pushl $1\n\tcall *%1" : "+a"(t) : "r"(fn) : "ecx", "edx", "memory", "cc");
        });
        static int s_n = 0;
        if (s_n < 10) { ++s_n; DWORD h2 = *(DWORD*)(g_priv + 0x22c); char nm[48] = "?";
            GUARDED("privtool:nm", { DWORD so = *(DWORD*)(g_priv + 0x24c); const char* c = so ? *(const char**)(so + 8) : nullptr;
                                     snprintf(nm, sizeof nm, "%s", c ? c : "(null)"); });
            BYTE* b = (BYTE*)g_priv;
            LOG("privtool: player 2's preview rebuilt by the game's 0x422266 -> %s (+0x22c %08lx -> %08lx) item '%s' 1a8=%d 1c4=%d 251=%d 290=%lu",
                ok ? "ok" : "FAULT", h2, h2 ? *(DWORD*)h2 : 0, nm, b[0x1a8], b[0x1c4], b[0x251], *(DWORD*)(g_priv + 0x290)); }
    }
    template <int K> int __fastcall hEv(void* self, void* edx, void* ret, DWORD arg) {
        static int s_log = 0;
        if (g_on && g_priv && s_log < 30 && K != 4 && IsP2(arg)) { ++s_log; LOG("privtool: player 2's event %03lx goes to their own tool", kEvSlot[K]); }
        { static int s_nlog = 0; if (g_on && g_priv && K < 2 && s_nlog < 20 && !IsP2(arg)) { ++s_nlog;
            DWORD lp = 0; GUARDED("privtool:ev2", { DWORD ev = *(DWORD*)arg; DWORD data = *(DWORD*)(ev + 16);
                                                   lp = MAKELPARAM(*(DWORD*)(data + 8), *(DWORD*)(data + 12)); });
            LOG("privtool: event %03lx at %d,%d is NOT player 2's -> game tool", kEvSlot[K], (short)LOWORD(lp), (short)HIWORD(lp)); } }
        if (g_on && g_priv && IsP2(arg)) {
            DWORD slot = (DWORD)self + 0x12c, keep = *(DWORD*)slot;
            *(DWORD*)slot = g_priv; split::SetP2Pick(true);
            EnsurePreview(g_privKind);
            int r = 0;
            GUARDED("privtool:evcall", { r = oEv[K](self, edx, ret, arg); });
            if (K == 0) g_p2Stroke = true; else if (K == 1) g_p2Stroke = false;
            // ZT18d: path diagnostics for the owner's test (paths built nothing in the auto test): state +0x1a4, path pieces
            // +0x1a8.. (0x448a42 reads [+0x1ac]), cursor valid +0x1c8 (0x447215 / 0x4473e0 need it)
            if (g_privKind == &g_kinds[1] && K < 4) { static int s_pl = 0; if (s_pl < 40) { ++s_pl; BYTE* b = (BYTE*)g_priv;
                LOG("privtool: path event %03lx -> state %lu pieces %08lx %08lx %08lx cursor %d at %.1f %.1f %.1f", kEvSlot[K],
                    *(DWORD*)(b + 0x1a4), *(DWORD*)(b + 0x1a8), *(DWORD*)(b + 0x1ac), *(DWORD*)(b + 0x1b0), b[0x1c8],
                    *(float*)(b + 0x194), *(float*)(b + 0x198), *(float*)(b + 0x19c)); } }
            split::SetP2Pick(false); *(DWORD*)slot = keep;
            return r;
        }
        return oEv[K](self, edx, ret, arg);
    }
// ZT18c: a fence tool that is no longer the active one can keep its preview (the leave 0x41f6af skips the game's own
// clear-preview 0x42238a when 0x440ea9 says no) -> a "($50)" post stayed at player 1's mouse. Clear it the game's way
// (0x42238a, edi = tool, plain ret).
void ClearPreview(DWORD tool) {
    if (!tool) return;
    bool live = false;
    GUARDED("privtool:pv?", { DWORD h = *(DWORD*)(tool + 0x22c); live = *(DWORD*)tool == 0x8c1f48 && h && *(DWORD*)h; });
    if (!live) return;
    DWORD fn = 0x42238a, t = tool;
    bool ok = GUARDED("privtool:clearpv", {
        __asm__ __volatile__("pushl %%edi\n\tmovl %%edx, %%edi\n\tcall *%%ecx\n\tpopl %%edi" : "+c"(fn), "+d"(t) : : "eax", "memory", "cc");
    });
    LOG("privtool: left-over fence preview of %08lx cleared by the game's 0x42238a -> %s", tool, ok ? "ok" : "FAULT");
}
// ZT18d: same for land shaping (vt 0x8c96c8): its leave 0x46f774 removes the brush highlight +0x1b0 from the terrain
// (0x730444, stdcall(terrain, obj), ret 8) and deletes it (vfn 0, push 1) - but only when 0x440ea9 says the tool was
// entered. A highlight still there after the tool stopped being active = left over -> the same three steps.
void ClearShapeHighlight(DWORD tool) {
    if (!tool) return;
    DWORD obj = 0, terr = 0;
    GUARDED("privtool:hl?", { if (*(DWORD*)tool == 0x8c96c8) obj = *(DWORD*)(tool + 0x1b0);
                              terr = *(DWORD*)(*(DWORD*)0x9352d0 + 0x210); });
    if (!obj) return;
    typedef void (__stdcall* Rem_t)(DWORD, DWORD);
    typedef void (__fastcall* Del_t)(DWORD self, DWORD edx, int flags);
    bool ok = GUARDED("privtool:clearhl", {
        if (terr) ((Rem_t)0x730444)(terr, obj);
        ((Del_t)*(DWORD*)*(DWORD*)obj)(obj, 0, 1);
        *(DWORD*)(tool + 0x1b0) = 0;
    });
    LOG("privtool: left-over land-shaping highlight of %08lx removed the game's way (0x730444 + delete) -> %s", tool, ok ? "ok" : "FAULT");
}
    int __fastcall hOvUpd(void* self, void* edx, void* ret, int arg) {
        // ZT18b: two terrain-paint tools share the terrain's stroke state (terrain +0xf0): while player 2 paints and the
        // game's active tool is the same kind, player 2's tool IS the overhead's active one for the frame (player 1's
        // brush pauses during player 2's stroke instead of swallowing it).
        if (g_on && g_priv && g_p2Stroke && g_privKind && (g_privKind->vt == 0x8bf7e0 || g_privKind->vt == 0x8c96c8)) {
            DWORD act = *(DWORD*)((DWORD)self + 0x12c);
            if (act && act != g_priv && *(DWORD*)act == g_privKind->vt) {
                DWORD slot = (DWORD)self + 0x12c; *(DWORD*)slot = g_priv; split::SetP2Pick(true);
                int r = 0; bool ok = GUARDED("privtool:shared", { r = oOvUpd(self, edx, ret, arg); });
                split::SetP2Pick(false); *(DWORD*)slot = act;
                if (ok) return r;
            }
        }
        int r = oOvUpd(self, edx, ret, arg);
        g_ov = (DWORD)self;
        DWORD act = *(DWORD*)(g_ov + 0x12c);
        if (act && act != g_priv) {
            Kind* k = nullptr; GUARDED("privtool:kind", { k = KindOf(act); });
            if (k && k->game != act) { k->game = act; LOG("privtool: the game's %s tool = %08lx", k->name, act); }
            if (act != g_lastAct && g_lastAct && g_lastAct != g_priv) { ClearPreview(g_lastAct); ClearShapeHighlight(g_lastAct); }   // ZT18c/d
            if (act != g_lastAct) { DWORD vt = 0; GUARDED("privtool:vt", { vt = *(DWORD*)act; }); LOG("privtool: game tool now %08lx (vtable %08lx%s)", act, vt, k ? "" : " - not a known tool"); g_lastAct = act; }
        }
        if (g_on && g_priv && g_errs < 5) {
            bool ok = true;
            DWORD slot = g_ov + 0x12c, keep = *(DWORD*)slot;
            *(DWORD*)slot = g_priv;   // the tool only works while it is the overhead's active one
            split::SetP2Pick(true);
            ok = GUARDED("privtool:update", {
                DWORD tmp = 0; DWORD vt = *(DWORD*)g_priv;
                ((Upd_t)*(DWORD*)(vt + 0x38))((void*)g_priv, nullptr, &tmp, arg);
                if (tmp) ((Rel_t)kRelease)(&tmp, nullptr);
            });
            split::SetP2Pick(false);
            *(DWORD*)slot = keep;
            if (!ok && ++g_errs >= 5) { g_on = false; LOG("privtool: player 2's tool faulted 5 times - off"); }
        }
        return r;
    }
}

namespace privtool {
void Install() {
    if (!kEnabled) return;
    static const unsigned char pro[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c };
    hook::Install(0x444342, pro, 6, (void*)hOvUpd, (void**)&oOvUpd, "OverheadUpdate(privtool)");
    void* fn[5] = { (void*)hEv<0>, (void*)hEv<1>, (void*)hEv<2>, (void*)hEv<3>, (void*)hEv<4> };
    for (int i = 0; i < 5; ++i) {
        const unsigned char* p = (const unsigned char*)kEvAddr[i];
        unsigned char pro5[5]; memcpy(pro5, p, 5);
        if (pro5[0] != 0x55 || pro5[1] != 0x8b || pro5[2] != 0xec) continue;
        char nm[40]; snprintf(nm, sizeof nm, "OverheadEvent%03lx(privtool)", kEvSlot[i]);
        hook::Install(kEvAddr[i], pro5, 5, fn[i], (void**)&oEv[i], nm);
    }
}
bool Active() { return g_on && g_priv && g_ov; }
bool NeedsSource() { return false; }   // ZT18: a fresh instance needs no game tool to copy from
// ZT18: the game's own message that sets what the tool builds, sent while the tool is the overhead's active one
// (ZTMode routes item messages to it), with player 2's pick swapped in. Logs how many dwords of the tool changed.
// ZT18: the game sets a tool up through the component system (XML load vfn 0x50, parent link +0x0c, shared links
// +0x20c/+0x210/+0x214); a bare create gives none of that (createComponent from Lua neither). Copy those from the game's
// own instance of the same kind: plain values (floats / ints / flags) and the four links - never a field that points into
// memory (the game's heap containers are freed by its leave; ours stay the constructor's own).
static bool Readable(DWORD v) {
    if (v < 0x10000 || v >= 0x80000000 || (v & 3)) return false;
    MEMORY_BASIC_INFORMATION mb; if (!VirtualQuery((void*)v, &mb, sizeof mb)) return false;
    return mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
static void CopyConfig(Kind* k, DWORD own) {
    if (!k->game || k->game == own) { LOG("privtool: no game %s tool to take the set-up from", k->name); return; }
    int n = 0, links = 0;
    GUARDED("privtool:cfg", {
        DWORD* a = (DWORD*)own; DWORD* b = (DWORD*)k->game;
        for (DWORD i = 2; i < k->size / 4; ++i) {
            DWORD off = i * 4;
            if (off == 0x218 || off == 0x22c || a[i] == b[i]) continue;          // owned: built by enter / the item
            bool link = off == 0x0c || off == 0x20c || off == 0x210 || off == 0x214;
            bool selfB = b[i] >= k->game && b[i] < k->game + k->size;
            if (link) { a[i] = b[i]; ++links; continue; }
            if (selfB || Readable(a[i]) || Readable(b[i])) continue;           // pointers stay our own
            a[i] = b[i]; ++n;
        }
    });
    // ZT18b: the fence tool's item strings from its XML (+0x288 / +0x28c = the items the update switches to at 0x421a1c /
    // 0x421a24, +0x2f8 / +0x2fc = read at 0x42085d) are refcounted string objects -> shared with the game's own string
    // assign 0x409194 (thiscall: ecx = slot, arg = string, +0x10 refcount). Missing +0x28c made the first press set an
    // empty item -> no preview -> fault at 0x410231.
    int strs = 0;
    if (k->vt == 0x8c1f48) {
        typedef void (__fastcall* Asg_t)(void* slot, void* edx, DWORD str);
        const DWORD offs[] = { 0x288, 0x28c, 0x2f8, 0x2fc };
        for (DWORD off : offs) GUARDED("privtool:str", {
            DWORD src = *(DWORD*)(k->game + off);
            if (src && *(DWORD*)(own + off) != src) { ((Asg_t)0x409194)((void*)(own + off), nullptr, src); ++strs; } });
    }
    LOG("privtool: player 2's %s tool took the game's set-up: %d values, %d links, %d item strings", k->name, n, links, strs);
}
static bool Configure(Kind* k, const char* item) {
    // ZT18b: terrain paint = the biome tool's own ZT_SET_BIOME handler (0x4035cf: looks the brush up by name and puts it in
    // THIS tool's +0x2a0 handle) - without it player 2 painted the default first biome.
    const char* msg = k == &g_kinds[0] ? "ZT_SETPLACEMENTFENCE" : k == &g_kinds[1] ? "ZT_SETPLACEMENTPATH" : k->vt == 0x8bf7e0 ? "ZT_SET_BIOME" : nullptr;
    // land shaping: NO message - its ZT_SETTERRAINMODE handler (0x471d0c -> vfn 0x2c0) took the tab's name and the game
    // closed without a fault (13:55 test); its mode is a plain value that CopyConfig takes from the game's tool instead.
    if (item && (strstr(item, "Tab") || strchr(item, ' '))) msg = nullptr;   // a tab, not an item
    if (!msg || !item || !*item || !strcmp(item, "fence") || !strcmp(item, "fences") || !strcmp(item, "paths")) {
        LOG("privtool: player 2's %s tool keeps the game's default item ('%s')", k->name, item ? item : ""); return false; }
    static DWORD before[0x1000 / 4];
    GUARDED("privtool:snap", { memcpy(before, (void*)g_priv, k->size); });
    char code[300], out[200];
    snprintf(code, sizeof code, "local m=queryObject('ZTMode') if m then m:sendMessage('%s','%s') %s end return 'ok'", msg, item,
             k == &g_kinds[0] ? "m:sendMessage('ZT_SETFENCEPLACEMENTMODE','rubber-band')" : "");
    DWORD slot = g_ov + 0x12c, keep = *(DWORD*)slot, prev = *(DWORD*)0x935224;
    *(DWORD*)slot = g_priv; split::SetP2Pick(true);
    bool ok = luabridge::Run(code, out, sizeof out);
    split::SetP2Pick(false); *(DWORD*)slot = keep; *(DWORD*)0x935224 = prev;
    if (char* nl = strchr(out, '\n')) *nl = 0;
    int changed = 0;
    GUARDED("privtool:diff", { for (DWORD i = 0; i < k->size / 4; ++i) if (before[i] != ((DWORD*)g_priv)[i]) ++changed; });
    LOG("privtool: player 2's %s tool set to '%s' (%s %s) - %d dwords changed", k->name, item, ok ? "ok" : "lua error", out, changed);
    return ok;
}
// ZT18c: player 2's private fence / path tool follows player 2's aim the way the shared tool did: the game's own
// UI_MOUSEMOVE message to ZTMainMode (forwarded to the active child) with player 2's tool as the active one and the pick
// swapped. Without it the preview stayed where the item message put it (player 1's mouse) and a tap started the fence there.
void Hover() {
    if (!g_on || !g_priv || !g_ov || !g_privKind || (g_privKind != &g_kinds[0] && g_privKind != &g_kinds[1])) return;
    char out[64];
    DWORD slot = g_ov + 0x12c, keep = *(DWORD*)slot, prev = *(DWORD*)0x935224;
    *(DWORD*)slot = g_priv; split::SetP2Pick(true);
    luabridge::Run("local mm=queryObject('ZTMainMode') if mm then mm:sendMessage('UI_MOUSEMOVE',{}) end return 'ok'", out, sizeof out);
    split::SetP2Pick(false); *(DWORD*)slot = keep; *(DWORD*)0x935224 = prev;
}
// ZT18: player 2 gets a FRESH instance of the game's tool class (the game's own create function), entered the game's
// way (vfn 0x228 with overhead+0x12c swapped) and configured with the game's own item message. ZT17f byte-copied the
// game's instance instead: the copy shared heap members (tool+0x218, a std::map) with the game's tool, the game's tool
// left when player 1 got mode_selection back and freed them -> faults at 0x71462B (map lookup) / 0x41F716 (+0x218 dtor).
static bool StartKind(Kind* k, const char* item) {
    if (!kEnabled || !k) return false;
    if (!g_ov) { LOG("privtool: no overhead seen yet"); return false; }
    if (g_priv && g_privKind != k) { Stop(); g_priv = 0; }   // the old kind's instance is left (not freed: the game owns the allocator)
    if (!g_priv) {
        DWORD f = 0;
        if (!GUARDED("privtool:create", { f = ((Create_t)k->create)(); }) || !f || *(DWORD*)f != k->vt) {
            LOG("privtool: the game's create function for %s gave %08lx", k->name, f); return false; }
        g_priv = f; g_privKind = k; g_errs = 0; g_entered = false;
        LOG("privtool: player 2's %s tool = new instance %08lx (the game's create %08lx)", k->name, f, k->create);
        CopyConfig(k, f);
    } else if (!g_entered) CopyConfig(k, g_priv);   // ZT18b: picked again -> take the game's tool's new set-up (land shaping mode)
    if (!g_entered) { bool ok = CallEnterExit(g_ov, g_priv, 0x228); g_entered = ok; LOG("privtool: player 2's tool enter -> %s", ok ? "ok" : "FAULT");
                      if (!ok) { g_priv = 0; return false; } }   // a faulted object is never used again
    bool needsItem = k == &g_kinds[0] || k == &g_kinds[1];
    bool configured = Configure(k, item);
    if (needsItem && !configured) { Stop(); LOG("privtool: no %s item for player 2's own tool - shared tool", k->name); return false; }
    if (configured) {
        // ZT18b: NO leave + enter here any more. The item message (0x421a40 path) already built the preview +0x22c; leave
        // (0x41f6af -> 0x42238a) deleted it again (the entity dtor nulls the handle's object, the slot stays) -> the first press
        // faulted at 0x410231. Missing preview now = the game's own create-preview 0x422266 (EnsurePreview).
        bool okE = true;
        EnsurePreview(k);
        DWORD h = 0; if (k == &g_kinds[0]) GUARDED("privtool:h", { h = *(DWORD*)(g_priv + 0x22c); });
        LOG("privtool: player 2's tool set up with the item (+0x22c %08lx -> %08lx)", h, h ? *(DWORD*)h : 0);
        // ZT18 diagnostics: dword differences against the game's own configured tool of this kind (still active now)
        if (k == &g_kinds[0] && k->game && k->game != g_priv) GUARDED("privtool:cmp", {   // ZT18c: fence only (+0x22c is a fence handle)
            char line[1100]; int n = 0, cnt = 0; DWORD* a = (DWORD*)g_priv; DWORD* b = (DWORD*)k->game;
            for (DWORD i = 1; i < k->size / 4 && n < 1000; ++i) {
                if (a[i] == b[i]) continue;
                bool pa = a[i] >= g_priv && a[i] < g_priv + k->size, pb = b[i] >= k->game && b[i] < k->game + k->size;
                if (pa && pb && a[i] - g_priv == b[i] - k->game) continue;   // self pointers
                ++cnt; n += snprintf(line + n, sizeof line - n, " %03lx:%08lx/%08lx", i * 4, a[i], b[i]);
            }
            LOG("privtool: own vs game %s tool, %d dwords differ (own/game):%s", k->name, cnt, line);
            DWORD ha = a[0x22c / 4], hb = b[0x22c / 4];
            LOG("privtool: +0x22c handle own %08lx -> %08lx, game %08lx -> %08lx", ha, ha ? *(DWORD*)ha : 0, hb, hb ? *(DWORD*)hb : 0);
        });
        if (!okE) { g_priv = 0; return false; }
    }
    g_errs = 0; g_on = true;
    return true;
}
bool Start(const char* type) {
    bool ok = StartKind(&g_kinds[0], type);
    if (ok) LOG("privtool: player 2 draws '%s' fences with their own tool", type ? type : "");
    return ok;
}
bool StartCurrent(const char* item) {
    // ZT17f: player 2's pick just switched the game's tool (terrain / path / fence): copy THAT one for player 2
    if (!g_ov) return false;
    DWORD act = *(DWORD*)(g_ov + 0x12c); Kind* k = nullptr;
    if (!act || act == g_priv || !GUARDED("privtool:cur", { k = KindOf(act); }) || !k) { LOG("privtool: game tool %08lx has no private copy", act); return false; }
    k->game = act;
    bool ok = StartKind(k, item);
    if (ok) LOG("privtool: player 2 uses their own %s tool", k->name);
    return ok;
}
void Stop() {
    if (g_on) LOG("privtool: player 2's %s tool off", g_privKind ? g_privKind->name : "own");
    if (g_entered && g_ov && g_priv) { bool ok = CallEnterExit(g_ov, g_priv, 0x22c); LOG("privtool: player 2's tool leave -> %s", ok ? "ok" : "FAULT"); if (!ok) g_priv = 0; }
    if (g_priv) { ClearPreview(g_priv); ClearShapeHighlight(g_priv); }   // ZT18d: no-ops after a full leave
    g_entered = false; g_on = false; g_p2Stroke = false;
}
void NotePost(LPARAM l) { g_posts[g_postN++ & 15] = { l, GetTickCount() }; }
LRESULT Dispatch(WNDPROC orig, HWND h, UINT msg, WPARAM w, LPARAM l) {
    DWORD slot = g_ov + 0x12c, keep = *(DWORD*)slot;
    *(DWORD*)slot = g_priv; split::SetP2Pick(true);
    LRESULT r = 0;
    GUARDED("privtool:msg", { r = CallWindowProcW(orig, h, msg, w, l); });
    split::SetP2Pick(false); *(DWORD*)slot = keep;
    return r;
}
}

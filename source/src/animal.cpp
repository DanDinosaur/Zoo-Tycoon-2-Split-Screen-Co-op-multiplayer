#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "animal.h"
#include "xinput_pad.h"
#include "luabridge.h"
#include "split.h"
#include "p2tools.h"
#include "guard.h"
#include "log.h"
#include "config.h"
#include "overlay.h"
#include "sndwatch.h"
#include "callsnd.h"
#include "padui.h"

extern "C" DWORD zt_GenericCall(DWORD fn, DWORD eaxv, DWORD ecxv, const DWORD* args, int n, float* st0);

namespace {
    constexpr DWORD kGetPosPtr = 0x6c3135;   // eax = entity -> float* position
    constexpr DWORD kLocComp   = 0x6c5156;   // eax = entity -> location component (cached at entity+0x68)
    constexpr DWORD kNodeUpd   = 0x770254;   // (node) rebuild the node transform

    // animation candidates, first one the species has wins (ANIM_REQUEST answers SUCCESS / FAILURE)
    // ZT14: swimmers (manatee, sharks, dolphins...) have no Stand/Walk anims: the water ones follow as fallbacks
    const char* kIdle[]   = { "Stand_Idle", "TreadWater_Idle", "TreadWaterSub_Idle", "SwimSub_Idle", "Swim_Idle", "OnLand_Idle", nullptr };
    const char* kWalk[]   = { "Walk_Ahead", "StrutWalk_Ahead", "SlenderWalk_Ahead", "Swim_Ahead", "SwimSub_Ahead", "SwimGlide_Ahead", "SwimSubGlide_Ahead", nullptr };
    const char* kRun[]    = { "Run_Ahead", "Gallop_Ahead", "Trot_Ahead", "Walk_Ahead", "SwimSub_Ahead", "Swim_Ahead", nullptr };
    // in water (the animal floats above the ground under it): swim first
    const char* kSwimIdle[] = { "TreadWater_Idle", "TreadWaterSub_Idle", "SwimSub_Idle", "Swim_Idle", "Stand_Idle", nullptr };
    const char* kSwimWalk[] = { "Swim_Ahead", "SwimSub_Ahead", "SwimGlide_Ahead", "SwimSubGlide_Ahead", "Walk_Ahead", nullptr };
    const char* kAttack[] = { "Stand_BiteMed", "Stand_BitMed", "Stand_SwipeMed", "Stand_HitMed", "Stand_SwipeLeft",
                              "Stand_SwipeRight", "Stand_Kick", "Stand_Charge", "Stand_Headbutt", "Stand_Pounce", nullptr };
    const char* kSpecial[]= { "Stand_Pounce", "Stand_PlayPounce", "Stand_RearUp", "Stand_Rear", "Stand_ShakeMane",
                              "Stand_ScuffGround", "Stand_Stretch", "Stand_Display", nullptr };
    const char* kCall[]   = { "Stand_Roar", "Stand_Chuff", "Stand_Snarl", "Stand_Call", "Stand_Bellow", "Stand_Trumpet",
                              "Stand_Growl", "Stand_Vocalize", "Stand_Bark", "Stand_Howl", "Stand_Sniff", nullptr };
    const char* kCall2[]  = { "Stand_PlayRoar", "Stand_FriendlyPuff", "Stand_Snarl", "Stand_Yawn", "Stand_Listen", nullptr };
    const char* kEat[]    = { "Eat_ChewGulp", "Eat_DrinkA", "Eat_Graze", "Stand_Eat", "Stand_Graze", "Stand_LickObject",
                              "Stand_Sniff", nullptr };
    const char* kSniff[]  = { "Stand_Sniff", "Stand_Listen", "Stand_Yawn", "Stand_Idle", nullptr };
    const char* kLie[]    = { "Stand_2Lie", "Lie_Idle", nullptr };
    const char* kSleep[]  = { "Sleep_Idle", "SleepPaws_Idle", "SleepStand_Idle", "Lie_Yawn", "Lie_Idle", nullptr };   // vanilla Sleep sets
    const char* kDrink[]  = { "Eat_DrinkA", "Eat_Drink", "EatTrough_Drink", "Stand_Drink", "Eat_Idle", nullptr };      // vanilla DrinkWaterTerrain
    const char* kFeed[]   = { "Eat_ChewGulp", "Eat_BiteA", "Eat_Chew", "Eat_Graze", "Stand_Eat", "Stand_Graze", "Eat_Idle", nullptr };
    const char* kStand[]  = { "Lie_2Stand", "Stand_Idle", nullptr };

    #include "animaldata.inc"
    // species from the entity type name ("Lion_Adult_F", "TriceratopsJPOG_Young_M"): longest table name that is a prefix
    // followed by '_' or the end
    const SpeciesFight* FindSpecies(const char* type) {
        const SpeciesFight* best = nullptr; size_t bl = 0;
        for (const SpeciesFight& f : kSpecies) {
            size_t l = strlen(f.sp);
            if (l > bl && !strncmp(type, f.sp, l) && (type[l] == '_' || type[l] == 0)) { best = &f; bl = l; }
        }
        return best;
    }
    const char* kNoFight[] = { "Stand_Snort", "Stand_AlarmCall", "Stand_Alarm", "Stand_ThreatNoise", "Stand_Sniff", "Stand_Listen", nullptr };
    enum Loco { L_NONE, L_IDLE, L_WALK, L_RUN };
    enum Pose { P_STAND, P_LIE, P_SLEEP };

    struct Rider {
        bool on = false;
        DWORD ent = 0;
        float hx = 0, hy = 1;            // heading (ground unit vector)
        float camYaw = 0, camPitch = 0.42f, camDist = 16.f;
        float x = 0, y = 0, z = 0;
        bool run = false;
        bool strafing = false;
        Loco loco = L_NONE; float locoAt = -10.f;
        float busyUntil = 0.f;           // one-shot action playing (no locomotion)
        Pose pose = P_STAND;
        float yHeldAt = -1.f; bool yLong = false;
        float faceAt = -10.f; float lastHx = 0, lastHy = 0;
        float checkAt = 0.f;
        bool takeLatch = false;
        bool rotOK = false; float rot0[9] = {}, a0 = 0.f;   // ZT17: turn the model by writing the scene node's rotation
        const char* sndAnim = nullptr; float sndAt = 0.f; long snd3d = 0;   // ZT17: call / bite waiting for the game's sound
        const SpeciesFight* sf = nullptr; char type[64] = {}; int atkN = 0; float blockLogAt = -10.f, sleepTickAt = 0.f, poseAnimAt = 0.f;
        bool inWater = false, aquatic = false; float waterTop = 0.f, depth = 0.f; bool landOK = true; bool rbLatch = false; bool diveHinted = false; float waterLvl = -9999.f; float camZ = -9999.f; bool noDive = false; float floorZ = -9999.f; float sinkLogAt = -10.f; float* nodePos = nullptr;
    } g_r[2];

    float Clamp(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
    bool WaterHere(const Rider& r, float x, float y, float g) {   // ZT19e
        // ZT19f: WaterZ does not work on every zoo (log 14:34: 'water grid ... no water test') - ZT19e then said 'dry'
        // everywhere and LT / A no longer dived. Only a positive water reading is used; otherwise the height test decides
        if (!r.landOK) return true;
        float w; if (split::WaterZ(x, y, w)) return w - g > 0.6f;
        if (r.waterLvl > -9000.f) return r.z < r.waterLvl + 1.5f;   // ZT19g: on a rock / raised ground above the water = land
        return true;
    }

    bool Readable(DWORD a, DWORD n) {
        MEMORY_BASIC_INFORMATION mi{};
        if (a < 0x10000 || !VirtualQuery((void*)a, &mi, sizeof mi)) return false;
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !mi.Protect) return false;
        return a + n <= (DWORD)mi.BaseAddress + mi.RegionSize;
    }
    DWORD Rd(DWORD a) { return Readable(a, 4) ? *(DWORD*)a : 0; }

    float* PosPtr(DWORD ent) {
        DWORD r = 0;
        if (!GUARDED("animal:GetPos", { r = zt_GenericCall(kGetPosPtr, ent, 0, nullptr, 0, nullptr); })) return nullptr;
        return Readable(r, 12) ? (float*)r : nullptr;
    }
    DWORD Node(DWORD ent) {
        DWORD comp = Rd(ent + 0x68);
        if (!comp) GUARDED("animal:LocComp", { comp = zt_GenericCall(kLocComp, ent, 0, nullptr, 0, nullptr); });
        DWORD node = comp ? Rd(comp + 0xc) : 0;
        if (!(node && Readable(Rd(node), 4))) return 0;
        // ZT20c (owner: the ridden T-Rex and Sarcosuchus models pop around while walking, Deinosuchus is fine; log: 'position
        // is not inside node' for exactly those two): for some models comp+0xc is not the node that holds the position, so the
        // transform we rebuilt was the wrong one and the model only followed when the engine synced it. The node that holds
        // the position sits 0x84 before it (as in every animal that works); it is used when its own position call (vfn +0x124)
        // gives that same pointer back.
        if (float* pp = PosPtr(ent)) {
            DWORD off = (DWORD)pp - node;
            if (!(off > 0 && off < 0x200)) {
                static DWORD s_ent[4], s_res[4]; static int s_n = 0;   // per entity: found node, or 1 = none
                for (int i = 0; i < 4; ++i) if (s_ent[i] == ent) return (s_res[i] > 1 && Readable(Rd(s_res[i]), 4)) ? s_res[i] : node;
                DWORD cand = (DWORD)pp - 0x84, vt = Rd(cand), fn = vt ? Rd(vt + 0x124) : 0, back = 0;
                if (fn && Readable(fn, 4)) GUARDED("animal:node2", { back = zt_GenericCall(fn, 0, cand, nullptr, 0, nullptr); });
                bool ok = back == (DWORD)pp;
                int k = s_n++ & 3; s_ent[k] = ent; s_res[k] = ok ? cand : 1;
                if (ok) LOG("animal: entity %08lx - the node holding its position is %08lx (not %08lx) - using it", ent, cand, node);
                else LOG("animal: entity %08lx - no node found around its position (%08lx back %08lx)", ent, cand, back);
                if (ok) return cand;
            }
        }
        return node;
    }

    bool Lua(const char* code, char* out = nullptr, int n = 0) {
        static char scratch[512];
        if (!out) { out = scratch; n = sizeof scratch; }
        if (!luabridge::Ready() || !luabridge::Idle()) return false;
        return luabridge::Run(code, out, n);
    }
    // play the first animation of the list the species has; returns the chosen name (or null)
    const char* Anim(int p, const char* const* list, float playTime = 0.f) {
        char code[1400]; int k = snprintf(code, sizeof code,
            "local e=zt2mod_ride and zt2mod_ride[%d] if not e then return '' end "
            "for _,n in ipairs({", p);
        for (int i = 0; list[i] && k < (int)sizeof code - 200; ++i) k += snprintf(code + k, sizeof code - k, "'%s',", list[i]);
        snprintf(code + k, sizeof code - k,
            "}) do local ok,r=pcall(function() return e:sendMessage('ANIM_REQUEST',{seqToken=n,scale=1,time=%.1f}) end) "
            "if ok and r=='ANIM_REQUEST_SUCCESS' then return n end end return ''", playTime);
        static char out[128];
        if (!Lua(code, out, sizeof out)) return nullptr;
        char* nl = strchr(out, '\n'); if (nl) *nl = 0;
        return out[0] ? out : nullptr;
    }
    void Face(int p, float hx, float hy) {
        char code[256];
        // ZT17d: the game's handler (0x6c6864) takes a POINT to face (angle = atan2(point - entity position)), or a number
        // angle - not a direction. Passing the direction made the animal face the map origin. Give it a point ahead of it.
        const Rider& r = g_r[p];
        snprintf(code, sizeof code, "local e=zt2mod_ride and zt2mod_ride[%d] if e then e:BFG_SET_ENTITY_FACING_DIRECTION({x=%.3f,y=%.3f,z=%.3f}) end",
                 p, r.x + hx * 10.f, r.y + hy * 10.f, r.z);
        Lua(code);
    }

    void DoRelease(int p, const char* why) {
        Rider& r = g_r[p];
        if (!r.on) return;
        char code[256];
        snprintf(code, sizeof code, "local e=zt2mod_ride and zt2mod_ride[%d] if e then pcall(function() e:sendMessage('BFAI_ACTIVATE',true) end) "
                                    "pcall(function() e:sendMessage('ANIM_REQUEST',{seqToken='Stand_Idle',scale=1,time=0}) end) zt2mod_ride[%d]=nil end", p, p);
        Lua(code);
        LOG("animal: player %d lets go (%s)", p + 1, why);
        r = Rider{};
        r.takeLatch = true;
    }

    // The game's overhead camera target (BFG_GET_CAMERA_X/Y/Z read it): ZTWorldMgr C++ object +0x10 = camera entity,
    // +0x17c location component, +0xc node, node vfn +0x124 = position pointer (same pattern as animals, 0x500da0).
    // ZT20d (owner: ZT20c - nobody could ride; log 13:33: 'entity 0080eff3 has no position' for every animal, GetPos faulted).
    // That run the bridge picked the game's OTHER script context (globals with _BFGEntity), where an object's _this sits one
    // step closer to the native handle - the old fixed chain (peek(peek(peek(ptr)+4))) read a code address. zt2mod_nat tries
    // both layouts and only accepts a native object whose vtable is inside zt.exe's data (0x800000-0x9a0000).
    void DefineNat() {
        Lua("if not zt2mod_nat then zt2mod_nat=function(b) local P=zt2mod.peek local u=zt2mod.ptr(b._this) "
            "local function vt(x) if not x or x==0 or (x>=4194304 and x<10485760) then return false end local v=P(x) "
            " if not v or v<8388608 or v>=10092544 then return false end local f=P(v) return f and f>=4198400 and f<9437184 end "
            "local tries={} for _,h in ipairs({P(u),u}) do if h and h~=0 then local o=P(h+4) "
            " if o and o~=0 then for _,c in ipairs({P(o),P(o+4),o}) do table.insert(tries,c) if vt(c) and c~=h then return c,'ok' end end end end end "
            "return 0,'none:'..table.concat(tries,',') end end");
    }
    DWORD g_camNode = 0;
    bool GameCamTarget(float x, float y, float z) {
        if (!g_camNode) {
            char out[64];
            if (!Lua("local pk=zt2mod.peek local w=queryObject('ZTWorldMgr') if not w then return 0 end "
                     "local o=pk(pk(pk(zt2mod.ptr(w._this))+4)) local c=o and pk(o+16) local l=c and pk(c+380) local n=l and pk(l+12) "
                     "return string.format('%d', n or 0)", out, sizeof out)) return false;
            g_camNode = (DWORD)strtoul(out, nullptr, 10);
            LOG("animal: game camera node %08lx", g_camNode);
        }
        DWORD node = g_camNode, vt = Rd(node), fn = vt ? Rd(vt + 0x124) : 0;
        if (!fn) { g_camNode = 0; return false; }
        DWORD pp = 0;
        if (!GUARDED("animal:camPos", { pp = zt_GenericCall(fn, 0, node, nullptr, 0, nullptr); })) { g_camNode = 0; return false; }
        if (!Readable(pp, 12)) { g_camNode = 0; return false; }
        return GUARDED("animal:camSet", { float* f = (float*)pp; f[0] = x; f[1] = y; f[2] = z;
                                          DWORD a[1] = { node }; zt_GenericCall(kNodeUpd, 0, 0, a, 1, nullptr); });
    }

    bool g_testFar = false;   // test only (auto 'far'): take the nearest animal at any distance
    unsigned g_selPtr[2] = { 0, 0 }; DWORD g_selTick[2] = { 0, 0 };   // ZT19f: the animal each player last selected
    bool Take(int p, bool haveAim, float ax, float ay) {
        g_camNode = 0;   // re-read once per take (a new zoo has a new camera)
        Rider& r = g_r[p];
        char code[3600], out[256];
        // player 1: aim = the overhead camera's look-at point (= the middle of player 1's view, also in split-screen)
        snprintf(code, sizeof code,
            "local ax,ay=%.3f,%.3f "
            "if %d==0 then local w=queryObject('ZTWorldMgr') if w then ax=w:BFG_GET_CAMERA_X() ay=w:BFG_GET_CAMERA_Y() end end "
            // ZT19f (owner: 'it sometimes puts me in control of a different animal than I clicked' - log: player 2 selected
            // a Sarcosuchus, got the Goliath grouper nearer the aim): the animal this player selected in the last 60 s wins
            "local m=queryObject('BFGManager') local r=m and m:BFG_GET_ENTITIES('animal') local best,bd=nil,%.1f local pref=%u local pick=nil "
            "local other=zt2mod_ride and zt2mod_ride[%d] "
            "if r then for i=1,table.getn(r) do local e=resolveTable(r[i].value) "
            "  if e and (not other or zt2mod.ptr(other._this)~=zt2mod.ptr(e._this)) then local p=e:BFG_GET_ENTITY_POSITION() "
            "  if pref~=0 and zt2mod.ptr(e._this)==pref then pick=e end "
            "  local dx,dy=p.x-ax,p.y-ay local d=dx*dx+dy*dy if d<bd then best,bd=e,d end end end end "
            "if pick then best=pick end "
            "if not best then return string.format('none %%.1f %%.1f',ax,ay) end "
            "zt2mod_ride=zt2mod_ride or {} zt2mod_ride[%d]=best "
            "pcall(function() best:sendMessage('BFAI_ACTIVATE',false) end) "
            "local f=best:BFG_GET_ENTITY_FACING_DIRECTION() local q=best:BFG_GET_ENTITY_POSITION() "
            "local ent,ediag=zt2mod_nat(best) "
            "local ty='?' pcall(function() ty=best:BFG_GET_BINDER_TYPE() end) if type(ty)~='string' then ty=tostring(best._type or '?') end "
            // ZT19g: the water level near the animal = the height of the nearest fish / sea animal within 80 m (they float
            // at the surface); WaterZ does not work on the owner's zoo and the height test let a mastodon on a rock 'swim'
            "local wl=-9999 local wd=6400 if r then for i=1,table.getn(r) do local e=resolveTable(r[i].value) local t2='' "
            "  pcall(function() t2=e:BFG_GET_BINDER_TYPE() end) "
            "  for _,k in ipairs({'Grouper','Shark','Whale','Dolphin','Fish','Ray','Orca','Wrasse','Dorado','Manatee','Ichthyo','Plesio','Mosasaur','Cymbo','Dunkle','Liopleuro'}) do "
            "   if string.find(t2,k,1,true) then local p=e:BFG_GET_ENTITY_POSITION() local dx,dy=p.x-q.x,p.y-q.y local d=dx*dx+dy*dy "
            "    if d<wd then wd=d wl=p.z end break end end end end "
            "if ent==0 then pcall(function() best:sendMessage('BFAI_ACTIVATE',true) end) for k,v in pairs(zt2mod_ride) do if v==best then zt2mod_ride[k]=nil end end return 'noent '..tostring(ediag) end "
            "return string.format('ok %%d %%.4f %%.4f %%.3f %%.3f %%.3f %%s %%.3f',ent or 0,f.x,f.y,q.x,q.y,q.z,ty,wl)",
            ax, ay, p, g_testFar ? 1e12f : 30.f * 30.f, (g_selPtr[p] && GetTickCount() - g_selTick[p] < 60000) ? g_selPtr[p] : 0u, 1 - p, p);
        DefineNat();
        if (!Lua(code, out, sizeof out)) { LOG("animal: take failed: %s", out); return false; }
        unsigned ent = 0; float fx = 0, fy = 1, x = 0, y = 0, z = 0; char ty[64] = "?";
        float wl = -9999.f;
        if (sscanf(out, "ok %u %f %f %f %f %f %63s %f", &ent, &fx, &fy, &x, &y, &z, ty, &wl) < 6 || !ent) {
            LOG("animal: player %d - no animal near the aim (%s)", p + 1, out); return false; }
        float* pp = PosPtr(ent);
        if (!pp || !Node(ent)) { LOG("animal: entity %08x has no position/node - not taken", ent); Lua("zt2mod_ride=zt2mod_ride or {}"); return false; }
        r = Rider{};
        r.on = true; r.ent = ent; r.x = pp[0]; r.y = pp[1]; r.z = pp[2]; r.waterLvl = wl;
        if (wl > -9000.f) LOG("animal: water level near player %d's animal %.2f (from a fish / sea animal)", p + 1, wl);
        float l = sqrtf(fx * fx + fy * fy); if (l > 0.01f) { r.hx = fx / l; r.hy = fy / l; }
        r.camYaw = atan2f(r.hx, r.hy);   // camera behind the animal: forward on the ground = (sinY, cosY)
        r.takeLatch = true;
        strncpy(r.type, ty, sizeof r.type - 1); r.sf = FindSpecies(r.type);
        {   // ZT17c: crocodiles, turtles, seals... climb out onto land / rock islands; only true sea animals stay in the water
            static const char* kSea[] = { "whale", "dolphin", "shark", "orca", "beluga", "narwhal", "manatee", "dugong", "ray",
                "fish", "ichthyosaur", "plesiosaur", "mosasaur", "liopleurodon", "elasmosaur", "kronosaur", "shonisaur", "dunkleosteus",
                "squid", "octopus", "jelly", nullptr };
            char low[96] = ""; for (int i = 0; ty && ty[i] && i < 95; ++i) { low[i] = (char)tolower((unsigned char)ty[i]); low[i + 1] = 0; }
            r.landOK = true; for (int i = 0; kSea[i]; ++i) if (strstr(low, kSea[i])) r.landOK = false;
        }
        // ZT19e: the 13:56 crash = player 1 took a mastodon standing 0.6 m+ above the terrain (a rock / raised ground):
        // 'in the water' -> TreadWater anims on a land animal. Land-capable animals count as in the water only where the
        // game's terrain has water (WaterZ); sea animals (tanks) keep the old height test
        { float g; r.inWater = split::TerrainZ(r.x, r.y, g) && r.z - g > 0.6f && WaterHere(r, r.x, r.y, g); r.aquatic = r.inWater; r.waterTop = r.z; }
        {   // ZT17: the facing message alone does not turn the model (owner: LB/RB turned only the camera). The position we write
            // is the scene node's local translation; the node's 3x3 rotation sits right before it (Gamebryo NiTransform).
            DWORD node = Node(ent); DWORD off = (DWORD)pp - node; float* m = pp - 9;
            if (off > 0 && off < 0x200 && Readable((DWORD)m, 36)) {
                float det = m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6]) + m[2]*(m[3]*m[7]-m[4]*m[6]);
                float r0 = m[0]*m[0]+m[1]*m[1]+m[2]*m[2];
                r.rotOK = fabsf(det - 1.f) < 0.05f && fabsf(r0 - 1.f) < 0.05f;
                memcpy(r.rot0, m, 36); r.a0 = atan2f(r.hx, r.hy);
                LOG("animal: node %08lx position at +%03lx, rotation det %.3f row %.3f -> %s", node, off, det, r0, r.rotOK ? "model turns with the heading" : "not a rotation, facing message only");
            } else {
                LOG("animal: position %08lx is not inside node %08lx (+%lx) - facing message only", (DWORD)pp, node, off);
                // ZT20b (owner: the ridden T-Rex model disappears and pops up in other spots while walking): for this model the
                // entity position is not the scene node's, so the model only followed when the engine synced it now and then.
                // The node's own translation (vfn +0x124) is written too - only if it holds the same world position.
                DWORD vt = Rd(node), fn = vt ? Rd(vt + 0x124) : 0, np = 0;
                if (fn) GUARDED("animal:nodePos", { np = zt_GenericCall(fn, 0, node, nullptr, 0, nullptr); });
                if (np && np != (DWORD)pp && Readable(np, 12)) {
                    float* q = (float*)np;
                    bool same = fabsf(q[0] - pp[0]) < 2.f && fabsf(q[1] - pp[1]) < 2.f && fabsf(q[2] - pp[2]) < 3.f;
                    LOG("animal: node translation %08lx = %.1f %.1f %.1f (entity %.1f %.1f %.1f) -> %s", np, q[0], q[1], q[2], pp[0], pp[1], pp[2],
                        same ? "written with the entity position" : "different space, left alone");
                    if (same) r.nodePos = q;
                }
            }
        }
        r.rbLatch = true;   // RB of the take combo is still held: no turning until it is released
        if (r.aquatic) LOG("animal: player %d's animal is in the water - it stays in the water", p + 1);
        LOG("animal: player %d takes control of entity %08x (%s, species %s, fights: %s) at %.1f %.1f %.1f", p + 1, ent, r.type,
            r.sf ? r.sf->sp : "unknown", !r.sf ? "generic" : r.sf->cls == 'A' ? "attacks" : r.sf->cls == 'D' ? "defends" : "no", r.x, r.y, r.z);
        return true;
    }
}

namespace {
    char g_hint[2][96]; float g_hintUntil[2] = { 0.f, 0.f };
    void Hint(int p, float now, const char* t) { snprintf(g_hint[p], sizeof g_hint[p], "%s", t); g_hintUntil[p] = now + 3.f; }
    bool BigType(const char* ty) {   // ZT19: binder type contains a big_animals word
        const char* l = config::Get().bigAnimals; char w[64];
        while (*l) { int n = 0; while (l[n] && l[n] != ',' && n < 63) { w[n] = l[n]; ++n; } w[n] = 0; l += n; if (*l == ',') ++l;
                     if (n && ty && strstr(ty, w)) return true; }
        return false;
    }
    // the ridden animal's attack lands on the nearest other animal in front of it within reach: the game's own sickness
    // meter ("health", 0 = healthy, SuperStaff heal sets 0) goes up by the damage (capped), the victim plays its own hit
    // reaction if the species has one
    void Hit(int p, float dmg, float now) {
        const Config& c = config::Get();
        static char code[8192]; char out[200];
        snprintf(code, sizeof code,
            "local e=zt2mod_ride and zt2mod_ride[%d] if not e then return 'none' end "
            "local q=e:BFG_GET_ENTITY_POSITION() local f=e:BFG_GET_ENTITY_FACING_DIRECTION() "
            "local me=zt2mod.ptr(e._this) local m=queryObject('BFGManager') local r=m and m:BFG_GET_ENTITIES('animal') "
            // ZT19 (owner: a bite on a woolly mammoth missed): the reach counts from the animals' middles, so a big biter
            // (Sarcosuchus ~10 m long) or a big target (mammoth) needs +3 m each (big_animals list)
            "local best,bd=nil,1e9 local base=%.2f local mg=%.2f local bigl='%s' "
            "local function isbig(s) for w in string.gfind(bigl,'[^,]+') do if string.find(s,w,1,true) then return true end end return false end "
            "local dead={} for _,d in ipairs(zt2mod_dying or {}) do pcall(function() dead[zt2mod.ptr(d.e._this)]=1 end) end "
            "if r then for i=1,table.getn(r) do local t=resolveTable(r[i].value) "
            "  if t and zt2mod.ptr(t._this)~=me and not dead[zt2mod.ptr(t._this)] then local ok,tp=pcall(function() return t:BFG_GET_ENTITY_POSITION() end) "
            "   if ok and tp then local dx,dy=tp.x-q.x,tp.y-q.y local d=math.sqrt(dx*dx+dy*dy) "
            "    if d<bd and d<base+12 then local tt='' pcall(function() tt=t:BFG_GET_BINDER_TYPE() end) "
            "     if d<base+(isbig(tt) and 3 or 0) and (d<1.5+mg+(isbig(tt) and 3 or 0) or (dx*f.x+dy*f.y)>0.25*d) then best,bd=t,d end end end end end end "
            // ZT19i (owner): guests and staff (zookeepers...) can be bitten too - 3 bites; never player 2's keeper body
            "local hum=false if m then local kb=zt2mod_keeper and zt2mod.ptr(zt2mod_keeper._this) "
            " for _,kind in ipairs({'guest','staff','keeper'}) do local okk,hr=pcall(function() return m:BFG_GET_ENTITIES(kind) end) "
            "  if okk and type(hr)=='table' then for i=1,table.getn(hr) do local t=resolveTable(hr[i].value) "
            "   if t and not dead[zt2mod.ptr(t._this)] and zt2mod.ptr(t._this)~=kb then local ok,tp=pcall(function() return t:BFG_GET_ENTITY_POSITION() end) "
            "    if ok and tp then local dx,dy=tp.x-q.x,tp.y-q.y local d=math.sqrt(dx*dx+dy*dy) "
            "     if d<bd and d<base and (d<1.5+mg or (dx*f.x+dy*f.y)>0.25*d) then best,bd,hum=t,d,true end end end end end end end "
            "if not best then "
            // no animal: a breakable fence segment in front (the game's own rules: not the zoo wall, has an s_Broken type,
            // f_FenceStrength <= max like the Triceratops BashFence task) -> after N hits it morphs into its broken type
            "  if %d==0 then return 'miss' end "
            "  local fr=m and m:BFG_GET_ENTITIES('fence') local fb,fd=nil,%.2f "
            "  if fr then for i=1,table.getn(fr) do local t=resolveTable(fr[i].value) "
            "    if t then local ok,tp=pcall(function() return t:BFG_GET_ENTITY_POSITION() end) "
            "     if ok and tp then local dx,dy=tp.x-q.x,tp.y-q.y local d=math.sqrt(dx*dx+dy*dy) "
            "      if d<fd and (d<2 or (dx*f.x+dy*f.y)>0.3*d) then local ty,bk,st='','',99 "
            "       pcall(function() ty=t:BFG_GET_BINDER_TYPE() bk=t:BFG_GET_ATTR_STRING('s_Broken') st=t:BFG_GET_ATTR_FLOAT('f_FenceStrength') end) "
            "       if ty~='zoowall' and type(bk)=='string' and bk~='' and st and st<=%.1f then fb,fd=t,d end end end end end end "
            "  if not fb then return 'miss' end "
            "  zt2mod_fhit=zt2mod_fhit or {} local id=tostring(zt2mod.ptr(fb._this)) zt2mod_fhit[id]=(zt2mod_fhit[id] or 0)+1 "
            "  local ty=fb:BFG_GET_BINDER_TYPE() local bk=fb:BFG_GET_ATTR_STRING('s_Broken') "
            "  pcall(function() fb:BFG_PLAY_PARTICLE('AttackedCloud') end) "
            "  if zt2mod_fhit[id]>=%d then zt2mod_fhit[id]=nil local ok,r=pcall(function() return fb:BFG_ENTITY_MORPH_TO_NEW_ENTITY(bk) end) "
            "    return string.format('fencebroken %%s -> %%s %%s',ty,bk,tostring(ok and r)) end "
            "  return string.format('fence %%d %%s %%.1f',zt2mod_fhit[id],ty,fd) "
            "end "
            "pcall(function() best:BFG_PLAY_PARTICLE('AttackedCloud') end) "
            "local ty='?' pcall(function() ty=best:BFG_GET_BINDER_TYPE() end) "
            "local h0=-1 pcall(function() h0=best:BFG_GET_ATTR_FLOAT('health') end) "
            // ZT18d (owner, JPOG style): a victim is beaten after N bites - 5 for small / medium animals, 10 for big ones
            // (big_animals list, matched on the binder type); each bite adds cap/N to the game's sickness meter
            "local N=%d local big='%s' for w in string.gfind(big,'[^,]+') do if string.find(ty,w,1,true) then N=%d break end end if hum then N=3 end "
            "zt2mod_ahit=zt2mod_ahit or {} local aid=tostring(zt2mod.ptr(best._this)) local n=math.min(N,(zt2mod_ahit[aid] or 0)+1) zt2mod_ahit[aid]=n "
            "local h1=h0 if h0>=0 then local cap=%.1f h1=math.min(cap,math.max(h0,cap*n/N)) if n>=N then h1=cap end "
            "  pcall(function() best:BFG_SET_ATTR_FLOAT('health',h1) end) pcall(function() h1=best:BFG_GET_ATTR_FLOAT('health') end) end "
            // ZT19g (owner: a sleeping animal must wake up at the first bite): rested (rest need 0), its AI restarted
            // (BFAI off/on drops the sleep task), and its own wake / stand-up anim
            "pcall(function() best:BFG_SET_ATTR_FLOAT('rest',0) end) pcall(function() best:sendMessage('BFAI_ACTIVATE',false) end) "
            "pcall(function() best:sendMessage('BFAI_ACTIVATE',true) end) "
            "for _,n in ipairs({'Sleep_Wake','Sleep_WakeUp','Sleep_StandUp','Lie_StandUp','Lie_GetUp','Lie_Stand','Stand_Up'}) do "
            "  local ok,res=pcall(function() return best:sendMessage('ANIM_REQUEST',{seqToken=n,scale=1,time=0}) end) "
            "  if ok and res=='ANIM_REQUEST_SUCCESS' then break end end "
            "local an='' for _,n in ipairs({'Stand_GetHitLeft','Stand_BeHitLeft','Stand_BeHit','Stand_2Attacked_BeHitLeft','Stand_Attacked','Stand_2Attacked','Attacked_Idle'}) do "
            "  local ok,res=pcall(function() return best:sendMessage('ANIM_REQUEST',{seqToken=n,scale=1,time=0}) end) "
            "  if ok and res=='ANIM_REQUEST_SUCCESS' then an=n break end end "
            // ZT18d (owner: beaten animals die): the game's own fight death (Die_Fight in the species .beh) morphs the
            // animal into 'Carcass_Meat' - same call. Never the animal the other player rides.
            // ZT19e (owner: the beaten mammoth kept walking, then 'died' still breathing, got up, crash): a beaten animal stops
            // at once (AI off), plays the species' own death / lie-down anim, stays down, and is removed 5 s after it lies
            // down (DyingTick: the game's deleteEntity; old carcass morph only if that fails)
            "if n>=N then local rid=false for k,v in pairs(zt2mod_ride or {}) do if v and zt2mod.ptr(v._this)==zt2mod.ptr(best._this) then rid=true end end "
            "  if not rid then zt2mod_ahit[aid]=nil pcall(function() best:sendMessage('BFAI_ACTIVATE',false) end) local dn='' "
            "    for _,a in ipairs({'Die_Fight','Die','Stand_Die','Die_A','Stand_LieDown','Lie_Down','Stand_Lie','Faint','Stand_Faint','Knockdown','Stand_Knockdown','Fall_Down','Lie_Idle','Sleep_Idle'}) do "
            "      local ok,res=pcall(function() return best:sendMessage('ANIM_REQUEST',{seqToken=a,scale=1,time=0}) end) "
            "      if ok and res=='ANIM_REQUEST_SUCCESS' then dn=a break end end "
            "    if hum and dn=='' then local okd=pcall(function() deleteEntity(best) end) return string.format('killed %%s removed-now %%s',tostring(ty),tostring(okd)) end "
            "    zt2mod_dying=zt2mod_dying or {} table.insert(zt2mod_dying,{e=best,t=zt2mod_now or 0,a=0}) "
            "    return string.format('killed %%s %%s',tostring(ty),dn) end "
            // ZT19t (owner): the OTHER PLAYER's animal beaten -> that player is put back in builder mode (C++ lets go), then the
            // animal gets the same lie-down-and-disappear as any beaten animal
            "  local ri=-1 for k,v in pairs(zt2mod_ride or {}) do if v and zt2mod.ptr(v._this)==zt2mod.ptr(best._this) then ri=k end end "
            "  zt2mod_ahit[aid]=nil zt2mod_beaten=best return string.format('riderbeaten %%d %%s',ri,tostring(ty)) end "
            "return string.format('hit %%s %%.1f %%.1f %%.1f %%d %%d %%s',tostring(ty),bd,h0,h1,n,N,an)",
            p, c.attackReach + c.attackMargin + (BigType(g_r[p].type) ? 3.f : 0.f), c.attackMargin, c.bigAnimals, c.fenceBreak, c.attackReach + 2.f + c.attackMargin, c.fenceMaxStrength, c.fenceHits,
            c.biteHitsSmall, c.bigAnimals, c.biteHitsBig, c.damageCap);
        (void)dmg;
        { char nw[64]; snprintf(nw, sizeof nw, "zt2mod_now=%.2f", now); Lua(nw); }
        if (!Lua(code, out, sizeof out)) { LOG("animal: player %d hit check failed: %.120s", p + 1, out); return; }
        char* nl = strchr(out, '\n'); if (nl) *nl = 0;
        LOG("animal: player %d attack -> %s", p + 1, out);
        int n = 0; char t[96];
        char vt[64]; int bn = 0, bN = 0; float bd = 0, h0 = 0, h1 = 0;
        if (!strncmp(out, "miss", 4)) { Hint(p, now, "Missed - get closer!"); return; }   // ZT19: say so (owner saw nothing)
        { int ri = -1; if (sscanf(out, "riderbeaten %d %63s", &ri, vt) == 2) {   // ZT19t: the other player's animal is beaten
            if (char* u = strchr(vt, '_')) *u = 0;
            if (ri == 0 || ri == 1) { DoRelease(ri, "beaten in a fight"); Hint(ri, now, "Your animal was beaten!"); }
            char kc[1200], ko[96];
            snprintf(kc, sizeof kc, "local best=zt2mod_beaten zt2mod_beaten=nil if not best then return '' end "
                "pcall(function() best:sendMessage('BFAI_ACTIVATE',false) end) local dn='' "
                "for _,a in ipairs({'Die_Fight','Die','Stand_Die','Die_A','Stand_LieDown','Lie_Down','Stand_Lie','Lie_Idle','Sleep_Idle'}) do "
                "  local ok,res=pcall(function() return best:sendMessage('ANIM_REQUEST',{seqToken=a,scale=1,time=0}) end) "
                "  if ok and res=='ANIM_REQUEST_SUCCESS' then dn=a break end end "
                "zt2mod_dying=zt2mod_dying or {} table.insert(zt2mod_dying,{e=best,t=%.2f,a=0}) return dn", now);
            Lua(kc, ko, sizeof ko);
            LOG("animal: player %d beat player %d's %s - %s let go, lying down (%s)", p + 1, ri + 1, vt, ri >= 0 ? "rider" : "?", ko);
            snprintf(t, sizeof t, "%s was beaten!", vt); Hint(p, now, t); return; } }
        if (sscanf(out, "killed %63s", vt) == 1) { if (char* u = strchr(vt, '_')) *u = 0;
            snprintf(t, sizeof t, "%s was killed!", vt); Hint(p, now, t); return; }
        if (sscanf(out, "hit %63s %f %f %f %d %d", vt, &bd, &h0, &h1, &bn, &bN) == 6 && bN > 0) {   // ZT18d: JPOG-style bite count
            if (char* u = strchr(vt, '_')) *u = 0;
            if (bn >= bN) snprintf(t, sizeof t, "%s: %d / %d bites - beaten!", vt, bN, bN);
            else snprintf(t, sizeof t, "%s: %d / %d bites", vt, bn, bN);
            Hint(p, now, t);
        }
        else if (sscanf(out, "fence %d", &n) == 1) {
            if (n == 1) snprintf(t, sizeof t, "Fence: 1 / %d bites - it breaks at %d", c.fenceHits, c.fenceHits);   // ZT18d
            else snprintf(t, sizeof t, "Fence: %d / %d bites", n, c.fenceHits);
            Hint(p, now, t); }
        else if (!strncmp(out, "fencebroken", 11)) Hint(p, now, "Fence broken!");
    }
}

namespace {
    // ZT13: A = drink / eat only where the game's own AI would: a shore (the DrinkWaterTerrain task's target) or a water
    // object within 4 m -> drink (thirst -40, EmoteAnimalThirstGood); food within 4 m -> eat (hunger -30,
    // EmoteAnimalHungerGood); nothing -> sniff + hint. Returns 'drink', 'eat' or 'none'.
    const char* FeedCheck(int p) {
        static char out[96];
        char code[1600];
        snprintf(code, sizeof code,
            "local e=zt2mod_ride and zt2mod_ride[%d] if not e then return 'none' end local q=e:BFG_GET_ENTITY_POSITION() "
            "local m=queryObject('BFGManager') "
            "local function near(kind, filt) local ok,r=pcall(function() return m:BFG_GET_ENTITIES(kind) end) if not ok or not r then return nil end "
            "  for i=1,table.getn(r) do local t=resolveTable(r[i].value) if t then local ok2,tp=pcall(function() return t:BFG_GET_ENTITY_POSITION() end) "
            "   if ok2 and tp then local dx,dy=tp.x-q.x,tp.y-q.y if dx*dx+dy*dy<16 then local ty='' pcall(function() ty=t:BFG_GET_BINDER_TYPE() end) "
            "    if not filt or filt(string.lower(ty)) then return ty end end end end end return nil end "
            "local w=near('shore') or near('food',function(t) return string.find(t,'water') or string.find(t,'drink') end) "
            "local function adj(a,d,emote) pcall(function() local v=e:BFG_GET_ATTR_FLOAT(a) e:BFG_SET_ATTR_FLOAT(a,math.max(0,v+d)) end) "
            "  pcall(function() e:BFG_PLAY_PARTICLE(emote) end) end "
            "if w then adj('thirst',-40,'EmoteAnimalThirstGood') return 'drink '..w end "
            "local f=near('food',function(t) return not string.find(t,'water') end) "
            "if f then adj('hunger',-30,'EmoteAnimalHungerGood') return 'eat '..f end return 'none'", p);
        if (!Lua(code, out, sizeof out)) return "none";
        char* nl = strchr(out, '\n'); if (nl) *nl = 0;
        return out;
    }
    void SleepTick(int p) {
        char code[300];
        snprintf(code, sizeof code, "local e=zt2mod_ride and zt2mod_ride[%d] if not e then return end "
            "pcall(function() local v=e:BFG_GET_ATTR_FLOAT('rest') e:BFG_SET_ATTR_FLOAT('rest',math.max(0,v-10)) end) "
            "pcall(function() e:BFG_PLAY_PARTICLE('EmoteAnimalRestVeryGood') end)", p);
        Lua(code);
    }
}

namespace {
    // ZT12: ridden animals do not walk through fences, walls or gates (JPOG rule: a fence only goes after the bites that
    // break it). Intact segment centres are cached from BFG_GET_ENTITIES('fence') every 3 s while someone rides; a step
    // that ends closer than kFenceKeep to a centre AND gets closer is refused (moving away always works). Curbs and broken
    // segments do not block. A step off the terrain is refused too (a ridden lion that crossed the zoo wall crashed the game).
    constexpr float kFenceKeep = 3.0f;
    std::vector<float> g_fx, g_fy; float g_fenceAt = -100.f;
    void RefreshFences(float now) {
        if (now - g_fenceAt < 3.f) return;
        g_fenceAt = now;
        static char out[24576];
        if (!Lua("local m=queryObject('BFGManager') local r=m and m:BFG_GET_ENTITIES('fence') if not r then return '' end local t={} "
                 "for i=1,table.getn(r) do local e=resolveTable(r[i].value) if e then local ty='' pcall(function() ty=e:BFG_GET_BINDER_TYPE() end) "
                 " if not string.find(ty,'curb') and not string.find(ty,'_broken') then local ok,p=pcall(function() return e:BFG_GET_ENTITY_POSITION() end) "
                 "  if ok and p then table.insert(t,string.format('%.1f %.1f',p.x,p.y)) end end end end return table.concat(t,';')", out, sizeof out)) return;
        g_fx.clear(); g_fy.clear();
        for (char* q = out; *q; ) {
            float x, y; int n = 0;
            if (sscanf(q, "%f %f%n", &x, &y, &n) != 2) break;
            g_fx.push_back(x); g_fy.push_back(y);
            q += n; while (*q == ';' || *q == ' ') ++q;
            if (*q == '\n' || *q == '\r') break;
        }
    }
    float NearestFence(float x, float y) {
        float best = 1e9f;
        for (size_t i = 0; i < g_fx.size(); ++i) { float dx = g_fx[i] - x, dy = g_fy[i] - y, d = dx * dx + dy * dy; if (d < best) best = d; }
        return sqrtf(best);
    }
    bool StepAllowed(float ox, float oy, float nx, float ny, bool aquatic = false, float waterTop = 0.f) {
        float h;
        if (!split::TerrainZ(nx, ny, h)) return false;
        if (aquatic && h > waterTop - 0.4f) return false;   // ZT14: a water animal stays where the bottom is under its swim height
        float dn = NearestFence(nx, ny);
        if (dn >= kFenceKeep) return true;
        return dn > NearestFence(ox, oy);   // already close: only moving away
    }
}

namespace animal {
bool Riding(int p) { return p >= 0 && p < 2 && g_r[p].on; }
void ReleaseAll() { DoRelease(0, "reset"); DoRelease(1, "reset"); }
void Release(int p) { if (p >= 0 && p < 2) ::DoRelease(p, "player 2 pad gone"); }

// ZT17b: the game rewrites the animal's node rotation in its own update (from its facing, which our facing message does
// not turn), so the heading is written right before each draw, then the node's world transform is rebuilt.
void ApplyTurn() {
    const auto& c = config::Get();
    for (int p = 0; p < 2; ++p) {
        Rider& r = g_r[p];
        if (!r.on) continue;
        float* pp = PosPtr(r.ent); DWORD node = Node(r.ent);
        if (!pp || !node) continue;
        if (!r.rotOK || !c.animalNodeRotate) {   // no turn write: still hold a diving animal under the surface
            // ZT18d: only while DIVING (depth > 0). At the surface the engine floats / grounds the animal itself - pinning it to
            // waterTop kept a crocodile in the air after it walked off a cliff (the height gap read as "in water").
            if (r.aquatic && r.inWater && r.depth > 0.01f) GUARDED("animal:dive", { pp[2] = r.waterTop - r.depth;
                DWORD a3[1] = { node }; zt_GenericCall(kNodeUpd, 0, 0, a3, 1, nullptr); });
            continue;
        }
        float t = (c.animalNodeRotate > 0 ? -1.f : 1.f) * (atan2f(r.hx, r.hy) - r.a0), cs = cosf(t), sn = sinf(t);
        GUARDED("animal:turn", {
            float* m = pp - 9; const float* o = r.rot0;
            for (int k = 0; k < 3; ++k) { m[k] = cs * o[k] - sn * o[3 + k]; m[3 + k] = sn * o[k] + cs * o[3 + k]; m[6 + k] = o[6 + k]; }
            if (r.aquatic && r.inWater && r.depth > 0.01f) pp[2] = r.waterTop - r.depth;   // ZT18d: diving only
            DWORD a2[1] = { node }; zt_GenericCall(kNodeUpd, 0, 0, a2, 1, nullptr);
        });
    }
}
bool RidePose(int p, float& tx, float& ty, float& tz, float& yaw, float& pitch, float& dist) {
    if (!Riding(p)) return false;
    const Rider& r = g_r[p];
    // ZT19g: smoothed height. ZT19q FIX: ZT19g's comment had swallowed 'pitch = ...; dist = ...;' on this line -> camera
    // pitch / distance were never set (camera on the animal's back, D-pad zoom dead) since ZT19g
    tx = r.x; ty = r.y; tz = (r.camZ > -9000.f ? r.camZ : r.z) + 1.2f; yaw = r.camYaw;
    pitch = r.camPitch; dist = r.camDist;
    return true;
}

In FromPad(const XPad& x) {
    In i; i.lx = x.LX(); i.ly = x.LY(); i.rx = x.RX(); i.ry = x.RY(); i.lt = x.LT(); i.rt = x.RT();
    for (int b = 0; b < PAD_COUNT; ++b) { i.down[b] = x.Down((PadButton)b); i.prev[b] = x.Down((PadButton)b) && !x.Pressed((PadButton)b) || (!x.Down((PadButton)b) && x.Released((PadButton)b)); }
    return i;
}
bool Frame(int p, const In& pad, float dt, float now, bool haveAim, float aimX, float aimY, bool keyTake) {
    Rider& r = g_r[p];
    const bool takeCombo = pad.D(PAD_RB) && pad.P(PAD_DUP);
    if (!r.on) {
        if (r.takeLatch && !pad.D(PAD_RB) && !pad.D(PAD_B)) r.takeLatch = false;
        if ((takeCombo || keyTake) && !r.takeLatch) Take(p, haveAim, aimX, aimY);
        return r.on;
    }
    // --- still alive? (entity deleted / zoo unloaded) ---
    if (now >= r.checkAt) {
        r.checkAt = now + 0.5f;
        char out[160];
        // ZT19 (owner: the ridden Sarcosuchus "got put in a box" twice): the game crated the ridden animal (its own
        // isCratedEntity / uncrateEntity script functions, the same the crate's Uncrate button uses) -> take it straight
        // back out and keep riding; the AI stays off
        char code[900]; snprintf(code, sizeof code, "local e=zt2mod_ride and zt2mod_ride[%d] if not e then return 'gone' end "
            "local ok=pcall(function() return e:BFG_GET_ENTITY_POSITION() end) if not ok then return 'gone' end "
            "local okc,cr=pcall(function() return isCratedEntity(e) end) "
            "if okc and cr and cr~=0 then local m=getCurrentModeName and getCurrentModeName() or '?' "
            "  local ok2,res=pcall(function() return uncrateEntity(e) end) "
            "  pcall(function() e:sendMessage('BFAI_ACTIVATE',false) end) "
            "  local ok3,c2=pcall(function() return isCratedEntity(e) end) "
            "  return 'crated mode='..tostring(m)..' uncrate='..tostring(ok2)..':'..tostring(res)..' still='..tostring(ok3 and c2) end "
            // ZT19y (owner: the ridden T-Rex sometimes got stuck sliding in a bent-down eat / drink pose): the game's own
            // needs still push eat / drink behaviour - every 0.5 s the AI is kept off and hunger / thirst kept satisfied
            "pcall(function() e:sendMessage('BFAI_ACTIVATE',false) end) "
            "for _,n in ipairs({'hunger','thirst'}) do pcall(function() e:BFG_SET_ATTR_FLOAT(n,0) end) end "
            "return okc and 'ok' or ('ok nocheck '..tostring(cr))", p);
        if (Lua(code, out, sizeof out)) {
            char* nl = strchr(out, '\n'); if (nl) *nl = 0;
            if (!strncmp(out, "gone", 4)) { DoRelease(p, "animal gone"); return false; }
            if (!strncmp(out, "crated", 6)) {
                LOG("animal: player %d's animal was put in a crate by the game - %s", p + 1, out);
                if (float* q = PosPtr(r.ent)) { r.x = q[0]; r.y = q[1]; r.z = q[2]; }
                Hint(p, now, "Out of the crate - keep going!");
            }
            static bool s_nc = false; if (!s_nc && !strncmp(out, "ok nocheck", 10)) { s_nc = true; LOG("animal: crate check unavailable: %s", out); }
        }
    }
    if (pad.P(PAD_B) || keyTake) { DoRelease(p, pad.P(PAD_B) ? "B" : "key"); return false; }

    float* pp = PosPtr(r.ent);
    DWORD node = Node(r.ent);
    if (!pp || !node) { DoRelease(p, "entity lost"); return false; }

    // --- camera: RS orbit / tilt, D-pad up/down zoom, left/right height ---
    r.camYaw += pad.rx * 2.2f * dt;
    r.camPitch = Clamp(r.camPitch + (-pad.ry * 1.2f + (pad.D(PAD_DRIGHT) ? 0.8f : 0.f) - (pad.D(PAD_DLEFT) ? 0.8f : 0.f)) * dt, config::Get().sky ? 0.15f : 0.38f, 1.35f);   // ZT18f: 0.15 with the painted sky; ZT18d: min 0.38 (was 0.12) - lower showed the undrawn (black) sky; stick up = look up (owner: was inverted)
    r.camDist = Clamp(r.camDist * (1.f + ((pad.D(PAD_DDOWN) ? 1.f : 0.f) - (pad.D(PAD_DUP) ? 1.f : 0.f)) * 1.2f * dt), 5.f, 60.f);

    // ZT17: the game didn't start a 3D sound for the call / bite -> play the species' own sound from the game data
    if (r.sndAnim && now - r.sndAt > 0.35f) {
        if (sndwatch::Count3D() == r.snd3d) { LOG("animal: player %d's %s made no sound - playing it from the game data", p + 1, r.sndAnim); callsnd::Play(r.type, r.sndAnim); }
        r.sndAnim = nullptr;
    }
    // --- one-shot actions ---
    auto Act = [&](const char* const* list, float secs, const char* what) {
        const char* n = Anim(p, list);
        LOG("animal: player %d %s -> %s", p + 1, what, n ? n : "(none for this species)");
        if (n) { sndwatch::Watch(n, 1.2f); r.sndAnim = n; r.sndAt = now; r.snd3d = sndwatch::Count3D(); }
        if (n) { r.busyUntil = now + secs; r.loco = L_NONE; }
    };
    static bool s_rtPrev[2], s_ltPrev[2];
    bool rt = pad.rt > 0.5f, lt = pad.lt > 0.5f;
    bool rtP = rt && !s_rtPrev[p], ltP = lt && !s_ltPrev[p]; s_rtPrev[p] = rt; s_ltPrev[p] = lt;
    if (pad.P(PAD_X)) {             // ZT14 (JPOG layout: LB/RB turn): X = lie down -> sleep -> stand up
        if (r.pose == P_STAND)      { Act(kLie, 1.6f, "lies down");  r.pose = P_LIE; }
        else if (r.pose == P_LIE)   { Act(kSleep, 0.5f, "sleeps");   r.pose = P_SLEEP; r.sleepTickAt = now; }
        else                        { Act(kStand, 1.6f, "stands up"); r.pose = P_STAND; }
    }
    if (r.pose == P_SLEEP && now >= r.sleepTickAt) { r.sleepTickAt = now + 6.f; SleepTick(p); }   // vanilla rest emoticon + rest need
    // ZT13: keep lying / sleeping (vanilla BFBehAnimate playTime=20; one ANIM_REQUEST alone played one cycle and the animal
    // popped back up): re-request the pose's idle every 1.8 s
    if ((r.pose == P_LIE || r.pose == P_SLEEP) && now >= r.busyUntil && now >= r.poseAnimAt) {
        static const char* kLieLoop[] = { "Lie_Idle", nullptr };
        static const char* kSleepLoop[] = { "Sleep_Idle", "SleepPaws_Idle", "SleepStand_Idle", "Lie_Idle", nullptr };
        Anim(p, r.pose == P_SLEEP ? kSleepLoop : kLieLoop, 0.f);
        r.poseAnimAt = now + 1.8f;   // ZT13 test: 20 s play time + 4 s still popped up between requests
    }
    if (r.pose == P_STAND && now >= r.busyUntil) {
        if (rtP) {
            if (r.sf && r.sf->cls == 'N') Act(kNoFight, 1.2f, "can't fight (this species doesn't attack)");
            else {
                // the species' own attack anims from the game data, taking turns (left/right swipes, bite/kick ...)
                const char* list[8] = {}; int n = 0;
                if (r.sf) { int cnt = 0; while (cnt < 6 && r.sf->atk[cnt]) ++cnt;
                            for (int i = 0; i < cnt; ++i) list[n++] = r.sf->atk[(i + r.atkN) % cnt]; ++r.atkN; }
                else for (int i = 0; kAttack[i] && n < 7; ++i) list[n++] = kAttack[i];
                list[n] = nullptr;
                Act(list, 1.4f, r.sf && r.sf->cls == 'D' ? "defends" : "attacks");
                Hit(p, r.sf && r.sf->cls == 'A' ? config::Get().attackDamage : config::Get().defendDamage, now);
            }
        }
        else if (ltP && !(r.aquatic && r.inWater)) Act(kSpecial, 1.6f, "special move");
        else if (pad.P(PAD_A) && !(r.aquatic && r.inWater && r.depth > 0.05f)) {   // ZT19: under water A = swim up
            const char* f = FeedCheck(p);
            LOG("animal: player %d A -> %s", p + 1, f);
            if (!strncmp(f, "drink", 5)) Act(kDrink, 2.5f, "drinks");
            else if (!strncmp(f, "eat", 3)) Act(kFeed, 2.5f, "eats");
            else { Act(kSniff, 1.5f, "sniffs (no water or food here)"); Hint(p, now, "No water or food here"); }
        }
    }
    if (pad.P(PAD_Y)) { r.yHeldAt = now; r.yLong = false; }
    // ZT14: Y = the species' iconic call from the game data (roar, trumpet, bray, laugh, howl...); hold Y = its second call
    const char* const* call1 = (r.sf && r.sf->call[0]) ? r.sf->call : kCall;
    const char* const* call2 = (r.sf && r.sf->call[0] && r.sf->call[1]) ? r.sf->call + 1 : call1;
    if (pad.D(PAD_Y) && r.yHeldAt >= 0 && !r.yLong && now - r.yHeldAt > 0.4f) {
        r.yLong = true; if (r.pose == P_STAND) Act(call2, 2.2f, "second call"); }
    if (pad.R(PAD_Y)) { if (!r.yLong && r.pose == P_STAND) Act(call1, 2.2f, "calls"); r.yHeldAt = -1.f; }
    if (pad.P(PAD_LS)) { r.run = !r.run; LOG("animal: player %d run %s", p + 1, r.run ? "on" : "off"); }

    // --- movement: stick relative to the player's camera; the animal turns towards it and walks forward ---
    const Config& c = config::Get();
    RefreshFences(now);
    const float oldX = r.x, oldY = r.y;
    { float g; if (split::TerrainZ(r.x, r.y, g)) r.inWater = (r.depth > 0.01f ? (r.waterTop - g > 0.6f) : (pp[2] - g > 0.6f)) && WaterHere(r, r.x, r.y, g); }
    // JPOG layout: hold LB / RB = turn the ANIMAL left / right on the spot; the camera stays where it is (owner 19:16)
    if (r.rbLatch && !pad.D(PAD_RB)) r.rbLatch = false;
    float turnIn = (pad.D(PAD_RB) && !r.rbLatch ? 1.f : 0.f) - (pad.D(PAD_LB) ? 1.f : 0.f);
    bool turning = false;
    if (turnIn != 0.f && r.pose == P_STAND && now >= r.busyUntil) {
        float cur = atan2f(r.hx, r.hy) + turnIn * c.animalTurn * 0.8f * dt;
        r.hx = sinf(cur); r.hy = cosf(cur);
        turning = true;
    }
    float lx = pad.lx, ly = pad.ly;
    float mag = sqrtf(lx * lx + ly * ly); if (mag > 1.f) mag = 1.f;
    Loco want = L_IDLE;
    if (r.pose == P_STAND && now >= r.busyUntil && mag > 0.05f) {
        float sy = sinf(r.camYaw), cy = cosf(r.camYaw);
        float wx = sy * ly + cy * lx, wy = cy * ly - sy * lx;
        float wl = sqrtf(wx * wx + wy * wy);
        // JPOG strafe_back: stick down or mostly sideways = step that way on the screen without turning, never running
        bool strafe = c.animalStrafe && ly < 0.35f && (ly < -0.3f || fabsf(lx) > fabsf(ly) + 0.15f);
        if (wl > 0.001f && strafe) {
            wx /= wl; wy /= wl;
            float speed = c.animalWalk * 0.5f * (0.4f + 0.6f * mag);
            r.x += wx * speed * dt; r.y += wy * speed * dt;
            want = L_WALK;
            if (!r.strafing) { r.strafing = true; LOG("animal: player %d steps %s", p + 1, ly < -0.3f ? "back" : "sideways"); }
        } else if (wl > 0.001f) {
            r.strafing = false;
            wx /= wl; wy /= wl;
            // turn towards the wanted heading
            float cur = atan2f(r.hx, r.hy), tgt = atan2f(wx, wy);
            float d = tgt - cur; while (d > 3.14159f) d -= 6.28318f; while (d < -3.14159f) d += 6.28318f;
            float maxTurn = c.animalTurn * dt * (fabsf(d) > 1.0f ? 3.5f : fabsf(d) > 0.4f ? 2.f : 1.f);   // swing round quickly to where you look
            cur += Clamp(d, -maxTurn, maxTurn);
            r.hx = sinf(cur); r.hy = cosf(cur);
            float align = cosf(d); if (align < 0.f) align = 0.f;
            if (fabsf(d) > 1.2f) align = 0.f;   // facing well away from the look direction: turn round on the spot first
            bool running = r.run || pad.runHeld;
            float speed = (running ? c.animalRun : c.animalWalk) * (running ? 1.f : (0.4f + 0.6f * mag)) * (fabsf(d) > 1.2f ? 0.f : 0.25f + 0.75f * align);
            if (BigType(r.type)) speed *= 0.7f;   // ZT19g (owner: mastodon looked like sliding): big animals' strides are slower
            r.x += r.hx * speed * dt; r.y += r.hy * speed * dt;
            want = running ? L_RUN : L_WALK;
        }
    }
    if ((r.x != oldX || r.y != oldY) && !StepAllowed(oldX, oldY, r.x, r.y, r.aquatic && !r.landOK, r.waterTop)) {
        r.x = oldX; r.y = oldY;
        if (now - r.blockLogAt > 2.f) { r.blockLogAt = now; LOG("animal: player %d blocked by a fence/wall or the map edge", p + 1); }
    }
    if (turning && want == L_IDLE) want = L_WALK;   // turning on the spot: walk-in-place looks like stepping round
    if (r.pose != P_STAND || now < r.busyUntil) want = L_NONE;
    // ZT17c: water animals - hold LT = dive (the species' own under-water swim anims), let go = back up to the surface
    {   float g = 0.f, maxD = 0.f;
        // an animal that walks into the water later learns the surface height there (the game floats it at the top)
        static bool s_in[2]; if (r.inWater && !s_in[p] && r.depth <= 0.01f) { r.waterTop = pp[2]; r.aquatic = true; } s_in[p] = r.inWater;
        // ZT19j (owner: on the bottom the animal sometimes sinks into the ground / vanishes in spots): the floor is the
        // HIGHEST of 5 samples (middle, nose, tail, both sides - long animals lie over slopes), a failed sample keeps the
        // last good floor, clearance 1 m (+1 m big animals), and the depth limit eases instead of jumping
        if (r.aquatic && r.inWater) {
            float half = BigType(r.type) ? 4.f : 1.5f, best = -1e9f; int n = 0;
            const float ox[5] = { 0.f, r.hx * half, -r.hx * half,  r.hy * 0.8f, -r.hy * 0.8f };
            const float oy[5] = { 0.f, r.hy * half, -r.hy * half, -r.hx * 0.8f,  r.hx * 0.8f };
            for (int i = 0; i < 5; ++i) { float gz; if (split::TerrainZ(r.x + ox[i], r.y + oy[i], gz)) { if (gz > best) best = gz; ++n; } }
            if (n) r.floorZ = best;
            if (r.floorZ > -9000.f) { g = r.floorZ; maxD = r.waterTop - g - (BigType(r.type) ? 2.f : 1.f); }
        }
        if (maxD < 0.f) maxD = 0.f;
        float old = r.depth;
        // ZT19 (owner): the animal STAYS at the depth where LT is let go (it used to float back up by itself);
        // hold LT = swim down, hold A = swim up (easy for a young kid: two buttons, no combos)
        float vz = 0.f;
        if (pad.lt > 0.5f && !r.noDive) vz = 2.5f; else if (pad.D(PAD_A) && r.inWater) vz = -2.5f;
        r.depth += vz * dt;
        if (vz > 0.f && !r.diveHinted) { r.diveHinted = true; Hint(p, now, "Hold LT = swim down   Hold A = swim up"); }
        if (r.depth > maxD) r.depth = (r.depth - maxD > 0.05f) ? r.depth - Clamp((r.depth - maxD) * 6.f * dt, 0.02f, r.depth - maxD) : maxD;   // ZT19j: ease up
        if (r.depth < 0.f) r.depth = 0.f;
        if ((old > 0.3f) != (r.depth > 0.3f)) { r.loco = L_NONE; LOG("animal: player %d %s", p + 1, r.depth > 0.3f ? "dives" : "surfaces"); }
    }
    float gz;
    if (c.animalGround == 1) { if (split::TerrainZ(r.x, r.y, gz)) r.z = gz; }
    else r.z = pp[2];   // the engine's own ground fit keeps the animal on terrain, paths and plazas
    // ZT19y (owner: the mammoth sinks into the ground after walking a while, its legs splay out sideways - the game's foot
    // fit reaching down to the real ground): the engine's height is only refitted now and then while we move the animal, so
    // on a long walk up a slope the body falls below the terrain. On land it is never let below the terrain (paths and
    // plazas above it keep the engine's height).
    if (c.animalGround != 1 && !r.inWater && split::TerrainZ(r.x, r.y, gz) && gz > r.z + 0.03f) {
        if (gz - r.z > 0.3f && now - r.sinkLogAt > 3.f) { r.sinkLogAt = now; LOG("animal: player %d's %s was %.2f m under the ground - lifted", p + 1, r.type, gz - r.z); }
        r.z = gz; pp[2] = gz;
    }
    GUARDED("animal:move", {
        pp[0] = r.x; pp[1] = r.y; if (c.animalGround == 1) pp[2] = r.z;
        if (r.nodePos && Readable((DWORD)r.nodePos, 12)) { r.nodePos[0] = pp[0]; r.nodePos[1] = pp[1]; r.nodePos[2] = pp[2]; }   // ZT20b

        DWORD a[1] = { node };
        zt_GenericCall(kNodeUpd, 0, 0, a, 1, nullptr);
    });
    if (p == 0) GameCamTarget(r.x, r.y, r.z);   // the game's own camera (and sound listener) stays with player 1's animal
    if ((fabsf(r.hx - r.lastHx) > 0.01f || fabsf(r.hy - r.lastHy) > 0.01f || now - r.faceAt > 0.5f) && want != L_NONE) {
        Face(p, r.hx, r.hy); r.lastHx = r.hx; r.lastHy = r.hy; r.faceAt = now; }
    // locomotion animation: (re)request on change and every loop interval (the request plays the sequence once)
    static bool s_wasWater[2];
    if (s_wasWater[p] != r.inWater) { s_wasWater[p] = r.inWater; r.loco = L_NONE; }
    // ZT20b (owner: the mammoth's walk turned into a standing pose after ~5 s of walking and stayed - one ANIM_REQUEST plays the
    // sequence a limited time, like the lying pose in ZT13): walking / running / swimming re-requests its anim every 3 s
    bool relo = (want == L_WALK || want == L_RUN) && now - r.locoAt > c.walkReloop;   // ZT20c: 4.6 s (3 s showed the loop restart)
    if (want != L_NONE && (want != r.loco || relo || (c.animalLoop > 0.f && now - r.locoAt > c.animalLoop))) {
        static const char* kSubIdle[] = { "SwimSub_Idle", "TreadWaterSub_Idle", "TreadWater_Idle", "Swim_Idle", nullptr };
        static const char* kSubWalk[] = { "SwimSub_Ahead", "SwimSubGlide_Ahead", "Swim_Ahead", nullptr };
        const char* n = r.inWater ? Anim(p, r.depth > 0.3f ? (want == L_IDLE ? kSubIdle : kSubWalk) : (want == L_IDLE ? kSwimIdle : kSwimWalk))
                                  : Anim(p, want == L_RUN ? kRun : want == L_WALK ? kWalk : kIdle);
        if (want != r.loco) LOG("animal: player %d %s -> %s", p + 1, want == L_RUN ? "run" : want == L_WALK ? "walk" : "idle", n ? n : "(none)");
        // ZT19g (owner: flamingo camera jumping up and down): a species with no under-water anims (wading birds...) does not
        // dive - held under the surface while the engine floats it, its height (and the camera) jumped every frame
        if (r.inWater && r.depth > 0.3f && n && !strstr(n, "Sub")) { r.noDive = true; r.depth = 0.f; LOG("animal: player %d's %s cannot dive (no under-water anims)", p + 1, r.type); }
        r.loco = want; r.locoAt = now;
    }
    {   // ZT19g: the camera follows a smoothed height (the engine's ground / water fit moves the animal a little every frame;
        // diving uses our own depth, not the engine's position) - no shaking camera for any animal
        float tgt = (r.aquatic && r.inWater && r.depth > 0.01f) ? r.waterTop - r.depth : r.z;
        if (r.camZ < -9000.f || fabsf(tgt - r.camZ) > 8.f) r.camZ = tgt;
        else r.camZ += (tgt - r.camZ) * Clamp(dt * 3.f, 0.f, 1.f);
    }
    return true;
}
}
namespace animal {
void Draw(IDirect3DDevice9* dev, float now, int W, int H, bool split) {
    for (int p = 0; p < 2; ++p) {
        if (now >= g_hintUntil[p] || !g_hint[p][0]) continue;
        const int fpx = H / 30;
        int x0 = split ? (p ? W / 2 : 0) : 0, x1 = split ? (p ? W : W / 2) : W;
        RECT b = { x0 + W / 40, H / 5, x1 - W / 40, H / 5 + fpx * 2 };
        overlay::Line l = { g_hint[p], b, D3DCOLOR_ARGB(255, 255, 230, 120), D3DCOLOR_ARGB(150, 0, 0, 0) };
        overlay::Panel(dev, b, 0, &l, 1, fpx);
    }
}
}

namespace animal {
// ZT14: when a player selects an animal (pad A / mouse click -> the game's own selection), tell them how to ride it
// ZT19e: beaten animals lie still (death / lie anim, AI off) and are removed 5 s after they lie down
static void DyingTick(float now) {
    static float s_at = 0.f; if (now < s_at) return; s_at = now + 0.5f;
    char code[1200], out[200];
    snprintf(code, sizeof code,
        "if not zt2mod_dying or table.getn(zt2mod_dying)==0 then return '' end local now=%.2f local out='' local keep={} "
        "for _,d in ipairs(zt2mod_dying) do local age=now-d.t "
        " if age>=7 then local ok,err=pcall(function() deleteEntity(d.e) end) "
        "  if not ok then local ok2=pcall(function() d.e:BFG_ENTITY_MORPH_TO_NEW_ENTITY('Carcass_Meat') end) out=out..'carcass '..tostring(ok2)..' ('..tostring(err)..') ' "
        "  else out=out..'removed ' end "
        " else table.insert(keep,d) "
        "  if age>=2.2 and now>=d.a then d.a=now+1.5 pcall(function() d.e:sendMessage('BFAI_ACTIVATE',false) end) "
        "   for _,a in ipairs({'Dead_Idle','Die_Idle','Lie_Idle','Sleep_Idle'}) do local ok,res=pcall(function() return d.e:sendMessage('ANIM_REQUEST',{seqToken=a,scale=1,time=0}) end) "
        "    if ok and res=='ANIM_REQUEST_SUCCESS' then break end end end end end "
        "zt2mod_dying=keep return out", now);
    if (Lua(code, out, sizeof out) && out[0]) { char* nl = strchr(out, '\n'); if (nl) *nl = 0; LOG("animal: beaten animal %s", out); }
}
void SelectionHint(float now) {
    DyingTick(now);
    static float s_at = 0.f; static char s_last[96] = "";
    if (now < s_at) return; s_at = now + 0.15f;
    char out[128];
    if (!Lua("local s=getSelectedEntity and getSelectedEntity() if type(s)~='table' then return '' end "
             "local ty='' pcall(function() ty=s:BFG_GET_BINDER_TYPE() end) return tostring(zt2mod.ptr(s._this))..' '..tostring(ty)", out, sizeof out)) return;
    char* nl = strchr(out, '\n'); if (nl) *nl = 0;
    static int s_who = 0;
    bool changed = strcmp(out, s_last) != 0;
    if (changed) s_who = (now - p2tools::LastSelectAt() < 1.5f) ? 1 : 0;
    // ZT17c: the game puts its info window back at the bottom right (player 2's half) whenever it refreshes it,
    // so it is kept in the selecting player's half every check, not only when the selection changes
    if (split::Active() && out[0]) {
        char code[600], o2[96]; int x = s_who ? 714 : 202;
        snprintf(code, sizeof code,
            "local w=queryObject('UIRoot'):UI_GET_CHILD('Entity Info Layout') if not w then return 'no window' end "
            "local q=w:UI_GET_SCREEN_RECT() if q and q.x==%d then return 'same' end "
            "local ok=pcall(function() w:UI_SET_POS({x=%d,y=q and q.y or 513}) end) return 'moved '..tostring(ok)", x, x);
        if (Lua(code, o2, sizeof o2) && changed) LOG("animal: info window to player %d's half -> %s", s_who + 1, o2);
    }
    if (!changed) return;
    strncpy(s_last, out, sizeof s_last - 1);
    const char* ty = strchr(out, ' '); if (!ty) return; ++ty;
    bool animalType = FindSpecies(ty) != nullptr;   // ZT17: keepers and guests ("Keeper_Adult_F_01") are not animals
    if (!animalType) {
        LOG("animal: player %d selected %s", s_who + 1, ty);
        // ZT19h (owner: a crated animal's buttons - release it / pick it up - must be reachable with the stick): selecting a
        // crate puts that player's highlight into its info window (B leaves it)
        if (strstr(ty, "rate") || strstr(ty, "RATE")) {
            char o3[96]; float x = 0, y = 0, w = 0, h = 0;
            if (Lua("local w=queryObject('UIRoot'):UI_GET_CHILD('Entity Info Layout') if not w then return '' end "
                    "local q=w:UI_GET_SCREEN_RECT() if not q then return '' end return string.format('%.0f %.0f %.0f %.0f',q.x,q.y,q.w,q.h)", o3, sizeof o3)
                && sscanf(o3, "%f %f %f %f", &x, &y, &w, &h) == 4 && w > 0.f) {
                if (s_who == 0 && split::Active()) { x = 202.f; }   // it is moved into the selecting player's half (above)
                else if (s_who == 1 && split::Active()) { x = 714.f; }
                padui::EnterWindow(s_who, now, x, y, w, h);
            } else LOG("animal: crate selected - no info window found (%s)", o3);
        }
        return;
    }
    int p = s_who;
    g_selPtr[p] = (unsigned)strtoul(out, nullptr, 10); g_selTick[p] = GetTickCount();   // ZT19f
    if (Riding(p)) return;
    Hint(p, now, p == 0 ? "Ride this animal: RB + D-pad Up  (keyboard: T)" : "Ride this animal: RB + D-pad Up");
    g_hintUntil[p] = now + 5.f;
    LOG("animal: player %d selected %s - ride hint shown", p + 1, ty);
}
}

namespace animal {
// ZT17c: the half of a player who rides an animal loses the game's UI; both riding -> no UI at all.
// Every panel that lies wholly inside the rider's half is hidden with the game's own UI_HIDE and shown again after.
static float g_keepUntil = 0.f; static bool g_menuBusy = false;
void MenuBusy(bool busy) { g_menuBusy = busy; }
// ZT17d: a player who is NOT riding opens a menu / uses a build tool -> the whole UI comes back right away (the menu is
// shared, and its buttons may sit in the rider's half); it hides again when they are done
void KeepUI(float now) { g_keepUntil = now + 1.0f; RideUI(now); }
void RideUI(float now) {
    static int s_mask = 0; static float s_at = 0.f;
    int mask = (split::Active() ? 0 : 0) | (Riding(0) ? 1 : 0) | (Riding(1) ? 2 : 0);
    if (!split::Active() && mask) mask = 3;            // one screen: the rider gets the whole screen
    if (split::Active() && mask != 3 && (g_menuBusy || now < g_keepUntil)) mask = 0;   // the other player needs the menu
    if (mask == s_mask && (mask == 0 || now < s_at)) return;
    s_at = now + 0.5f;
    char code[1400], out[64];
    snprintf(code, sizeof code,
        "zt2hid=zt2hid or {} local L,R=%d,%d "
        "if %d==1 then for _,c in ipairs(zt2hid) do pcall(function() c:UI_SHOW() end) end zt2hid={} end "
        "if L==0 and R==0 then return 'shown' end local n=0 "
        "local function walk(o,d) local ch=o:UI_GET_CHILDREN() if type(ch)~='table' then return end "
        " for i,v in ipairs(ch) do local c=resolveTable(v.value) if c then "
        "  local ok,q=pcall(function() return c:UI_GET_SCREEN_RECT() end) "
        "  if ok and q and q.w and not c:UI_IS_HIDDEN() then "
        "   local full=q.w>=1000 and q.h>=700 local inL,inR=q.x+q.w<=520,q.x>=504 "
        // ZT19 (crash when player 1 took an animal while player 2 rode): with BOTH riding this also hid every panel that
        // spans both halves (containers no single-rider case ever hid) - now only panels wholly inside a rider's half
        "   if not full and ((inL and L==1) or (inR and R==1)) then c:UI_HIDE() table.insert(zt2hid,c) n=n+1 "
        "   elseif d<5 then walk(c,d+1) end "
        "  end end end end "
        "walk(queryObject('UIRoot'),0) return 'hid '..n",
        mask & 1 ? 1 : 0, mask & 2 ? 2 - 1 : 0, mask != s_mask ? 1 : 0);
    bool ok = Lua(code, out, sizeof out);
    if (mask != s_mask) LOG("animal: riders %d -> UI %s", mask, ok ? out : "lua error");
    s_mask = mask;
}
}
namespace animal { void TestFar(bool on) { g_testFar = on; LOG("animal: test take at any distance %s", on ? "on" : "off"); } }
// ZT18f: player 2's zookeeper body in player 2's own first person. The game's staff are entities like the animals, so
// the riding way is reused: the nearest zookeeper (else any staff member) has its AI switched off (BFAI_ACTIVATE false)
// and stands where player 2 stands (position pointer 0x6c3135 + node rebuild 0x770254), 0.45 m behind the eye so it
// is not in player 2's own view; facing by the game's facing call (a point ahead), walk / idle anims by ANIM_REQUEST.
// Let go = AI back on (the keeper goes back to work).
namespace {
    DWORD g_kEnt = 0; float g_kRetryAt = 0.f, g_kFaceAt = 0.f, g_kYaw = 99.f; int g_kMoving = -1; bool g_kNoneLogged = false;
    void KeeperAnim(bool moving) {
        static const char* kW[] = { "Walk", "Walk_Ahead", "Stand_Walk", "WalkFwd", "Walk_Forward", "Run", nullptr };
        static const char* kI[] = { "Stand_Idle", "Idle", "Stand", "Idle_Stand", nullptr };
        const char* const* l = moving ? kW : kI;
        char code[700]; int k = snprintf(code, sizeof code, "local e=zt2mod_keeper if not e then return '' end for _,n in ipairs({");
        for (int i = 0; l[i]; ++i) k += snprintf(code + k, sizeof code - k, "'%s',", l[i]);
        snprintf(code + k, sizeof code - k, "}) do local ok,r=pcall(function() return e:sendMessage('ANIM_REQUEST',{seqToken=n,scale=1,time=0}) end) "
                 "if ok and r=='ANIM_REQUEST_SUCCESS' then return n end end return ''");
        char out[64] = ""; Lua(code, out, sizeof out);
        static int s_n = 0; if (s_n < 6) { ++s_n; LOG("animal: player 2's zookeeper %s anim '%s'", moving ? "walk" : "idle", out); }
    }
}
namespace animal {
void KeeperRelease() {
    if (!g_kEnt) return;
    Lua("local e=zt2mod_keeper if e then pcall(function() e:sendMessage('BFAI_ACTIVATE',true) end) "
        "pcall(function() e:sendMessage('ANIM_REQUEST',{seqToken='Stand_Idle',scale=1,time=0}) end) end zt2mod_keeper=nil");
    LOG("animal: player 2's zookeeper body let go (back to work)");
    g_kEnt = 0; g_kMoving = -1; g_kYaw = 99.f;
}
void KeeperFrame(float x, float y, float feetZ, float yaw, bool moving, float now) {
    if (!config::Get().keeperBody) return;
    if (!g_kEnt) {
        if (now < g_kRetryAt) return;
        g_kRetryAt = now + 10.f;
        char code[1400], out[160] = "";
        snprintf(code, sizeof code,
            "local ax,ay=%.2f,%.2f local m=queryObject('BFGManager') if not m then return 'none' end "
            "local best,bd,bk,bt=nil,1e18,false,'' local seen={} "
            "for _,kind in ipairs({'keeper','zookeeper','staff'}) do "
            " local ok,r=pcall(function() return m:BFG_GET_ENTITIES(kind) end) "
            " if ok and type(r)=='table' then for i=1,table.getn(r) do local e=resolveTable(r[i].value) "
            "  if e then local id=zt2mod.ptr(e._this) if not seen[id] then seen[id]=1 "
            "   local ty='' pcall(function() ty=e:BFG_GET_BINDER_TYPE() end) if type(ty)~='string' then ty='' end "
            "   local k=string.find(string.lower(ty),'keeper')~=nil local p=e:BFG_GET_ENTITY_POSITION() "
            "   local dx,dy=p.x-ax,p.y-ay local d=dx*dx+dy*dy "
            "   if (k and not bk) or (k==bk and d<bd) then best,bd,bk,bt=e,d,k,ty end end end end end end "
            "if not best then return 'none' end zt2mod_keeper=best "
            "pcall(function() best:sendMessage('BFAI_ACTIVATE',false) end) "
            "local ent,ediag=zt2mod_nat(best) "
            "return string.format('ok %%d %%s',ent or 0,bt)", x, y);
        DefineNat();
        if (!Lua(code, out, sizeof out)) { g_kRetryAt = now + 1.f; return; }   // lua busy: again in a second
        unsigned ent = 0; char ty[96] = "?";
        if (sscanf(out, "ok %u %95s", &ent, ty) < 1 || !ent || !PosPtr(ent) || !Node(ent)) {
            if (!g_kNoneLogged) { g_kNoneLogged = true; LOG("animal: player 2's first person - no zookeeper / staff entity to use as a body (%s)", out); }
            if (ent) Lua("local e=zt2mod_keeper if e then pcall(function() e:sendMessage('BFAI_ACTIVATE',true) end) end zt2mod_keeper=nil");
            return;
        }
        g_kEnt = ent; g_kMoving = -1; g_kYaw = 99.f; g_kNoneLogged = false;
        LOG("animal: player 2's zookeeper body = entity %08x (%s)", ent, ty);
    }
    float* pp = PosPtr(g_kEnt); DWORD node = Node(g_kEnt);
    if (!pp || !node) { LOG("animal: player 2's zookeeper body is gone"); g_kEnt = 0; Lua("zt2mod_keeper=nil"); return; }
    float sy = sinf(yaw), cy = cosf(yaw);
    float bx = x - sy * 0.45f, by = y - cy * 0.45f;
    GUARDED("animal:keeperPos", { pp[0] = bx; pp[1] = by; pp[2] = feetZ; DWORD a[1] = { node }; zt_GenericCall(kNodeUpd, 0, 0, a, 1, nullptr); });
    if (fabsf(yaw - g_kYaw) > 0.05f && now >= g_kFaceAt) {
        g_kYaw = yaw; g_kFaceAt = now + 0.2f;
        char code[200]; snprintf(code, sizeof code, "local e=zt2mod_keeper if e then e:BFG_SET_ENTITY_FACING_DIRECTION({x=%.3f,y=%.3f,z=%.3f}) end",
                                 bx + sy * 10.f, by + cy * 10.f, feetZ);
        Lua(code);
    }
    if ((int)moving != g_kMoving) { g_kMoving = moving; KeeperAnim(moving); }
}
}

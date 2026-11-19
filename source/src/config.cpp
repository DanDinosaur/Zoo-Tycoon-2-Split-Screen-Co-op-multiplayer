#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "config.h"
namespace {
    Config g;
    wchar_t g_dir[MAX_PATH]{}, g_ini[MAX_PATH]{};
    float F(const wchar_t* s, const wchar_t* k, float d) {
        wchar_t b[64]; wchar_t def[64]; swprintf(def, 64, L"%g", d);
        GetPrivateProfileStringW(s, k, def, b, 64, g_ini); return (float)_wtof(b);
    }
    int I(const wchar_t* s, const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(s, k, d, g_ini); }
}
namespace config {
Config& Get() { return g; }
const wchar_t* GameDir() {
    if (!g_dir[0]) { GetModuleFileNameW(nullptr, g_dir, MAX_PATH); wchar_t* s = wcsrchr(g_dir, L'\\'); if (s) s[1] = 0; }
    return g_dir;
}
void Load() {
    swprintf(g_ini, MAX_PATH, L"%lszt2mod.ini", GameDir());
    g.log          = I(L"general", L"log", 1) != 0;
    g.dumpImage    = I(L"general", L"dump_image", 1) != 0;
    g.padEnabled   = I(L"pad", L"enabled", 1) != 0;
    g.deadzone     = F(L"pad", L"deadzone", 0.24f);
    g.trigger      = F(L"pad", L"trigger_threshold", 0.25f);
    g.pointerSpeed = F(L"pad", L"pointer_speed", 400.f);   // ZT18d: owner - half as sensitive
    g.fpLookSpeed = F(L"pad", L"guest_look_speed", 1400.f);
    g.consoleStyle = true;   // ZT19g (owner): every start in the easy aim-circle / highlight-menu style; hold RS switches for this run only
    g.relockMs     = I(L"pad", L"relock_ms", 700);
    g.windowed     = I(L"display", L"windowed", 0) != 0;
    g.splitMode    = I(L"split", L"mode", 1);
    g.marker       = I(L"marker", L"enabled", 1) != 0;
    g.markerSize   = F(L"marker", L"size", 0.11f);
    g.animalWalk   = F(L"animal", L"walk_speed", 3.5f);
    g.animalRun    = F(L"animal", L"run_speed", 9.f);
    g.animalTurn   = F(L"animal", L"turn_rate", 3.2f);
    g.animalLoop   = F(L"animal", L"loop_interval", 0.f);
    g.walkReloop   = F(L"animal", L"walk_reloop", 4.6f);   // ZT20c: walk / run anim re-requested after this many seconds
    g.animalGround = I(L"animal", L"ground", 0);
    g.animalStrafe = I(L"animal", L"strafe_back", 1);
    g.maxZoom      = F(L"camera", L"max_zoom", 110.f);
    g.animalNodeRotate = (int)F(L"animal", L"node_rotate", 0.f);   // ZT17d: off - the game facing call works now
    g.attackDamage = F(L"animal", L"attack_damage", 25.f);
    g.defendDamage = F(L"animal", L"defend_damage", 15.f);
    g.damageCap    = F(L"animal", L"damage_cap", 90.f);
    g.attackReach  = F(L"animal", L"attack_reach", 6.f);
    g.attackMargin = F(L"animal", L"attack_margin", 0.6f);   // ZT18d: ~2 ft extra around the target
    g.biteHitsSmall = I(L"animal", L"bite_hits", 5);
    g.biteHitsBig   = I(L"animal", L"bite_hits_big", 10);
    { wchar_t w[1024]; GetPrivateProfileStringW(L"animal", L"big_animals",
        L"Elephant,Rhino,Hippo,Giraffe,Whale,Orca,Shark,Bison,Buffalo,Mammoth,Mastodon,Moose,Bear,Tyrannosaurus,Spinosaurus,"
        L"Brachiosaurus,Apatosaurus,Argentinosaurus,Diplodocus,Camarasaurus,Triceratops,Stegosaurus,Ankylosaurus,Allosaurus,"
        L"Carcharodontosaurus,Giganotosaurus,Acrocanthosaurus,Albertosaurus,Iguanodon,Parasaurolophus,Edmontosaurus,Deinosuchus,"
        L"Sarcosuchus,Megalodon,Mosasaurus,Liopleurodon,Kronosaurus,Shonisaurus,Indricotherium,Paraceratherium,Megatherium,Elasmotherium",
        w, 1024, g_ini);
      WideCharToMultiByte(CP_ACP, 0, w, -1, g.bigAnimals, sizeof g.bigAnimals, nullptr, nullptr); }
    g.fenceBreak   = I(L"animal", L"fence_break", 1);
    g.fenceHits    = I(L"animal", L"fence_hits", 5);
    g.fenceMaxStrength = F(L"animal", L"fence_max_strength", 9.f);
    g.sky          = I(L"split", L"sky", 1);
    g.keeperBody   = I(L"split", L"keeper_body", 1);
    g.cullUnion    = I(L"split", L"cull_widen", 1);   // ZT17b: on again - the update hides objects outside player 1's view (owner: P2 half loses trees, fences, animals)
}
void SaveStyle() { WritePrivateProfileStringW(L"pad", L"console_style", g.consoleStyle ? L"1" : L"0", g_ini); }
}

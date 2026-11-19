#include <windows.h>
#include <d3d9.h>
#include <cwchar>
#include "mod.h"
#include "config.h"
#include "log.h"
#include "d3dhook.h"
#include "padinput.h"
#include "animal.h"
#include "overlay.h"
#include "dump.h"
#include "xinput_pad.h"
#include "guard.h"
#include "luabridge.h"
#include "render.h"
#include "split.h"
#include "p2tools.h"
#include "privtool.h"
#include "kbm2.h"
#include "dbgwatch.h"
#include "sndwatch.h"
#include <cstdio>
#include <cstring>

static int g_dragStep = 0, g_dragX0, g_dragY0, g_dragX1, g_dragY1;
#define ZT2MOD_BUILD "ZT20e"
namespace padinput { const char* RunningBuild() { return ZT2MOD_BUILD; } }

namespace {
    bool g_started = false;
    LARGE_INTEGER g_freq, g_t0, g_last;
    unsigned g_frames = 0;
    bool g_dumped = false;
    bool g_probed = false;
    int  g_splitForce = -1;   // test override from flag files
    bool g_inZoo = false;     // a zoo is loaded (game mode name from ZTMainMode, checked twice a second)
    char g_modeName[256];
    char g_luaOut[65536];
    // Lua REPL for testing: <gamedir>\zt2mod_lua.txt is run on the game thread, output goes to zt2mod_lua_out.txt
    void LuaFile() {
        wchar_t in[MAX_PATH], outp[MAX_PATH];
        swprintf(in, MAX_PATH, L"%lszt2mod_lua.txt", config::GameDir());
        swprintf(outp, MAX_PATH, L"%lszt2mod_lua_out.txt", config::GameDir());
        if (GetFileAttributesW(in) == INVALID_FILE_ATTRIBUTES || !luabridge::Ready() || !luabridge::Idle()) return;
        FILE* f = _wfopen(in, L"rb"); if (!f) return;
        static char code[65536]; size_t n = fread(code, 1, sizeof code - 1, f); fclose(f); code[n] = 0;
        DeleteFileW(in);
        bool ok = luabridge::Run(code, g_luaOut, sizeof g_luaOut);
        FILE* o = _wfopen(outp, L"wb");
        if (o) { fprintf(o, "%s\n%s", ok ? "OK" : "ERROR", g_luaOut); fclose(o); }
        LOG("lua: ran zt2mod_lua.txt (%u bytes) -> %s: %.700s", (unsigned)n, ok ? "ok" : "error", g_luaOut);
    }
    bool Flag(const wchar_t* name) {   // consume <gamedir>\name if present
        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls%ls", config::GameDir(), name);
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return false;
        DeleteFileW(p); return true;
    }
}

namespace mod {
static bool g_off = false;   // ZT20e: unsupported zt.exe
// ZT19k: watchdog - the game thread stopped presenting for 5 s (a hang, not a crash): log where it is, once per stall
static volatile LONG g_lastFrameTick = 0; static DWORD g_gameTid = 0;
static DWORD WINAPI Watchdog(LPVOID) {
    bool logged = false;
    for (;;) {
        Sleep(1000);
        LONG lf = g_lastFrameTick; if (!lf || !g_gameTid) continue;
        DWORD age = GetTickCount() - (DWORD)lf;
        if (age < 5000) { if (logged) LOG("watchdog: frames again"); logged = false; continue; }
        if (logged) continue; logged = true;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_gameTid);
        if (!t) { LOG("watchdog: no frame for %lu ms (game thread not open)", age); continue; }
        CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        char st[300] = ""; int k = 0;
        if (SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &c)) {
                DWORD* sp = (DWORD*)c.Esp; MEMORY_BASIC_INFORMATION mb{};
                if (VirtualQuery(sp, &mb, sizeof mb) && mb.State == MEM_COMMIT)
                    for (int i = 0; i < 400 && k < 270 && (BYTE*)(sp + i + 1) <= (BYTE*)mb.BaseAddress + mb.RegionSize; ++i) {
                        DWORD v = sp[i]; if (v >= 0x401000 && v < 0xa92000) k += wsprintfA(st + k, " %08lx", v); }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        HMODULE m = nullptr; char mn[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Eip, &m) && m) GetModuleFileNameA(m, mn, MAX_PATH);
        LOG("watchdog: NO FRAME for %lu ms - game thread at %08lx (%s) stack code refs:%s", age, c.Eip, strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn, st);
    }
}
void Start() {
    if (g_started) return; g_started = true;
    config::Load();
    modlog::Init(config::Get().log);
    LOG("build " ZT2MOD_BUILD "  game dir %ls  thread %lu", config::GameDir(), GetCurrentThreadId());
    {   // ZT20e (public release): the mod patches fixed code addresses of ONE zt.exe (33.5.0.2, PE time stamp 48a59624). Any
        // other exe: the mod stays off (plain d3d9 pass-through) instead of crashing the game.
        BYTE* b = (BYTE*)GetModuleHandleW(nullptr); DWORD ts = 0;
        if (b) ts = *(DWORD*)(b + *(DWORD*)(b + 0x3c) + 8);
        if (ts != 0x48a59624) { g_off = true; LOG("zt.exe time stamp %08lx is not the supported 33.5.0.2 (48a59624) - the mod stays OFF", ts); return; }
    }
    guard::Install();
    luabridge::PatchLibTables();
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0); g_last = g_t0;
}
void OnD3DCreated(IDirect3D9* d3d) {
    if (g_off) { static bool s_told = false; if (!s_told) { s_told = true;
        MessageBoxW(nullptr, L"This Zoo Tycoon 2 version (zt.exe) is not supported by the split-screen co-op mod.\n\n"
                             L"Supported: zt.exe 33.5.0.2 (Zoo Tycoon 2 Ultimate Collection / Complete Edition with all expansions).\n"
                             L"The game will start normally without the mod.", L"ZT2 co-op mod", MB_OK | MB_ICONINFORMATION); }
        return; }
    d3dhook::HookD3D(d3d);
}
void OnDeviceLost() { overlay::OnLost(); }

static wchar_t g_shotPath[MAX_PATH]; static bool g_shotReq = false;
void OnFrame(IDirect3DDevice9* dev, void* hwnd) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    float dt = (float)(t.QuadPart - g_last.QuadPart) / g_freq.QuadPart; g_last = t;
    if (dt > 0.1f) dt = 0.1f;
    float now = (float)(t.QuadPart - g_t0.QuadPart) / g_freq.QuadPart;
    ++g_frames;
    InterlockedExchange(&g_lastFrameTick, (LONG)GetTickCount());
    if (g_frames == 1) { LOG("first frame (game thread %lu)", GetCurrentThreadId()); render::Install();
        g_gameTid = GetCurrentThreadId(); HANDLE w = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr); if (w) CloseHandle(w); }
    if (g_frames == 60) { sndwatch::Install(); privtool::Install(); }
    if (g_frames % 300 == 0) guard::Rearm();   // ZT19
    if (g_frames % 6 == 0) virtualpad::Refresh();
    if (g_frames % 15 == 0) {
        if (Flag(L"zt2mod_trace.flag")) d3dhook::RequestTrace();
        if (Flag(L"zt2mod_shot.flag")) {
            swprintf(g_shotPath, MAX_PATH, L"%lszt2mod_shot.jpg", config::GameDir()); g_shotReq = true;   // taken after our overlays
        }
        if (Flag(L"zt2mod_dump.flag")) dump::Image("requested");
        if (Flag(L"zt2mod_split_on.flag")) g_splitForce = 1;
        if (Flag(L"zt2mod_split_off.flag")) g_splitForce = 0;
        dbgwatch::Tick((float)GetTickCount() / 1000.f);
        if (Flag(L"zt2mod_swap.flag")) split::ToggleP2First();
        if (Flag(L"zt2mod_wide.flag")) split::ToggleWide();
        if (Flag(L"zt2mod_watch.flag") && split::MainCam()) dbgwatch::Arm((BYTE*)split::MainCam() + 0x124, (float)GetTickCount() / 1000.f);
        // ZT11 unattended testing: zt2mod_auto.txt = lines "seconds x y [R]" | "seconds watch" | "seconds shot" | "seconds lua <file>",
        // timed from DLL load, read once. MUST BE EMPTY when handing over.
        {
            if (g_dragStep > 0 && hwnd) {
                auto At = [&](int x, int y) { POINT pt{ x, y }; ClientToScreen((HWND)hwnd, &pt); SetCursorPos(pt.x, pt.y); };
                auto Btn = [&](DWORD f) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = f; SendInput(1, &in, sizeof in); };
                int s = g_dragStep++;
                if (s == 1) At(g_dragX0, g_dragY0);
                else if (s == 6) Btn(MOUSEEVENTF_LEFTDOWN);
                else if (s > 6 && s <= 46) { float t = (s - 6) / 40.f; At(g_dragX0 + (int)((g_dragX1 - g_dragX0) * t), g_dragY0 + (int)((g_dragY1 - g_dragY0) * t)); }
                else if (s == 50) { Btn(MOUSEEVENTF_LEFTUP); g_dragStep = 0; }
            }
            struct Ev { float t; int x, y; char kind; char r; char txt[1500]; };
            static Ev s_ev[64]; static int s_nev = -1, s_iev = 0;
            static int s_ax, s_ay, s_aWait = 0; static bool s_ar;
            static FILETIME s_aft{}; static float s_base = 0.f;
            {   // re-read when the file changes during the session: times then count from the moment it is read
                wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lszt2mod_auto.txt", config::GameDir());
                WIN32_FILE_ATTRIBUTE_DATA a;
                if (GetFileAttributesExW(p, GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &s_aft)) {
                    bool first = s_aft.dwLowDateTime == 0 && s_aft.dwHighDateTime == 0;
                    s_aft = a.ftLastWriteTime;
                    if (!first && a.nFileSizeLow) { s_nev = -1; s_iev = 0; s_base = now; }
                }
            }
            if (s_nev < 0) {
                s_nev = 0;
                wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lszt2mod_auto.txt", config::GameDir());
                if (FILE* f = _wfopen(p, L"rb")) {
                    char line[1600];
                    while (s_nev < 64 && fgets(line, sizeof line, f)) {
                        Ev e{}; char w[32] = {};
                        if (sscanf(line, "%f %31s", &e.t, w) < 2) continue;
                        float tt = e.t + s_base;
                        if (!strcmp(w, "watch")) e.kind = 'w'; else if (!strcmp(w, "shot")) e.kind = 's';
                        else if (!strcmp(w, "clear")) e.kind = 'x';
                        else if (!strcmp(w, "far")) e.kind = 'F';
                        else if (!strcmp(w, "p1pick")) { e.kind = 'P'; sscanf(line, "%*f %*s %399s", e.txt); }
                        else if (!strcmp(w, "p1drag")) { e.kind = 'D'; int a, b; if (sscanf(line, "%*f %*s %d %d %d %d", &e.x, &e.y, &a, &b) < 4) continue; snprintf(e.txt, sizeof e.txt, "%d %d", a, b); }
                        else if (!strcmp(w, "p1click")) { e.kind = 'R'; if (sscanf(line, "%*f %*s %d %d", &e.x, &e.y) < 2) continue; }
                        else if (!strcmp(w, "lua")) {   // rest of the line = Lua code for the REPL file
                            e.kind = 'L'; const char* r = strstr(line, "lua") + 3; while (*r == ' ') ++r;
                            strncpy(e.txt, r, sizeof e.txt - 1);
                        }
                        else if (!strcmp(w, "pad1") || !strcmp(w, "pad2")) {
                            e.kind = w[3] == '1' ? '1' : '2';
                            const char* r = strstr(line, w) + 4; while (*r == ' ') ++r;
                            strncpy(e.txt, r, sizeof e.txt - 1);
                        }
                        else { e.kind = 'c'; char r = 0; if (sscanf(line, "%f %d %d %c", &e.t, &e.x, &e.y, &r) < 3) continue; e.r = r; }
                        e.t = tt; s_ev[s_nev++] = e;
                    }
                    fclose(f);
                    if (s_nev) LOG("auto: %d test events", s_nev);
                }
            }
            if (s_aWait > 0 && --s_aWait == 0 && hwnd) {
                HWND h = (HWND)hwnd;
                PostMessageW(h, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(s_ax, s_ay));
                PostMessageW(h, s_ar ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, (s_ar ? MK_RBUTTON : MK_LBUTTON) | kbm2::kOurs, MAKELPARAM(s_ax, s_ay));
                PostMessageW(h, s_ar ? WM_RBUTTONUP : WM_LBUTTONUP, kbm2::kOurs, MAKELPARAM(s_ax, s_ay));
                LOG("auto: click at %d,%d", s_ax, s_ay);
            }
            while (s_iev < s_nev && now >= s_ev[s_iev].t && s_aWait == 0) {
                Ev& e = s_ev[s_iev++];
                if (e.kind == 'L') {
                    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lszt2mod_lua.txt", config::GameDir());
                    if (FILE* f = _wfopen(p, L"wb")) { fputs(e.txt, f); fclose(f); }
                    LOG("auto: lua %.60s", e.txt);
                }
                else if (e.kind == '1' || e.kind == '2' || e.kind == 'x') {
                    const wchar_t* names[4] = { L"zt2mod_pad.txt", L"zt2mod_pad2.txt", L"zt2mod_kbm2.txt", L"zt2mod_click.txt" };
                    for (int i = 0; i < 4; ++i) {
                        if (e.kind == '1' && i != 0) continue; if (e.kind == '2' && i != 1) continue;
                        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls%ls", config::GameDir(), names[i]);
                        if (FILE* f = _wfopen(p, L"wb")) { if (e.kind != 'x') fputs(e.txt, f); fclose(f); }
                    }
                    LOG("auto: %s %s", e.kind == 'x' ? "test files emptied" : (e.kind == '1' ? "pad1" : "pad2"), e.txt);
                }
                else if (e.kind == 'F') animal::TestFar(true);
                else if (e.kind == 'P') { p2tools::NoteP1Pick(e.txt, now); LOG("auto: player 1 picks '%s'", e.txt); }
                else if (e.kind == 'D' && hwnd) {   // REAL drag: press at x,y, glide to x2,y2 over 40 frames, release
                    g_dragX0 = e.x; g_dragY0 = e.y; sscanf(e.txt, "%d %d", &g_dragX1, &g_dragY1); g_dragStep = 1;
                    LOG("auto: real drag %d,%d -> %d,%d", e.x, e.y, g_dragX1, g_dragY1);
                }
                else if (e.kind == 'R' && hwnd) {   // a REAL click (SendInput) like player 1's mouse
                    POINT pt{ e.x, e.y }; ClientToScreen((HWND)hwnd, &pt); SetCursorPos(pt.x, pt.y);
                    INPUT in[2]{}; in[0].type = in[1].type = INPUT_MOUSE;
                    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(2, in, sizeof(INPUT)); LOG("auto: real click at %d,%d", e.x, e.y);
                }
                else if (e.kind == 'w') { if (split::MainCam()) dbgwatch::Arm((BYTE*)split::MainCam() + 0x124, (float)GetTickCount() / 1000.f); }
                else if (e.kind == 's') { static int s_shotN = 0; swprintf(g_shotPath, MAX_PATH, L"%lszt2mod_shot%d.jpg", config::GameDir(), ++s_shotN);
                                          g_shotReq = true; }
                else if (hwnd) { PostMessageW((HWND)hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(e.x, e.y));
                                 s_ax = e.x; s_ay = e.y; s_ar = e.r == 'R' || e.r == 'r'; s_aWait = 8; break; }
            }
        }
        // ZT08 unattended testing: zt2mod_click.txt = "x y" (client pixels, add R for a right click) -> one posted click
        {
            static FILETIME s_ct{};
            static int s_cx, s_cy, s_cWait = 0; static bool s_cr;
            if (s_cWait > 0 && --s_cWait == 0 && hwnd) {
                HWND h = (HWND)hwnd;
                PostMessageW(h, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(s_cx, s_cy));
                PostMessageW(h, s_cr ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, (s_cr ? MK_RBUTTON : MK_LBUTTON) | kbm2::kOurs, MAKELPARAM(s_cx, s_cy));
                PostMessageW(h, s_cr ? WM_RBUTTONUP : WM_LBUTTONUP, kbm2::kOurs, MAKELPARAM(s_cx, s_cy));
                LOG("test click %s at %d,%d", s_cr ? "right" : "left", s_cx, s_cy);
            }
            wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lszt2mod_click.txt", config::GameDir());
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (hwnd && GetFileAttributesExW(p, GetFileExInfoStandard, &a) && a.nFileSizeLow && CompareFileTime(&a.ftLastWriteTime, &s_ct)) {
                s_ct = a.ftLastWriteTime;
                if (FILE* f = _wfopen(p, L"rb")) {
                    char b[64] = {}; fread(b, 1, sizeof b - 1, f); fclose(f);
                    int x = 0, y = 0; char r = 0;
                    if (sscanf(b, "%d %d %c", &x, &y, &r) >= 2) {
                        // move now, press a few frames later (the UI updates its hovered button per frame)
                        PostMessageW((HWND)hwnd, WM_MOUSEMOVE, kbm2::kOurs, MAKELPARAM(x, y));
                        s_cx = x; s_cy = y; s_cr = r == 'R' || r == 'r'; s_cWait = 8;
                    }
                }
            }
        }
    }
    if (g_frames % 10 == 0) LuaFile();
    if (!g_probed && luabridge::Ready() && luabridge::Idle()) {
        g_probed = true;
        bool ok = luabridge::Run(
            "local n = {} for k, v in pairs(_G) do table.insert(n, k .. ':' .. type(v)) end table.sort(n) "
            "return 'globals: ' .. table.concat(n, ' ')", g_luaOut, sizeof g_luaOut);
        LOG("lua: bridge ready (%s): %.6000s", ok ? "ok" : "error", g_luaOut);
    }
    if (!g_dumped && config::Get().dumpImage && now > 20.f) { g_dumped = true; dump::Image("auto"); }
    if (g_frames % 30 == 0 && luabridge::Ready() && luabridge::Idle()) {
        char buf[256];
        if (luabridge::Run("local m = queryObject('ZTMainMode') if m then return m:ZT_GET_CURRENT_MODE_NAME() end", buf, sizeof buf)) {
            char* nl = strchr(buf, '\n'); if (nl) *nl = 0;
            bool menu = !buf[0] || !strcmp(buf, "nil") || !strcmp(buf, "mode_main") || !strcmp(buf, "mode_mainload") ||
                        !strcmp(buf, "mode_load") || !strcmp(buf, "mode_options") || !strcmp(buf, "mode_download");
            if (strcmp(buf, g_modeName)) { LOG("game mode: %s", buf); strncpy(g_modeName, buf, sizeof g_modeName - 1); }
            g_inZoo = !menu;
            p2tools::SetModeName(buf);
        }
    }
    {   // split-screen on while two pads are connected (owner rule), or forced by ini / test flag
        int mode = config::Get().splitMode;
        bool want = mode == 2 || (mode == 1 && ((padslots::SlotOf(0) >= 0 && padslots::SlotOf(1) >= 0) || kbm2::Active()));
        if (g_splitForce >= 0) want = g_splitForce == 1;
        split::SetActive(want && g_inZoo);
    }
    padinput::Frame(dev, (HWND)hwnd, dt, now);
    if (g_shotReq) { g_shotReq = false; LOG("screenshot -> %d", overlay::Screenshot(dev, g_shotPath) ? 1 : 0); }   // includes the mod's overlays
}
}

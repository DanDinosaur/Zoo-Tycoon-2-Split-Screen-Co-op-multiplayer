#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <string>
#include "kbm2.h"
#include "xinput_pad.h"
#include "config.h"
#include "log.h"
#include "split.h"
#include "p2tools.h"
#include "privtool.h"
#include "padui.h"
#include "animal.h"
#include "overlay.h"
#include "padinput.h"

namespace {
    WNDPROC g_orig = nullptr;
    HWND g_hwnd = nullptr;
    HANDLE g_kb = nullptr, g_ms = nullptr;          // player 2's devices
    std::wstring g_kbName, g_msName;
    bool g_waitMouse = false; float g_waitUntil = 0.f;
    bool g_lastKbP2 = false, g_lastMsP2 = false;
    POINT g_p1Pos{ -1, -1 };                          // where player 1's (the OS) pointer belongs
    // player 2's input state (raw)
    volatile bool g_key[256]{}, g_p1key[256]{};
    float g_dx = 0, g_dy = 0; int g_wheel = 0;
    bool g_lmb = false, g_rmb = false, g_mmb = false;
    // per-frame copies
    bool g_k[256]{}, g_kPrev[256]{};
    bool g_l = false, g_lPrev = false, g_r = false, g_rPrev = false;
    float g_px = -1, g_py = -1;                       // player 2's pointer (client px)
    bool g_leftInWorld = false;                       // the held left button started in the world (drag = A held)
    char g_msg[160] = ""; float g_msgUntil = 0.f;
    std::wstring g_ini;
    float g_now = 0.f;

    // keys player 2's camera owns (kept away from the game); everything else on player 2's keyboard goes to the game
    bool CamKey(UINT vk) {
        switch (vk) { case 'W': case 'A': case 'S': case 'D': case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                      case VK_ADD: case VK_SUBTRACT: case VK_OEM_PLUS: case VK_OEM_MINUS: case VK_F9:
                      case 'T': case 'I': case 'J': case 'K': case 'L':
                      case 'G': case 'C': return !g_key[VK_CONTROL]; }   // ZT18f: G / C = player 2's own first person / camera (vanilla hotkeys; Ctrl+C = Construction still goes to the game)
        return false;
    }
    void Msg(const char* s) { snprintf(g_msg, sizeof g_msg, "%s", s); g_msgUntil = g_now + 4.f; LOG("kbm2: %s", s); }

    std::wstring DevName(HANDLE h) {
        wchar_t b[512]; UINT n = 512;
        if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, b, &n) == (UINT)-1) return L"";
        return b;
    }
    HANDLE FindDev(const std::wstring& name) {
        if (name.empty()) return nullptr;
        UINT n = 0; GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
        if (!n) return nullptr;
        RAWINPUTDEVICELIST* l = new RAWINPUTDEVICELIST[n];
        HANDLE r = nullptr;
        if (GetRawInputDeviceList(l, &n, sizeof(RAWINPUTDEVICELIST)) != (UINT)-1)
            for (UINT i = 0; i < n; ++i) if (DevName(l[i].hDevice) == name) { r = l[i].hDevice; break; }
        delete[] l; return r;
    }
    void Save() {
        WritePrivateProfileStringW(L"kbm", L"p2_keyboard", g_kbName.c_str(), g_ini.c_str());
        WritePrivateProfileStringW(L"kbm", L"p2_mouse", g_msName.c_str(), g_ini.c_str());
    }
    void LogDevices() {
        UINT n = 0; GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
        RAWINPUTDEVICELIST* l = new RAWINPUTDEVICELIST[n ? n : 1];
        if (n && GetRawInputDeviceList(l, &n, sizeof(RAWINPUTDEVICELIST)) != (UINT)-1)
            for (UINT i = 0; i < n; ++i) if (l[i].dwType != RIM_TYPEHID)
                LOG("kbm2: device %p %s %ls", l[i].hDevice, l[i].dwType == RIM_TYPEMOUSE ? "mouse" : "keyboard", DevName(l[i].hDevice).c_str());
        delete[] l;
    }

    void OnRaw(HRAWINPUT h) {
        BYTE buf[256]; UINT n = sizeof buf;
        if (GetRawInputData(h, RID_INPUT, buf, &n, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
        RAWINPUT* ri = (RAWINPUT*)buf;
        HANDLE dev = ri->header.hDevice;
        if (ri->header.dwType == RIM_TYPEKEYBOARD) {
            UINT vk = ri->data.keyboard.VKey; bool up = (ri->data.keyboard.Flags & RI_KEY_BREAK) != 0;
            if (vk >= 256) return;
            bool p2 = dev && dev == g_kb;
            g_lastKbP2 = p2;
            if (vk == VK_F9 && !up && dev) {
                if (p2) { g_kb = g_ms = nullptr; g_kbName.clear(); g_msName.clear(); Save(); g_lastKbP2 = false;
                          Msg("Player 2: back to the controller"); return; }
                g_kb = dev; g_kbName = DevName(dev); g_waitMouse = true; g_waitUntil = g_now + 15.f; g_lastKbP2 = true;
                Msg("Player 2: keyboard set - now click with player 2's mouse");
                return;
            }
            (p2 ? g_key : g_p1key)[vk] = !up;
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) { /* generic modifiers: VKey already generic */ }
        } else if (ri->header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE& m = ri->data.mouse;
            USHORT bf = m.usButtonFlags;
            if (g_waitMouse && dev && (bf & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN))) {
                g_ms = dev; g_msName = DevName(dev); g_waitMouse = false; Save();
                Msg("Player 2: keyboard + mouse ready (F9 on player 2's keyboard = back to the controller)");
                g_lastMsP2 = true; return;
            }
            bool p2 = dev && dev == g_ms;
            g_lastMsP2 = p2;
            if (!p2) return;
            if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) { g_dx += (float)m.lLastX; g_dy += (float)m.lLastY; }
            if (bf & RI_MOUSE_LEFT_BUTTON_DOWN) g_lmb = true;   if (bf & RI_MOUSE_LEFT_BUTTON_UP) g_lmb = false;
            if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN) g_rmb = true;  if (bf & RI_MOUSE_RIGHT_BUTTON_UP) g_rmb = false;
            if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) g_mmb = true; if (bf & RI_MOUSE_MIDDLE_BUTTON_UP) g_mmb = false;
            if (bf & RI_MOUSE_WHEEL) g_wheel += (short)m.usButtonData;
        }
    }

    LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
        case WM_INPUT:
            OnRaw((HRAWINPUT)l);
            break;
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if ((g_kb && g_lastKbP2 && CamKey((UINT)w)) || (w == VK_F9 && g_lastKbP2)) return 0;   // player 2's camera keys
            break;
        case WM_CHAR:
            if (g_kb && g_lastKbP2) { WPARAM up = w >= 'a' && w <= 'z' ? w - 32 : w; if (CamKey((UINT)up)) return 0; }
            break;
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
            if (w & kbm2::kOurs) { bool p2 = (w & kbm2::kP2) != 0; w &= ~(kbm2::kOurs | kbm2::kP2);
                (void)p2;
                break; }               // our own posted clicks (player 2 / pad) always pass
            if (g_ms && g_lastMsP2) {
                // player 2's mouse moved the shared Windows pointer: put it back where player 1 had it
                if (msg == WM_MOUSEMOVE && g_p1Pos.x >= 0) { POINT c; GetCursorPos(&c);
                    if (c.x != g_p1Pos.x || c.y != g_p1Pos.y) SetCursorPos(g_p1Pos.x, g_p1Pos.y); }
                return 0;
            }
            if (msg == WM_MOUSEMOVE) GetCursorPos(&g_p1Pos);
            // ZT15: player 1's world click waits while their own build tool goes back in (player 2 had the game's tool)
            if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) && p2tools::P1Mouse(msg == WM_LBUTTONDOWN, (short)LOWORD(l), (short)HIWORD(l))) return 0;
            break;
        }
        return CallWindowProcW(g_orig, h, msg, w, l);
    }

    // GetAsyncKeyState / GetKeyState: player 2's camera keys are invisible to the game
    typedef SHORT (WINAPI* KS_t)(int);
    KS_t g_gaks = nullptr, g_gks = nullptr;
    SHORT WINAPI hGAKS(int vk) { SHORT r = g_gaks(vk); if (vk > 0 && vk < 256 && g_kb && g_key[vk] && !g_p1key[vk] && CamKey(vk)) r = 0; return r; }
    SHORT WINAPI hGKS(int vk)  { SHORT r = g_gks(vk);  if (vk > 0 && vk < 256 && g_kb && g_key[vk] && !g_p1key[vk] && CamKey(vk)) r &= 1; return r; }
    bool PatchApi(const char* name, void* fn, void** orig) {
        BYTE* p = (BYTE*)GetProcAddress(GetModuleHandleA("user32.dll"), name);
        if (!p) return false;
        // 8B FF 55 8B EC = mov edi,edi / push ebp / mov ebp,esp (hot-patchable prologue)
        if (!(p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 && p[3] == 0x8B && p[4] == 0xEC)) {
            LOG("kbm2: %s prologue %02x %02x %02x %02x %02x - not hooked", name, p[0], p[1], p[2], p[3], p[4]); return false; }
        BYTE* t = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, p, 5); t[5] = 0xE9; *(int*)(t + 6) = (int)((p + 5) - (t + 10));
        DWORD op; VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &op);
        p[0] = 0xE9; *(int*)(p + 1) = (int)((BYTE*)fn - (p + 5));
        VirtualProtect(p, 5, op, &op); FlushInstructionCache(GetCurrentProcess(), p, 5);
        *orig = t; LOG("kbm2: %s hooked", name); return true;
    }

    // ---- virtual player 2 keyboard + mouse (test file) ----
    struct VState { bool active = false; bool key[256]{}; bool lmb = false, rmb = false; int x = -1, y = -1; int wheel = 0; DWORD until = 0; };
    VState g_v; FILETIME g_vTime{};
    int KeyByName(const char* t) {
        static const struct { const char* n; int vk; } k[] = { {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
            {"CTRL", VK_CONTROL}, {"SHIFT", VK_SHIFT}, {"ESC", VK_ESCAPE}, {"DEL", VK_DELETE}, {"F9", VK_F9}, {"PLUS", VK_ADD},
            {"MINUS", VK_SUBTRACT}, {"ENTER", VK_RETURN}, {"SPACE", VK_SPACE}, {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD} };
        for (auto& e : k) if (!strcmp(t, e.n)) return e.vk;
        if (strlen(t) == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) return t[0];
        return 0;
    }
    void VirtualRefresh() {
        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lszt2mod_kbm2.txt", config::GameDir());
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a) || (a.nFileSizeLow == 0 && a.nFileSizeHigh == 0)) { g_v.active = false; return; }
        if (!CompareFileTime(&a.ftLastWriteTime, &g_vTime)) return;
        g_vTime = a.ftLastWriteTime;
        FILE* f = _wfopen(p, L"rb"); if (!f) return;
        char b[1024]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
        VState v; v.active = true;
        for (char* t = strtok(b, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) {
            if (!strncmp(t, "X=", 2)) v.x = atoi(t + 2); else if (!strncmp(t, "Y=", 2)) v.y = atoi(t + 2);
            else if (!strncmp(t, "WHEEL=", 6)) v.wheel = atoi(t + 6);
            else if (!strncmp(t, "FOR=", 4)) v.until = GetTickCount() + atoi(t + 4);
            else if (!strcmp(t, "LMB")) v.lmb = true; else if (!strcmp(t, "RMB")) v.rmb = true;
            else if (int vk = KeyByName(t)) v.key[vk] = true;
        }
        if (g_v.active && v.x < 0) { v.x = g_v.x; v.y = g_v.y; }   // the pointer stays where the last file put it
        g_v = v;
        LOG("kbm2: virtual player 2 keyboard+mouse: %s", b);
    }
}

namespace kbm2 {
bool Active() { return (g_kb && g_ms) || g_v.active; }

void Install(HWND hwnd) {
    if (g_orig || !hwnd) return;
    g_hwnd = hwnd;
    wchar_t ini[MAX_PATH]; swprintf(ini, MAX_PATH, L"%lszt2mod.ini", config::GameDir()); g_ini = ini;
    if (!GetPrivateProfileIntW(L"kbm", L"enabled", 1, ini)) { LOG("kbm2: off ([kbm] enabled=0)"); return; }
    RAWINPUTDEVICE rid[2] = { { 0x01, 0x06, 0, hwnd }, { 0x01, 0x02, 0, hwnd } };   // keyboard, mouse -> the game window
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) { LOG("kbm2: RegisterRawInputDevices failed %lu", GetLastError()); return; }
    g_orig = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)Proc);
    LogDevices();
    wchar_t b[512];
    GetPrivateProfileStringW(L"kbm", L"p2_keyboard", L"", b, 512, ini); g_kbName = b;
    GetPrivateProfileStringW(L"kbm", L"p2_mouse", L"", b, 512, ini); g_msName = b;
    g_kb = FindDev(g_kbName); g_ms = FindDev(g_msName);
    if (!g_kb || !g_ms) { g_kb = g_ms = nullptr; }
    PatchApi("GetAsyncKeyState", (void*)hGAKS, (void**)&g_gaks);
    PatchApi("GetKeyState", (void*)hGKS, (void**)&g_gks);
    LOG("kbm2: ready (window proc %p), player 2 keyboard+mouse %s - F9 on a keyboard to join", (void*)g_orig, g_kb ? "remembered" : "not set");
}

bool Frame(IDirect3DDevice9* dev, HWND hwnd, float dt, float now, int W, int H) {
    g_now = now;
    if (!Active()) return false;
    // ---- this frame's input ----
    for (int i = 0; i < 256; ++i) { g_kPrev[i] = g_k[i]; g_k[i] = g_key[i]; }
    g_lPrev = g_l; g_rPrev = g_r;
    float dx = g_dx, dy = g_dy; int wheel = g_wheel; g_dx = g_dy = 0; g_wheel = 0;
    g_l = g_lmb; g_r = g_rmb;
    if (g_v.active) {
        if (g_v.until && (int)(GetTickCount() - g_v.until) >= 0) { int x = g_v.x, y = g_v.y; g_v = VState(); g_v.active = true; g_v.x = x; g_v.y = y; }
        for (int i = 0; i < 256; ++i) g_k[i] = g_v.key[i];
        g_l = g_v.lmb; g_r = g_v.rmb; wheel = g_v.wheel; g_v.wheel = 0;
        if (g_v.x >= 0) { g_px = (float)g_v.x; g_py = (float)g_v.y; }
    }
    auto K = [](int vk) { return g_k[vk]; };
    auto KP = [](int vk) { return g_k[vk] && !g_kPrev[vk]; };
    if (g_px < 0) { g_px = W * 0.75f; g_py = H * 0.5f; }
    // ---- ZT18f: G = player 2's own zookeeper first person, C = player 2's camera (the game's hotkeys, kept for player 2) ----
    if (KP('G') && !K(VK_CONTROL) && !animal::Riding(1)) { padinput::P2PhotoOff(); split::P2FirstPerson(!split::P2InFirstPerson()); }
    if (KP('C') && !K(VK_CONTROL) && !animal::Riding(1)) padinput::P2PhotoToggle(now);
    const bool fp = split::P2InFirstPerson(), photo = fp && padinput::P2Photo();
    bool look = K(VK_CONTROL) || g_mmb || (fp && (g_r || photo));   // first person: right mouse held = look (camera: always)
    if (!look) { g_px += dx; g_py += dy; }
    if (g_px < 0) g_px = 0; if (g_py < 0) g_py = 0; if (g_px > W - 1) g_px = (float)W - 1; if (g_py > H - 1) g_py = (float)H - 1;
    const bool inHalf = g_px >= W * 0.5f;

    // ---- riding: T take / let go, I/J/K/L walk, Shift run, the same keys as player 1 ----
    {
        animal::In in;
        in.ly = (K('I') ? 1.f : 0.f) - (K('K') ? 1.f : 0.f);
        in.lx = (K('L') ? 1.f : 0.f) - (K('J') ? 1.f : 0.f);
        in.runHeld = K(VK_SHIFT);
        in.rx = look ? dx * 0.02f : 0.f;
        if (g_l) in.rt = 1.f;                 // left click = attack while riding
        if (g_r) in.lt = 1.f;                 // right click = special
        float ax = 0, ay = 0; bool aim = split::P2Aim(ax, ay);
        bool keyT = KP('T');
        if (!fp && (animal::Riding(1) || keyT)) {   // ZT18f: no riding start while walking
            bool riding = animal::Frame(1, in, dt, now, aim, ax, ay, keyT);
            if (riding) return true;
        }
    }

    // ---- camera: W/A/S/D pan, arrows turn / tilt, +/- and wheel zoom, Ctrl or middle mouse + move = mouselook, edges ----
    float panX = (K('D') ? 1.f : 0.f) - (K('A') ? 1.f : 0.f), panY = (K('W') ? 1.f : 0.f) - (K('S') ? 1.f : 0.f);
    if (fp) {   // ZT18f: W/S walk, A/D step sideways, Shift run, arrows turn / look, mouse look (right mouse / Ctrl / camera)
        float rx = (K(VK_RIGHT) ? 1.f : 0.f) - (K(VK_LEFT) ? 1.f : 0.f), ry = (K(VK_UP) ? 1.f : 0.f) - (K(VK_DOWN) ? 1.f : 0.f);
        if (look) split::P2FpLook(dx * 0.0035f, dy * 0.0035f);
        split::P2FpInput(photo ? panX * 0.5f : panX, photo ? panY * 0.5f : panY, rx, ry, dt, K(VK_SHIFT) && !photo);
        padinput::P2FpBody(dt, now, panX != 0.f || panY != 0.f);
        if (photo && wheel) padinput::P2PhotoZoom(wheel > 0 ? 1.25f : 0.8f);
        if (photo && g_r && !g_rPrev) padinput::P2PhotoOff();
        panX = panY = 0.f;
    }
    const int edge = H / 120 + 2;
    if (inHalf && !look && !fp) {
        if (g_px >= W - edge) panX = 1.f; if (g_px <= W * 0.5f + edge) panX = -1.f;
        if (g_py <= edge) panY = 1.f; if (g_py >= H - edge) panY = -1.f;
    }
    float rot = (K(VK_RIGHT) ? 1.f : 0.f) - (K(VK_LEFT) ? 1.f : 0.f);
    float tilt = (K(VK_UP) ? 1.f : 0.f) - (K(VK_DOWN) ? 1.f : 0.f);
    float zoom = (K(VK_SUBTRACT) || K(VK_OEM_MINUS) ? 1.f : 0.f) - (K(VK_ADD) || K(VK_OEM_PLUS) ? 1.f : 0.f);
    if (wheel) zoom -= wheel / 120.f * 6.f;      // one notch = a quick zoom step
    if (look) { rot += dx * 0.15f; tilt -= dy * 0.15f; }
    if (!fp) split::P2Input(panX, panY, rot, zoom, dt, tilt);

    // ---- clicks: the game's panels anywhere on the screen, the world in player 2's half ----
    p2tools::SetPointer((int)(inHalf ? g_px : W * 0.75f), (int)(inHalf ? g_py : H * 0.5f));
    bool down[PAD_COUNT]{};
    bool lPress = g_l && !g_lPrev, rPress = g_r && !g_rPrev;
    char name[96]; bool item = false;
    if (lPress) {
        bool ui = padui::OverUI((int)g_px, (int)g_py, W, H, now, name, sizeof name, &item);
        // ZT18f: the zookeeper / camera / overhead buttons give player 2 their OWN first person and camera (the game's
        // modes stay player 1's), like the pad
        if (ui && !animal::Riding(1) && (!strcmp(name, "firstperson") || !strcmp(name, "photo") || (fp && !strcmp(name, "overhead")))) {
            if (!strcmp(name, "photo")) padinput::P2PhotoToggle(now);
            else { padinput::P2PhotoOff(); split::P2FirstPerson(!strcmp(name, "firstperson") ? !split::P2InFirstPerson() : false); }
            g_leftInWorld = false;
            LOG("kbm2: player 2's own '%s' (kept from the game)", name);
        } else if (!ui && photo) { padinput::P2PhotoSnap(now); g_leftInWorld = false; }
        else if (ui) {
            int x = (int)g_px, y = (int)g_py;
            PostMessageW(hwnd, WM_MOUSEMOVE, kOurs, MAKELPARAM(x, y));
            PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON | kOurs, MAKELPARAM(x, y));
            PostMessageW(hwnd, WM_LBUTTONUP, kOurs, MAKELPARAM(x, y));
            if (g_p1Pos.x >= 0) { POINT c = g_p1Pos; ScreenToClient(hwnd, &c); PostMessageW(hwnd, WM_MOUSEMOVE, kOurs, MAKELPARAM(c.x, c.y)); }
            padui::NoteClick(1, name, item, now);
            g_leftInWorld = false;
            LOG("kbm2: player 2 clicks the UI at %d,%d '%s'", x, y, name);
        } else g_leftInWorld = inHalf;
    }
    down[PAD_A] = g_l && g_leftInWorld;
    down[PAD_B] = g_r && !(fp && !p2tools::Busy()); (void)rPress;   // ZT18f: in first person the right mouse looks (B only to stop a tool)
    down[PAD_BACK] = false;                      // Delete goes to the game (vanilla key)
    down[PAD_LB] = K(VK_OEM_COMMA); down[PAD_RB] = K(VK_OEM_PERIOD);
    padui::WatchTick(1, now);
    static XPad s_pad(1);
    s_pad.Inject(down, 0, 0, 0, 0, 0, 0);
    p2tools::Frame(dev, s_pad, dt, now, W, H, hwnd, false);
    return true;
}

void Draw(IDirect3DDevice9* dev, float now, int W, int H) {
    g_now = now;
    static int s_n = 0; if (++s_n % 6 == 0) VirtualRefresh();
    if (g_waitMouse && now > g_waitUntil) { g_waitMouse = false; g_kb = nullptr; g_kbName.clear(); Msg("Player 2: no mouse clicked - press F9 again"); }
    if (now < g_msgUntil) {
        const int fpx = H / 38;
        RECT b = { W / 2 + W / 40, H / 6, W - W / 40, H / 6 + fpx * 2 };
        overlay::Line l = { g_msg, b, D3DCOLOR_ARGB(255, 255, 255, 255), D3DCOLOR_ARGB(170, 0, 0, 0) };
        overlay::Panel(dev, b, 0, &l, 1, fpx);
    }
    if (!Active() || !split::Active() || g_px < 0) return;
    // player 2's own pointer: a cyan arrow-head made of a few blocks, with a P2 tag
    int x = (int)g_px, y = (int)g_py, s = H / 90 + 2;
    for (int i = 0; i < 6; ++i) {
        RECT r = { x, y + i * s / 2, x + (i + 1) * s / 2 + 2, y + (i + 1) * s / 2 };
        overlay::Panel(dev, r, D3DCOLOR_ARGB(255, 60, 220, 255), nullptr, 0, 0);
    }
    RECT o = { x - 1, y - 1, x + 3, y + s * 3 + 1 };
    overlay::Panel(dev, o, D3DCOLOR_ARGB(255, 0, 40, 60), nullptr, 0, 0);
    const int fpx = H / 60;
    RECT tag = { x + s * 2, y + s * 2, x + s * 2 + fpx * 2, y + s * 2 + fpx + 4 };
    overlay::Line l = { "P2", tag, D3DCOLOR_ARGB(255, 20, 20, 20), D3DCOLOR_ARGB(220, 60, 220, 255) };
    overlay::Panel(dev, tag, 0, &l, 1, fpx);
}
}

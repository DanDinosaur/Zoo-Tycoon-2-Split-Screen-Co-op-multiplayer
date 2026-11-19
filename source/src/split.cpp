#include <windows.h>
#include <cmath>
#include <cstring>
#include "split.h"
#include "hook.h"
#include "log.h"
#include "guard.h"
#include "animal.h"
#include "config.h"
#include "d3dhook.h"
#include <d3d9.h>
#include "overlay.h"

// NiCamera layout in zt.exe 33.5.0.2 (verified live ZT02a, REPL dump vs BFG_GET_CAMERA_*):
//   +0x23 byte  world-data-valid flag (0 -> GetWorldTranslate recomputes from local, 0x59fe02)
//   +0x6c float[9] world rotate, row-major, columns = dir, up, right
//   +0x90 float[3] world translate (camera position), +0x9c scale
//   +0x100 dir, +0x10c up, +0x118 right (cached world vectors passed to SetCameraData)
//   +0x124 frustum L R T B N F, +0x13c port L R T B (0..1, reset by the game every frame)
//   ZT world: z up. Overhead camera: pos = target - dir * dist, dir = (sinY cosP, cosY cosP, -sinP),
//   up = (sinY sinP, cosY sinP, cosP), right = (cosY, -sinY, 0).
namespace {
    typedef void (__stdcall* SceneDraw_t)(void* self, void* view, void* a2, void* flags);
    typedef char (__stdcall* ViewBegin_t)(void* view);
    SceneDraw_t oSceneDraw = nullptr;
    ViewBegin_t oViewBegin = nullptr;

    bool  g_on = false;
    int   g_pass = -1;
    BYTE* g_passCam = nullptr;
    float g_port[4];                 // port to force in the current pass
    bool  g_skipBeginFrame = false;
    bool  g_p2First = false;
    float g_wide = 1.f;          // ZT10 test (zt2mod_wide.flag): between frames the camera frustum is widened this much so
    float g_realFr[6]; bool g_haveReal = false; BYTE* g_wideCam = nullptr;   // the update's ground culling covers player 2 too
    // ZT10 union cull view: the game culls the ground in its UPDATE with the main camera (player 1). Between frames the
    // camera becomes a straight-down view above both players' areas with a frustum covering both; the real camera is put
    // back at the start of the draw and around every pick.
    void* volatile g_view = nullptr;
    BYTE* g_retStub = nullptr;     // executable "ret" used as a no-op BeginFrame in the second pass

    BYTE* g_mainCam = nullptr;       // the game's main NiCamera (seen in SceneDraw)
    // ---- picking: NiCamera port point -> frustum point (0x591d17, thiscall ecx=cam, eax=out*, 3 stack args, ret 0xc).
    // Only caller: NiCamera::WindowPointToRay (0x591e85). In split-screen the main camera is player 1's left half,
    // so give it the left-half port + half frustum for the conversion (a point in player 2's half falls outside -> no pick).
    float g_pickSave[10]; bool g_pickSwapped = false;
    struct Pose { float tx, ty, tz, yaw, pitch, dist; bool valid; } g_p2{};
    // ZT18e: player 2's own first person (g_p2 = eye pose, dist ~0) - player 1 keeps the game's mode_super_staff
    bool g_p2Fp = false; Pose g_p2Over{};
    const float kEye = 1.7f, kFpDist = 0.05f, kFpNear = 0.25f;
    // ZT18f: the game's photo mode = player 1 full screen (g_solo); painted sky for our own camera poses (g_skyWant per pass);
    // player 2's photo view zoom + grab request
    bool g_solo = false, g_skyWant = false, g_p2ShotReq = false; float g_p2Zoom = 1.f;
    bool On() { return g_on && !g_solo; }
    void SkyFor(void* view) {
        if (!g_skyWant || !config::Get().sky || !view) return;
        BYTE* cam = *(BYTE**)((BYTE*)view + 0x70); IDirect3DDevice9* d = d3dhook::Device();
        if (!cam || !d) return;
        float* dir = (float*)(cam + 0x100); float* fr = (float*)(cam + 0x124);
        float h = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        GUARDED("split:sky", { overlay::Sky(d, atan2f(-dir[2], h), fr[2], fr[3]); });
    }

    struct CamSave { BYTE flag; float rot[9], pos[3], vec[9], fr[6]; };
    void Save(BYTE* c, CamSave& s) { s.flag = c[0x23]; memcpy(s.rot, c + 0x6c, 36); memcpy(s.pos, c + 0x90, 12); memcpy(s.vec, c + 0x100, 36); memcpy(s.fr, c + 0x124, 24); }
    void Restore(BYTE* c, const CamSave& s) { c[0x23] = s.flag; memcpy(c + 0x6c, s.rot, 36); memcpy(c + 0x90, s.pos, 12); memcpy(c + 0x100, s.vec, 36); memcpy(c + 0x124, s.fr, 24); }
    // ZT10 union cull view: the game culls the ground in its UPDATE with the main camera (player 1), so player 2's half
    // showed black ground. Between frames the camera becomes a straight-down view above both players' areas with a
    // frustum covering both; the real camera is put back at the start of the draw and around every pick.
    CamSave g_cullReal, g_cullTmp; bool g_cullActive = false, g_cullSwapped = false; BYTE* g_cullCam = nullptr;
    float g_cullLogAt = -100.f; float g_cullFr[6];

    // ZT11: the ground (and scene-graph) cull runs INSIDE SceneDraw (0x5031f9 -> 0x72278c -> 0x7244e6) against 6 world
    // planes cached in the NiCamera at +0x184 (mask +0x1e4). They are rebuilt only by 0x5916cd (rot -> dir/up/right,
    // planes from frustum + position, world->camera matrix +0xb4..), which the game calls in its camera update.
    // Writing a pose without rebuilding left player 2 culled with player 1's planes = black ground.
    void RebuildPlanes(BYTE* c) {
        if (!c) return;
        asm volatile("call *%1" : "+c"(c) : "r"((void*)0x5916cd) : "eax", "edx", "memory", "cc");
    }
    void PoseFromCamera(BYTE* c, Pose& p) {
        float* d = (float*)(c + 0x100); float* pos = (float*)(c + 0x90);
        float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
        p.pitch = atan2f(-d[2], h);
        p.yaw = atan2f(d[0], d[1]);
        p.dist = 55.f;
        p.tx = pos[0] + d[0] * p.dist; p.ty = pos[1] + d[1] * p.dist; p.tz = pos[2] + d[2] * p.dist;
        p.valid = true;
        LOG("split: player 2 camera starts at target %.1f %.1f %.1f yaw %.3f pitch %.3f", p.tx, p.ty, p.tz, p.yaw, p.pitch);
    }
    void WritePose(BYTE* c, const Pose& p) {
        float sy = sinf(p.yaw), cy = cosf(p.yaw), sp = sinf(p.pitch), cp = cosf(p.pitch);
        float dir[3] = { sy * cp, cy * cp, -sp }, up[3] = { sy * sp, cy * sp, cp }, rt[3] = { cy, -sy, 0.f };
        float* R = (float*)(c + 0x6c);
        for (int i = 0; i < 3; ++i) { R[i * 3 + 0] = dir[i]; R[i * 3 + 1] = up[i]; R[i * 3 + 2] = rt[i]; }
        float* pos = (float*)(c + 0x90);
        pos[0] = p.tx - dir[0] * p.dist; pos[1] = p.ty - dir[1] * p.dist; pos[2] = p.tz - dir[2] * p.dist;
        memcpy(c + 0x100, dir, 12); memcpy(c + 0x10c, up, 12); memcpy(c + 0x118, rt, 12);
        c[0x23] = 1;   // world data valid: keep ours, do not recompute from local
    }
    void CullEnter(BYTE* c) {
        // widen the frustum (camera position is the game's): the update's ground culling then covers player 2's area
        // unless it lies behind player 1's camera
        float* fr = (float*)(c + 0x124);
        const float k = 12.f;
        fr[0] = g_cullReal.fr[0] * k; fr[1] = g_cullReal.fr[1] * k; fr[2] = g_cullReal.fr[2] * k; fr[3] = g_cullReal.fr[3] * k;
        if (fr[5] < 3000.f) fr[5] = 3000.f;
        memcpy(g_cullFr, fr, 24);
    }

    char __stdcall hViewBegin(void* view) {
        g_view = view;
        if (g_pass >= 0 && view) {
            BYTE* cam = *(BYTE**)((BYTE*)view + 0x70);
            if (cam) memcpy(cam + 0x13c, g_port, 16);
            if (g_skipBeginFrame) {
                // second pass: same as the game's ViewBegin but without the renderer's BeginFrame (already begun)
                void** holder = *(void***)((BYTE*)view + 0x78);
                void** r = holder ? (void**)holder[3] : nullptr;
                if (r && g_retStub) {
                    void** vt = *(void***)r; DWORD op;
                    void* begin = vt[0xbc / 4];
                    VirtualProtect(&vt[0xbc / 4], 4, PAGE_EXECUTE_READWRITE, &op);
                    vt[0xbc / 4] = g_retStub;   // BeginFrame: thiscall, no args, result unused by ViewBegin
                    char ok = oViewBegin(view);
                    vt[0xbc / 4] = begin;
                    VirtualProtect(&vt[0xbc / 4], 4, op, &op);
                    if (ok) SkyFor(view);
                    return ok;
                }
            }
        }
        char ok = oViewBegin(view);
        if (ok && g_pass >= 0) SkyFor(view);   // ZT18f: only our own passes (g_pass 2 = the UI camera re-issue: g_skyWant off)
        return ok;
    }

    void __stdcall hSceneDraw(void* self, void* view, void* a2, void* flags) {
        BYTE* cam = view ? *(BYTE**)((BYTE*)view + 0x70) : nullptr;
        if (g_cullActive && g_cullCam) {   // real frustum for the draw (the game rewrites the position itself every update)
            if (!memcmp(g_cullCam + 0x124, g_cullFr, 24)) memcpy(g_cullCam + 0x124, g_cullReal.fr, 24);
            g_cullActive = false;
        }
        if (cam) g_mainCam = cam;
        animal::ApplyTurn();
        Pose ride1{}; ride1.valid = animal::RidePose(0, ride1.tx, ride1.ty, ride1.tz, ride1.yaw, ride1.pitch, ride1.dist);
        if (!cam) { oSceneDraw(self, view, a2, flags); return; }
        if (g_haveReal && g_wideCam == cam) { memcpy(cam + 0x124, g_realFr, 24); g_haveReal = false; }   // undo the between-frames widening (ZT18f: before the solo / off path too)
        if (!On()) {   // ZT18f: also the game's photo mode (solo) - player 1 full screen, player 2's state kept
            if (!ride1.valid) { oSceneDraw(self, view, a2, flags); return; }
            CamSave s1; Save(cam, s1); WritePose(cam, ride1); RebuildPlanes(cam);
            g_pass = 0; g_skyWant = true; g_port[0] = 0.f; g_port[1] = 1.f; g_port[2] = 1.f; g_port[3] = 0.f;   // full screen
            oSceneDraw(self, view, a2, flags);
            g_skyWant = false; g_pass = -1;
            Restore(cam, s1);
            RebuildPlanes(cam);
            return;
        }
        if (!g_p2.valid) PoseFromCamera(cam, g_p2);
        { Pose r2; if (animal::RidePose(1, r2.tx, r2.ty, r2.tz, r2.yaw, r2.pitch, r2.dist)) { r2.valid = true; g_p2 = r2; } }
        CamSave s; Save(cam, s);
        if (ride1.valid) WritePose(cam, ride1);
        const bool vertical = true;
        // player 1 (left half) first: one-pass world highlights (selection glow, placement footprints) are drawn in
        // the first pass and must be seen by player 1, whose HUD and picking are the game's own (JPOG 3k/36 lesson).
        // Then player 2 (right half) without a second BeginFrame. The camera is restored to player 1 afterwards.
        float* fr = (float*)(cam + 0x124);
        g_passCam = cam;
        D3DVIEWPORT9 p2vp{}; bool p2vpOk = false;   // ZT18f: player 2's half as the game set it (for player 2's photo)
        for (int k = 0; k < 2; ++k) {
            int who = g_p2First ? 1 - k : k;
            g_pass = who;
            // ZT18f: the half frustum is set per pass (player 2's photo view narrows it)
            memcpy(fr, s.fr, 16);
            if (vertical) { fr[0] = s.fr[0] * 0.5f; fr[1] = s.fr[1] * 0.5f; } else { fr[2] = s.fr[2] * 0.5f; fr[3] = s.fr[3] * 0.5f; }
            g_skyWant = who == 1 || ride1.valid;   // ZT18f: painted sky behind our own poses
            if (who == 0) {
                g_port[0] = 0.f; g_port[1] = vertical ? 0.5f : 1.f; g_port[2] = 1.f; g_port[3] = vertical ? 0.f : 0.5f;
                fr[4] = s.fr[4];   // ZT18e: player 1's own near plane (player 2's first person may have lowered it)
                if (k == 1) { memcpy(cam + 0x6c, s.rot, 36); memcpy(cam + 0x90, s.pos, 12); memcpy(cam + 0x100, s.vec, 36); cam[0x23] = s.flag;
                              if (ride1.valid) WritePose(cam, ride1); }
            } else {
                g_port[0] = vertical ? 0.5f : 0.f; g_port[1] = 1.f; g_port[2] = vertical ? 1.f : 0.5f; g_port[3] = 0.f;
                WritePose(cam, g_p2);
                // ZT18e: eye-level view - near plane close enough for fences / guests right in front of player 2
                fr[4] = (g_p2Fp && s.fr[4] > kFpNear) ? kFpNear : s.fr[4];
                if (g_p2Zoom > 1.01f) for (int i = 0; i < 4; ++i) fr[i] /= g_p2Zoom;   // ZT18f: photo view zoom
            }
            g_skipBeginFrame = k == 1;
            RebuildPlanes(cam);   // this pass's pose + half frustum
            oSceneDraw(self, view, a2, flags);
            if (who == 1 && g_p2ShotReq) { IDirect3DDevice9* d = d3dhook::Device(); p2vpOk = d && SUCCEEDED(d->GetViewport(&p2vp)); }
        }
        g_skipBeginFrame = false;
        g_skyWant = false;
        if (g_p2ShotReq) {   // ZT18f: player 2's photo = player 2's half right after the world draw (no UI on it yet)
            g_p2ShotReq = false;
            IDirect3DDevice9* d = d3dhook::Device(); D3DVIEWPORT9 vp = p2vp;
            bool ok = false;
            if (d && p2vpOk) GUARDED("split:grab", {
                RECT r{ (LONG)vp.X, (LONG)vp.Y, (LONG)(vp.X + vp.Width), (LONG)(vp.Y + vp.Height) };
                ok = overlay::GrabRect(d, r); });
            LOG("split: player 2 photo grabbed %s (%lux%lu at %lu,%lu)", ok && overlay::GrabPending() ? "ok" : "FAILED", vp.Width, vp.Height, vp.X, vp.Y);
        }
        Restore(cam, s);
        memcpy(cam + 0x124, s.fr, 24);
        RebuildPlanes(cam);   // player 1's real planes / view matrix again
        if (config::Get().cullUnion && g_p2.valid) {   // union cull view for the next update (undone at the next draw / picks)
            Save(cam, g_cullReal); g_cullCam = cam; CullEnter(cam); g_cullActive = true;
        }
        if (g_wide > 1.f) {   // widen for the next update's culling; undone at the start of the next draw
            memcpy(g_realFr, s.fr, 24); g_haveReal = true; g_wideCam = cam;
            float* f = (float*)(cam + 0x124); f[0] *= g_wide; f[1] *= g_wide; f[2] *= g_wide; f[3] *= g_wide;
        } else g_haveReal = false;
        // Re-issue player 1's full-screen camera to the renderer (as vanilla leaves it before the UI): the UI and
        // its 3D widgets draw with the renderer's current camera data and viewport, not the D3D viewport alone.
        g_pass = 2;
        g_port[0] = 0.f; g_port[1] = 1.f; g_port[2] = 1.f; g_port[3] = 0.f;
        g_skipBeginFrame = true;
        hViewBegin(view);
        g_skipBeginFrame = false;
        g_pass = -1; g_passCam = nullptr;
    }
}

// ---- View::WindowPointToRay (0x723ed1, eax = view, x, y, &origin, &dir, ret 0x10): every world pick goes through it.
// Trace: log each distinct caller chain once (find the tools' pick paths). Player 2 context: the pick is made from
// player 2's camera through the middle of player 2's half (the JPOG AimSwap idea).
namespace { int g_p2px = -1, g_p2py = -1; bool g_p2Pick = false; bool g_p2PickActive = false; CamSave g_p2PickSave; int g_traceN = 0; DWORD g_trace[96][3]; }
extern "C" void __cdecl ZtRayPre(DWORD ret, DWORD ebpv, int* x, int* y) {
    if (g_cullActive && g_cullCam) { memcpy(g_cullTmp.fr, g_cullCam + 0x124, 24); memcpy(g_cullCam + 0x124, g_cullReal.fr, 24); g_cullSwapped = true; }
    if (g_traceN < 96) {
        DWORD r2 = 0, r3 = 0;
        MEMORY_BASIC_INFORMATION mi{};
        auto ok = [&](DWORD a) { return a > 0x10000 && VirtualQuery((void*)a, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)); };
        if (ok(ebpv + 4)) { r2 = *(DWORD*)(ebpv + 4); DWORD e2 = *(DWORD*)ebpv; if (ok(e2 + 4)) r3 = *(DWORD*)(e2 + 4); }
        bool seen = false;
        for (int i = 0; i < g_traceN && !seen; ++i) seen = g_trace[i][0] == ret && g_trace[i][1] == r2;
        if (!seen) { g_trace[g_traceN][0] = ret; g_trace[g_traceN][1] = r2; g_trace[g_traceN][2] = r3; ++g_traceN;
            LOG("pick trace: ray from %08lx <- %08lx <- %08lx  at %d,%d", ret, r2, r3, *x, *y); }
    }
    g_p2PickActive = false;
    if (g_p2Pick && On() && g_mainCam && g_p2.valid) {
        D3DVIEWPORT9 vp{}; IDirect3DDevice9* d = d3dhook::Device();
        RECT rc{}; HWND h = GetForegroundWindow(); if (h) GetClientRect(h, &rc);
        int W = rc.right > 0 ? rc.right : 1920, H = rc.bottom > 0 ? rc.bottom : 1080; (void)vp; (void)d;
        *x = g_p2px >= 0 ? g_p2px : W * 3 / 4; *y = g_p2py >= 0 ? g_p2py : H / 2;
        Save(g_mainCam, g_p2PickSave); WritePose(g_mainCam, g_p2);
        g_p2PickActive = true;
    }
}
extern "C" void __cdecl ZtRayPost() {
    if (g_p2PickActive && g_mainCam) Restore(g_mainCam, g_p2PickSave);
    g_p2PickActive = false;
    if (g_cullSwapped && g_cullCam) { memcpy(g_cullCam + 0x124, g_cullTmp.fr, 24); g_cullSwapped = false; }
}
extern "C" { void* zt_oViewRay = nullptr; }
__attribute__((naked)) static void hViewRay() {
    asm volatile(
        "pushal\n\t"
        "lea 40(%esp), %ecx\n\t"      /* after pushal: ret at 32, x at 36, y at 40 */
        "lea 36(%esp), %edx\n\t"
        "push %ecx\n\t"
        "push %edx\n\t"
        "push %ebp\n\t"
        "pushl 44(%esp)\n\t"          /* return address */
        "call _ZtRayPre\n\t"
        "add $16, %esp\n\t"
        "popal\n\t"
        "pushl 16(%esp)\n\t"
        "pushl 16(%esp)\n\t"
        "pushl 16(%esp)\n\t"
        "pushl 16(%esp)\n\t"
        "call *_zt_oViewRay\n\t"
        "pushal\n\t"
        "call _ZtRayPost\n\t"
        "popal\n\t"
        "ret $16\n\t");
}
extern "C" void __cdecl ZtPickPre(BYTE* cam) {
    g_pickSwapped = false;
    if (g_p2PickActive && cam == g_mainCam && cam) {   // player 2's pick: right-half port + half frustum
        memcpy(g_pickSave, cam + 0x124, 40);
        float* fr = (float*)(cam + 0x124); float* port = (float*)(cam + 0x13c);
        fr[0] *= 0.5f; fr[1] *= 0.5f;
        port[0] = 0.5f; port[1] = 1.f; port[2] = 1.f; port[3] = 0.f;
        g_pickSwapped = true; return;
    }
    if (!On() || g_pass >= 0 || cam != g_mainCam || !cam) return;
    memcpy(g_pickSave, cam + 0x124, 40);              // frustum (6) + port (4)
    float* fr = (float*)(cam + 0x124); float* port = (float*)(cam + 0x13c);
    fr[0] *= 0.5f; fr[1] *= 0.5f;
    port[0] = 0.f; port[1] = 0.5f; port[2] = 1.f; port[3] = 0.f;
    g_pickSwapped = true;
}
extern "C" void __cdecl ZtPickPost(BYTE* cam) {
    if (g_pickSwapped && cam) memcpy(cam + 0x124, g_pickSave, 40);
    g_pickSwapped = false;
}
// naked thunk: keep eax (out ptr) / ecx (this) for the original, copy its 3 stack args, call, restore, ret 0xc
extern "C" { void* zt_oPortToFrustum = nullptr; }
__attribute__((naked)) static void hPortToFrustum() {
    asm volatile(
        "pushal\n\t"
        "push %ecx\n\t"
        "call _ZtPickPre\n\t"
        "add $4, %esp\n\t"
        "popal\n\t"
        "push %ecx\n\t"
        "pushl 16(%esp)\n\t"
        "pushl 16(%esp)\n\t"
        "pushl 16(%esp)\n\t"
        "call *_zt_oPortToFrustum\n\t"
        "pop %ecx\n\t"
        "pushal\n\t"
        "push %ecx\n\t"
        "call _ZtPickPost\n\t"
        "add $4, %esp\n\t"
        "popal\n\t"
        "ret $12\n\t");
}
// WorldPtToScreenPt (0x591a80): ecx=cam, eax=world point, esi/edx = out x/y (0..1 of the port), 1 stack arg, ret 4.
// Icons, selection boxes and floating text project through it. For player 1 in split-screen the full-screen result X
// becomes X - 0.25 in the left half (half frustum on a half viewport); outside [0, 0.5] = not on player 1's screen.
extern "C" int __cdecl ZtW2SAdjust(int ok, BYTE* cam, float* x) {
    if (!On() || g_pass >= 0 || cam != g_mainCam || !cam || !x) return ok;
    *x -= 0.25f;
    if (*x < 0.f || *x > 0.5f) return 0;
    return ok;
}
extern "C" { void* zt_oWorldToScreen = nullptr; }
__attribute__((naked)) static void hWorldToScreen() {
    asm volatile(
        "push %ecx\n\t"
        "push %edx\n\t"
        "push %esi\n\t"
        "pushl 16(%esp)\n\t"
        "call *_zt_oWorldToScreen\n\t"
        "movzbl %al, %eax\n\t"
        "pushl 0(%esp)\n\t"
        "pushl 12(%esp)\n\t"
        "push %eax\n\t"
        "call _ZtW2SAdjust\n\t"
        "add $12, %esp\n\t"
        "pop %esi\n\t"
        "pop %edx\n\t"
        "pop %ecx\n\t"
        "ret $4\n\t");
}
namespace split {
void Install() {
    static bool done = false; if (done) return; done = true;
    g_retStub = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (g_retStub) g_retStub[0] = 0xC3;
    static const unsigned char proScene[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    static const unsigned char proView[] = {0x53, 0x8B, 0x5C, 0x24, 0x08};
    hook::Install(0x50310d, proScene, 6, (void*)hSceneDraw, (void**)&oSceneDraw, "SceneDraw");
    hook::Install(0x724994, proView, 5, (void*)hViewBegin, (void**)&oViewBegin, "ViewBegin(split)");
    static const unsigned char proPick[] = {0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x45, 0x08};
    hook::Install(0x591d17, proPick, 7, (void*)hPortToFrustum, &zt_oPortToFrustum, "PortToFrustum(pick)");
    static const unsigned char proW2S[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};
    hook::Install(0x591a80, proW2S, 6, (void*)hWorldToScreen, &zt_oWorldToScreen, "WorldToScreen(icons)");
    static const unsigned char proRay[] = {0x57, 0x8B, 0x78, 0x70, 0x85, 0xFF};
    hook::Install(0x723ed1, proRay, 6, (void*)hViewRay, &zt_oViewRay, "ViewWindowPointToRay(pick)");
}
bool StandZ(float x, float y, float& h);
void SetP2Pick(bool on) { g_p2Pick = on; }
void P2FirstPerson(bool on) {
    if (on == g_p2Fp) return;
    if (on) {
        if (!g_p2.valid) return;
        g_p2Over = g_p2;   // overhead view to come back to
        float h = g_p2.tz; StandZ(g_p2.tx, g_p2.ty, h);   // ZT18f: on the water surface like the game's guest walk
        g_p2.tz = h + kEye; g_p2.dist = kFpDist; g_p2.pitch = 0.1f;   // stand at the look-at point, look ahead
        g_p2Fp = true;
        LOG("split: player 2 first person at %.1f %.1f ground %.1f (game near plane %.2f)", g_p2.tx, g_p2.ty, h, g_mainCam ? ((float*)(g_mainCam + 0x124))[4] : -1.f);
    } else {
        g_p2Fp = false;
        if (g_p2Over.valid && g_p2.valid) {   // overhead view back, centred where player 2 walked to
            float x = g_p2.tx, y = g_p2.ty; g_p2 = g_p2Over; g_p2.tx = x; g_p2.ty = y;
            float h; if (TerrainZ(x, y, h)) g_p2.tz = h;
        }
        LOG("split: player 2 first person off");
    }
}
bool P2InFirstPerson() { return g_p2Fp; }
void P2FpInput(float lx, float ly, float rx, float ry, float dt, bool run) {
    if (!g_p2Fp || !g_p2.valid) return;
    const float look = 2.4f;   // rad/s at full right stick
    g_p2.yaw += rx * look * dt;
    g_p2.pitch -= ry * look * 0.7f * dt;   // RS up = look up (pitch > 0 looks down)
    if (g_p2.pitch < -1.2f) g_p2.pitch = -1.2f; if (g_p2.pitch > 1.3f) g_p2.pitch = 1.3f;
    float m = sqrtf(lx * lx + ly * ly); if (m > 1.f) { lx /= m; ly /= m; m = 1.f; }
    if (m > 0.f) {
        float sp = (run ? 9.f : 4.f * m) * dt;   // walk ~4 m/s, full push = run ~9 m/s
        float sy = sinf(g_p2.yaw), cy = cosf(g_p2.yaw);
        float nx = g_p2.tx + (sy * ly + cy * lx) * sp, ny = g_p2.ty + (cy * ly - sy * lx) * sp, h;
        if (TerrainZ(nx, ny, h)) { g_p2.tx = nx; g_p2.ty = ny; }   // stop at the edge of the map
    }
    float h;
    if (StandZ(g_p2.tx, g_p2.ty, h)) { float k = dt * 12.f; if (k > 1.f) k = 1.f; g_p2.tz += (h + kEye - g_p2.tz) * k; }
}
void P2FpLook(float dyaw, float dpitch) {
    if (!g_p2Fp || !g_p2.valid) return;
    g_p2.yaw += dyaw; g_p2.pitch += dpitch;
    if (g_p2.pitch < -1.2f) g_p2.pitch = -1.2f; if (g_p2.pitch > 1.3f) g_p2.pitch = 1.3f;
}
bool P2Eye(float& x, float& y, float& z, float& yaw, float& pitch) {
    if (!g_p2.valid) return false;
    x = g_p2.tx; y = g_p2.ty; z = g_p2.tz; yaw = g_p2.yaw; pitch = g_p2.pitch; return true;
}
void SetSolo(bool on) { if (on != g_solo) { g_solo = on; LOG("split: %s", on ? "the game's photo mode - player 1 full screen" : "photo mode over - both halves again"); } }
bool Solo() { return g_solo; }
void SetP2Zoom(float z) { g_p2Zoom = z < 1.f ? 1.f : z > 6.f ? 6.f : z; }
void RequestP2Photo() { g_p2ShotReq = true; }
// ZT18f: water surface under x,y. The game's guest-walk camera (world/cameras/fpscam.xml BFAboveGroundFitComponent
// floatOnWater) stands on the water: its fit (0x727301..) reads the terrain vertex grid - grid = the same world object
// TerrainZ uses ([[0x9352d0]+0x210]+0xf0, component +0xf4 is copied from it at 0x727114): +0x90 vertex pointer array,
// +0x94 width, +0x98 height (0x50e344); vertex +0xc ground z, +0x14 water data (null = dry), water +0x14 surface z,
// vertex +0x2c / +0x2e grid column / row (words, x = col * [0x9355d4], y = row * [0x9355d8]).
// If the ground is below surface + 0.01 the game stands the camera at surface + 0.1.
bool WaterZ(float x, float y, float& z) {
    static int s_order = -1; static bool s_logged = false;
    bool found = false;
    GUARDED("split:WaterZ", {
        BYTE* g = *(BYTE**)0x9352d0; BYTE* v = g ? *(BYTE**)(g + 0x210) : nullptr; BYTE* w = v ? *(BYTE**)(v + 0xf0) : nullptr;
        float cx = *(float*)0x9355d4, cy = *(float*)0x9355d8;
        if (w && cx > 0.01f && cy > 0.01f) {
            int W = *(int*)(w + 0x94), H = *(int*)(w + 0x98); BYTE** arr = *(BYTE***)(w + 0x90);
            int col = (int)floorf(x / cx + 0.5f), row = (int)floorf(y / cy + 0.5f);
            if (arr && W > 0 && H > 0 && W < 100000 && H < 100000) {
                for (int t = 0; t < 2 && !found; ++t) {
                    int order = s_order >= 0 ? s_order : t;   // 0: index = row*W+col, 1: col*W+row (checked once by the grid words)
                    if (s_order >= 0 && t > 0) break;
                    int a = order == 0 ? row : col, b = order == 0 ? col : row;
                    if (a < 0 || b < 0 || b >= W || a * W + b >= W * H) continue;
                    BYTE* vx = arr[a * W + b];
                    if (!vx) continue;
                    int vc = *(WORD*)(vx + 0x2c), vr = *(WORD*)(vx + 0x2e);
                    if (vc != col || vr != row) {
                        if (!s_logged && t == 1) { s_logged = true; LOG("split: water grid - vertex words %d,%d for column %d row %d (cell %.2f x %.2f, %dx%d) - no water test", vc, vr, col, row, cx, cy, W, H); }
                        continue;
                    }
                    if (s_order < 0) { s_order = order; LOG("split: water grid index = %s (cell %.2f x %.2f, %dx%d vertices)", order == 0 ? "row*width+column" : "column*width+row", cx, cy, W, H); }
                    BYTE* wd = *(BYTE**)(vx + 0x14);
                    if (wd) { z = *(float*)(wd + 0x14); found = true; }
                    else break;   // dry vertex
                }
            }
        }
    });
    return found;
}
// standing height: the ground, or the water surface + 0.1 where the ground is under water (the game's floatOnWater fit)
bool StandZ(float x, float y, float& h) {
    if (!TerrainZ(x, y, h)) return false;
    float w; if (WaterZ(x, y, w) && h <= w + 0.01f) h = w + 0.1f;
    return true;
}
void SetP2Pointer(int x, int y) { g_p2px = x; g_p2py = y; }
void SetActive(bool on) { if (on != g_on) { g_on = on; LOG("split: %s", on ? "ON (vertical)" : "off"); if (!on) { g_p2.valid = false; g_p2Fp = false; } } }
bool Active() { return g_on; }
void* MainCam() { return g_mainCam; }
void ToggleWide() { g_wide = g_wide > 1.f ? 1.f : 4.f; LOG("split: test - frustum widening between frames x%.0f", g_wide); }
void ToggleP2First() { g_p2First = !g_p2First; LOG("split: test - player %d drawn first", g_p2First ? 2 : 1); }
void P2Follow(float dt);
void P2Input(float panX, float panY, float rotate, float zoom, float dt, float tilt) {
    if (!g_p2.valid || g_p2Fp) return;   // ZT18e: first person moves with P2FpInput
    g_p2.pitch += tilt * 1.2f * dt;
    if (g_p2.pitch < 0.35f) g_p2.pitch = 0.35f; if (g_p2.pitch > 1.45f) g_p2.pitch = 1.45f;
    float speed = (5.f + 0.5f * g_p2.dist) * dt;   // ZT07: was (20 + dist), flew off the map
    float sy = sinf(g_p2.yaw), cy = cosf(g_p2.yaw);
    // forward on the ground = (sinY, cosY), right = (cosY, -sinY)
    float nx = g_p2.tx + (sy * panY + cy * panX) * speed, ny = g_p2.ty + (cy * panY - sy * panX) * speed, h;
    if ((panX || panY) && TerrainZ(nx, ny, h)) { g_p2.tx = nx; g_p2.ty = ny; }   // stop at the edge of the map
    g_p2.yaw += rotate * 1.8f * dt;
    g_p2.dist *= 1.f + zoom * 1.5f * dt;
    const float mx = config::Get().maxZoom; if (g_p2.dist < 8.f) g_p2.dist = 8.f; if (g_p2.dist > mx) g_p2.dist = mx;
    P2Follow(dt);
}
// stdcall thunk: (world, org, hit, nrm, dir) -> eax = dir, call 0x71c08a(world, org, hit, nrm) (stdcall, ret 0x10)
extern "C" char __stdcall zt_RayTerrain(void* world, float* org, float* hit, float* nrm, float* dir);
__asm__(".globl _zt_RayTerrain@20\n_zt_RayTerrain@20:\n"
        "  push %ebp\n  mov %esp, %ebp\n"
        "  push 20(%ebp)\n  push 16(%ebp)\n  push 12(%ebp)\n  push 8(%ebp)\n"
        "  mov 24(%ebp), %eax\n  mov $0x71c08a, %ecx\n  call *%ecx\n"
        "  movzbl %al, %eax\n  pop %ebp\n  ret $20\n");
// Terrain height under (x,y): the game's own ray->terrain test 0x71c08a (the mouse-pick path 0x46fce4 uses it).
// stdcall, eax = ray direction (scaled by 1000 inside), args: world = [[0x9352d0]+0x210]+0xf0, &origin, &hit, &normal.
bool TerrainZ(float x, float y, float& z) {
    BYTE* g = *(BYTE**)0x9352d0; if (!g) return false;
    BYTE* v = *(BYTE**)(g + 0x210); if (!v) return false;
    void* world = *(void**)(v + 0xf0); if (!world) return false;
    static float dir[3] = { 0.f, 0.f, -1.f };
    float org[3] = { x, y, 600.f }, hit[3] = { 0, 0, 0 }, nrm[3] = { 0, 0, 0 };
    char r = 0;
    bool ok = GUARDED("split:TerrainZ", { r = zt_RayTerrain(world, org, hit, nrm, dir); });
    if (!ok || !r) return false;
    z = hit[2]; return true;
}
bool P2Aim(float& x, float& y) { if (!g_p2.valid) return false; x = g_p2.tx; y = g_p2.ty; return true; }
void P2Follow(float dt) {   // JPOG-style: player 2's look-at point rides on the ground, eased
    if (!g_p2.valid) return;
    float h;
    if (TerrainZ(g_p2.tx, g_p2.ty, h)) {
        float k = dt * 8.f; if (k > 1.f) k = 1.f;
        g_p2.tz += (h - g_p2.tz) * k;
    }
}
void* View() { return g_view; }
void* CurrentPassCamera() { return g_passCam; }
int CurrentPass() { return g_pass; }
}

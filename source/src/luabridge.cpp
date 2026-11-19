#include <windows.h>
#include <cstdio>
#include <cstring>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lstate.h"
#include "lobject.h"
}
#include "luabridge.h"
#include "guard.h"
#include "log.h"
#include "render.h"
#include "split.h"

// ---- allocator: must be the game's CRT heap (msvcr80), objects are freed by either side's GC ----
static void* (__cdecl* g_crtRealloc)(void*, size_t) = nullptr;
static void  (__cdecl* g_crtFree)(void*) = nullptr;
extern "C" void* zt_realloc(void* b, size_t s) { return g_crtRealloc(b, s); }
extern "C" void  zt_free(void* b) { g_crtFree(b); }

namespace {
    struct LibFn { DWORD entryVA; const char* name; lua_CFunction orig; };
    // zt.exe 33.5.0.2 luaL_reg entries (VA of the {name, func} pair) - checked against the name string before patching.
    LibFn g_fn[] = {
        {0x8bc908, "type", nullptr}, {0x8bc900, "tostring", nullptr}, {0x8bc8f8, "tonumber", nullptr},
        {0x8bc8d8, "next", nullptr}, {0x8bc8e8, "pairs", nullptr}, {0x8bc8e0, "ipairs", nullptr},
        {0x8bc740, "getn", nullptr},
    };
    lua_State* g_main = nullptr;
    // zt.exe 33.5.0.2: the game's "current Lua context" (BFLuaContext*) used by queryObject/handleMessage
    // (read in 0x80b7c6). Set while the game runs its own scripts; we borrow the last one seen.
    void** const kCurCtx = (void**)0x9356a8;
    void* g_ctx = nullptr;          // context we run in (one whose queryObject finds the game's objects)
    void* g_seen[8]; int g_nSeen = 0;
    bool  g_ctxChecked[8];
    bool g_bad = false;

    typedef void* (__cdecl* GetCtx_t)();
    GetCtx_t const GetCurrentLuaContext = (GetCtx_t)0x80b7c6;   // top of the game's executing-object stack, if it is a BFLuaContext
    void Capture(lua_State* L) {
        void* c = *kCurCtx ? *kCurCtx : GetCurrentLuaContext();
        if (c) {
            bool known = false; for (int i = 0; i < g_nSeen; ++i) if (g_seen[i] == c) known = true;
            if (!known && g_nSeen < 8) { g_ctxChecked[g_nSeen] = false; g_seen[g_nSeen++] = c; LOG("lua: game script context %p (vtbl %p)", c, *(void**)c); }
        }
        if (g_main || g_bad) return;
        global_State* g = L->l_G;
        lua_State* m = g ? g->mainthread : nullptr;
        long span = (long)((char*)L->top - (char*)L->base);
        bool ok = m && m->l_G == g && span >= 0 && span % (long)sizeof(TObject) == 0 && span <= 64 * (long)sizeof(TObject)
               && L->stack <= L->base && L->top <= L->stack_last;
        LOG("lua: first call from the game: L=%p G=%p main=%p args=%ld sizeof(TObject)=%u thread=%lu -> %s",
            L, g, m, span / (long)sizeof(TObject), (unsigned)sizeof(TObject), GetCurrentThreadId(), ok ? "layout OK" : "LAYOUT MISMATCH, bridge off");
        if (ok) g_main = m; else g_bad = true;
    }
    template <int I> int __cdecl Wrap(lua_State* L) { Capture(L); return g_fn[I].orig(L); }
    lua_CFunction g_wraps[] = { Wrap<0>, Wrap<1>, Wrap<2>, Wrap<3>, Wrap<4>, Wrap<5>, Wrap<6> };

    // print(...) for our chunks: appends to the current result buffer
    char* g_out = nullptr; int g_outSize = 0, g_outLen = 0;
    void Append(const char* s) {
        if (!g_out) { LOG("lua> %s", s); return; }
        int n = (int)strlen(s); if (g_outLen + n + 2 >= g_outSize) n = g_outSize - g_outLen - 2; if (n <= 0) return;
        memcpy(g_out + g_outLen, s, n); g_outLen += n; g_out[g_outLen++] = '\n'; g_out[g_outLen] = 0;
    }
    int __cdecl L_print(lua_State* L) {
        int n = lua_gettop(L);
        if (n == 1 && lua_type(L, 1) == LUA_TSTRING && lua_strlen(L, 1) > 1500) { Append(lua_tostring(L, 1)); return 0; }
        char line[2048]; int len = 0; line[0] = 0;
        for (int i = 1; i <= n; ++i) {
            const char* s; char tmp[64];
            switch (lua_type(L, i)) {
                case LUA_TNUMBER: snprintf(tmp, sizeof tmp, "%.14g", lua_tonumber(L, i)); s = tmp; break;
                case LUA_TSTRING: s = lua_tostring(L, i); break;
                case LUA_TBOOLEAN: s = lua_toboolean(L, i) ? "true" : "false"; break;
                case LUA_TNIL: s = "nil"; break;
                default: snprintf(tmp, sizeof tmp, "%s:%p", lua_typename(L, lua_type(L, i)), lua_topointer(L, i)); s = tmp;
            }
            len += snprintf(line + len, sizeof line - len, i > 1 ? "\t%s" : "%s", s ? s : "?");
            if (len >= (int)sizeof line - 1) break;
        }
        Append(line); return 0;
    }
    int __cdecl L_log(lua_State* L) { LOG("lua: %s", luaL_checkstring(L, 1)); return 0; }
    // RE helpers for the test REPL. Readable check with VirtualQuery (IsBadReadPtr faults through our VEH guard).
    bool Readable(DWORD a, DWORD n) {
        MEMORY_BASIC_INFORMATION mi{};
        if (a < 0x10000 || !VirtualQuery((void*)a, &mi, sizeof mi)) return false;
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !mi.Protect) return false;
        return a + n <= (DWORD)mi.BaseAddress + mi.RegionSize;
    }
    #define IsBadReadPtr(p, n) (!Readable((DWORD)(p), (DWORD)(n)))
    // zt2mod.call(fn, eax, ecx, a1, a2, ...) -> eax, st0 : any game function (cdecl/stdcall/thiscall/usercall eax+ecx);
    // esp is restored after the call, so the callee's cleanup style does not matter. Numbers with a fraction or
    // given as zt2mod.f(x) are pushed as floats.
    extern "C" DWORD zt_GenericCall(DWORD fn, DWORD eaxv, DWORD ecxv, const DWORD* args, int n, float* st0);
    // st0 != null: the callee returns a float in st(0), stored there.
    __asm__(".globl _zt_GenericCall\n_zt_GenericCall:\n"
            "  push %ebp\n  mov %esp, %ebp\n  push %ebx\n  push %esi\n  push %edi\n"
            "  mov 20(%ebp), %esi\n  mov 24(%ebp), %ecx\n"
            "1: test %ecx, %ecx\n  jz 2f\n  dec %ecx\n  push (%esi,%ecx,4)\n  jmp 1b\n"
            "2: mov 8(%ebp), %ebx\n  mov 12(%ebp), %eax\n  mov 16(%ebp), %ecx\n"
            "  call *%ebx\n"
            "  lea -12(%ebp), %esp\n"
            "  mov 28(%ebp), %ecx\n  test %ecx, %ecx\n  jz 3f\n  fstps (%ecx)\n"
            "3: pop %edi\n  pop %esi\n  pop %ebx\n  pop %ebp\n  ret\n");
    int __cdecl L_peek(lua_State* L) { DWORD a = (DWORD)luaL_checknumber(L, 1);
        if (IsBadReadPtr((void*)a, 4)) { lua_pushnil(L); return 1; } lua_pushnumber(L, (double)*(DWORD*)a); return 1; }
    int __cdecl L_peekf(lua_State* L) { DWORD a = (DWORD)luaL_checknumber(L, 1);
        if (IsBadReadPtr((void*)a, 4)) { lua_pushnil(L); return 1; } lua_pushnumber(L, (double)*(float*)a); return 1; }
    int __cdecl L_peekstr(lua_State* L) { DWORD a = (DWORD)luaL_checknumber(L, 1);
        if (IsBadReadPtr((void*)a, 1)) { lua_pushnil(L); return 1; } char b[256]; int i = 0;
        for (; i < 255 && !IsBadReadPtr((char*)a + i, 1) && ((char*)a)[i]; ++i) b[i] = ((char*)a)[i]; b[i] = 0; lua_pushstring(L, b); return 1; }
    int __cdecl L_poke(lua_State* L) { DWORD a = (DWORD)luaL_checknumber(L, 1); DWORD v = (DWORD)(long long)luaL_checknumber(L, 2);
        DWORD op; if (VirtualProtect((void*)a, 4, PAGE_EXECUTE_READWRITE, &op)) { *(DWORD*)a = v; VirtualProtect((void*)a, 4, op, &op); } return 0; }
    int __cdecl L_pokef(lua_State* L) { DWORD a = (DWORD)luaL_checknumber(L, 1); float v = (float)luaL_checknumber(L, 2);
        DWORD op; if (VirtualProtect((void*)a, 4, PAGE_EXECUTE_READWRITE, &op)) { *(float*)a = v; VirtualProtect((void*)a, 4, op, &op); } return 0; }
    int __cdecl L_call(lua_State* L) {   // zt2mod.call(fn, eax, ecx, [wantFloat], ...) see above; args: numbers; strings "f:1.5" = float
        DWORD fn = (DWORD)luaL_checknumber(L, 1), ea = (DWORD)(long long)luaL_optnumber(L, 2, 0), ec = (DWORD)(long long)luaL_optnumber(L, 3, 0);
        int wantF = lua_toboolean(L, 4);
        DWORD args[16]; int n = 0;
        for (int i = 5; i <= lua_gettop(L) && n < 16; ++i) {
            if (lua_type(L, i) == LUA_TSTRING) { const char* t = lua_tostring(L, i);
                if (t[0] == 'f' && t[1] == ':') { float f = (float)atof(t + 2); memcpy(&args[n++], &f, 4); }
                else args[n++] = (DWORD)t; }
            else args[n++] = (DWORD)(long long)lua_tonumber(L, i);
        }
        float st = 0.f; DWORD r = 0;
        bool ok = GUARDED("lua zt2mod.call", { r = zt_GenericCall(fn, ea, ec, args, n, wantF ? &st : nullptr); });
        if (!ok) { lua_pushnil(L); return 1; }
        lua_pushnumber(L, (double)r); lua_pushnumber(L, (double)st); return 2;
    }
    int __cdecl L_p2pick(lua_State* L) { split::SetP2Pick(lua_toboolean(L, 1) != 0); return 0; }
    // ZT16 research: give one object a private copy of its vtable whose slots count their calls (zt2mod.instrument(obj, nslots))
    volatile LONG g_cnt[256];
    int __cdecl L_instrument(lua_State* L) {
        DWORD obj = (DWORD)luaL_checknumber(L, 1); int n = (int)luaL_optnumber(L, 2, 200); if (n > 256) n = 256;
        DWORD* vt = *(DWORD**)obj;
        DWORD* nv = (DWORD*)VirtualAlloc(nullptr, 4096 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        BYTE* code = (BYTE*)(nv + 300);
        for (int i = 0; i < n; ++i) {
            BYTE* st = code + i * 12;
            st[0] = 0xF0; st[1] = 0xFF; st[2] = 0x05; *(DWORD*)(st + 3) = (DWORD)&g_cnt[i];   // lock inc dword [cnt+i]
            st[7] = 0xE9; *(int*)(st + 8) = (int)(vt[i] - (DWORD)(st + 12));                 // jmp original
            nv[i] = (DWORD)st;
        }
        for (int i = 0; i < 256; ++i) g_cnt[i] = 0;
        *(DWORD**)obj = nv;
        lua_pushnumber(L, (double)(DWORD)nv); return 1;
    }
    int __cdecl L_counts(lua_State* L) {
        static char buf[4096]; int k = 0; buf[0] = 0;
        for (int i = 0; i < 256; ++i) if (g_cnt[i]) { k += snprintf(buf + k, sizeof buf - k, "%03x:%ld ", i * 4, g_cnt[i]); g_cnt[i] = 0; }
        lua_pushstring(L, buf); return 1;
    }
    int __cdecl L_alloc(lua_State* L) { int n = (int)luaL_checknumber(L, 1); void* p = calloc(1, n < 16 ? 16 : n); lua_pushnumber(L, (double)(DWORD)p); return 1; }
    int __cdecl L_cfunc(lua_State* L) { lua_CFunction f = lua_tocfunction(L, 1); lua_pushnumber(L, (double)(DWORD)f); return 1; }
    int __cdecl L_ptr(lua_State* L) { lua_pushnumber(L, (double)(DWORD)lua_topointer(L, 1)); return 1; }   // userdata/table address
    int __cdecl L_view(lua_State* L) { lua_pushnumber(L, (double)(DWORD)render::View()); return 1; }
    int __cdecl L_ctx(lua_State* L) { lua_pushnumber(L, (double)(DWORD)g_ctx); return 1; }
}

namespace luabridge {
void PatchLibTables() {
    HMODULE crt = GetModuleHandleA("msvcr80.dll");
    if (!crt) crt = LoadLibraryA("msvcr80.dll");
    if (crt) { g_crtRealloc = (decltype(g_crtRealloc))GetProcAddress(crt, "realloc"); g_crtFree = (decltype(g_crtFree))GetProcAddress(crt, "free"); }
    if (!g_crtRealloc || !g_crtFree) { LOG("lua: msvcr80 not found, bridge off"); g_bad = true; return; }
    int n = 0;
    for (int i = 0; i < (int)(sizeof g_fn / sizeof g_fn[0]); ++i) {
        DWORD* e = (DWORD*)g_fn[i].entryVA;
        if (IsBadReadPtr(e, 8) || IsBadReadPtr((void*)e[0], 8) || strcmp((const char*)e[0], g_fn[i].name) != 0) {
            LOG("lua: lib table entry %08lx is not '%s' (different zt.exe?), skipped", g_fn[i].entryVA, g_fn[i].name); continue; }
        g_fn[i].orig = (lua_CFunction)e[1];
        DWORD op; VirtualProtect(&e[1], 4, PAGE_READWRITE, &op); e[1] = (DWORD)g_wraps[i]; VirtualProtect(&e[1], 4, op, &op);
        ++n;
    }
    LOG("lua: wrapped %d library functions to find the game's Lua state", n);
}
bool Ready() { return g_main != nullptr; }
// Pick a context in which queryObject("ZTMainMode") works (ZT01f: of the two scenario contexts only one does).
static void ChooseContext() {
    if (g_ctx || !g_main) return;
    lua_State* L = g_main;
    for (int i = 0; i < g_nSeen; ++i) {
        if (g_ctxChecked[i]) continue;
        g_ctxChecked[i] = true;
        void* saved = *kCurCtx; *kCurCtx = g_seen[i];
        bool good = false;
        GUARDED("lua ctx test", {
            int top = lua_gettop(L);
            lua_pushstring(L, "queryObject"); lua_gettable(L, LUA_GLOBALSINDEX);
            lua_pushstring(L, "ZTMainMode");
            if (lua_pcall(L, 1, 1, 0) == 0) good = !lua_isnil(L, -1);
            lua_settop(L, top);
        });
        *kCurCtx = saved;
        LOG("lua: context %p %s", g_seen[i], good ? "finds the game objects - using it" : "does not find the game objects");
        if (good) { g_ctx = g_seen[i]; return; }
    }
}
bool Idle() { return g_main && g_main->nCcalls == 0 && g_main->ci == g_main->base_ci; }

bool Run(const char* code, char* out, int outSize) {
    if (out && outSize) out[0] = 0;
    if (!g_main) { if (out) snprintf(out, outSize, "lua state not captured yet"); return false; }
    if (!Idle()) { if (out) snprintf(out, outSize, "lua busy"); return false; }
    ChooseContext();
    lua_State* L = g_main;
    bool ok = false;
    g_out = out; g_outSize = outSize; g_outLen = 0;
    void* savedCtx = *kCurCtx;
    bool noFault = GUARDED("lua run", {
        int top = lua_gettop(L);
        if (g_ctx && !savedCtx) *kCurCtx = g_ctx;
        // our helpers live in a private table "zt2mod" (never overwrite the game's own globals)
        lua_pushstring(L, "zt2mod"); lua_newtable(L);
        struct { const char* n; lua_CFunction f; } fns[] = { {"print", L_print}, {"log", L_log}, {"peek", L_peek}, {"peekf", L_peekf},
            {"peekstr", L_peekstr}, {"poke", L_poke}, {"pokef", L_pokef}, {"cfunc", L_cfunc}, {"ptr", L_ptr}, {"ctx", L_ctx}, {"view", L_view}, {"call", L_call}, {"alloc", L_alloc}, {"p2pick", L_p2pick}, {"instrument", L_instrument}, {"counts", L_counts} };
        for (auto& f : fns) { lua_pushstring(L, f.n); lua_pushcfunction(L, f.f); lua_settable(L, -3); }
        lua_settable(L, LUA_GLOBALSINDEX);
        if (luaL_loadbuffer(L, code, strlen(code), "=zt2mod") != 0 || lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
            Append(lua_tostring(L, -1) ? lua_tostring(L, -1) : "error");
        } else {
            ok = true;
            int n = lua_gettop(L);
            for (int i = top + 1; i <= n; ++i) {
                lua_pushcfunction(L, L_print); lua_pushvalue(L, i); lua_call(L, 1, 0);
            }
        }
        lua_settop(L, top);
    });
    *kCurCtx = savedCtx;
    g_out = nullptr;
    if (!noFault) { if (out) snprintf(out, outSize, "FAULT while running Lua (see log)"); return false; }
    return ok;
}
}

#pragma once
// Bridge to the game's own Lua 5.0.2 state. We compile stock Lua 5.0 (same struct layout as MSVC with
// -malign-double, allocator redirected to the game's msvcr80) and drive the game's lua_State directly.
// The state is captured by wrapping a few standard library C functions in zt.exe's luaL_reg tables
// (unencrypted .rdata) before the game registers them.
namespace luabridge {
    void PatchLibTables();               // DllMain time
    bool Ready();
    // Run Lua source on the main thread (only call from the game thread, e.g. Present).
    // Returns false on error; out gets the result (tostring of all returns) or the error.
    bool Run(const char* code, char* out, int outSize);
    bool Idle();                         // main thread not inside Lua right now
}

#include <windows.h>
#include "mod.h"
#include "log.h"
BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID reserved) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); mod::Start(); }
    // ZT19k: the game stopped twice with nothing in the log - a normal exit now says so (reserved != null = ExitProcess)
    if (r == DLL_PROCESS_DETACH) LOG("process exit (%s)", reserved ? "the game closed / ExitProcess" : "dll unloaded");
    return TRUE;
}

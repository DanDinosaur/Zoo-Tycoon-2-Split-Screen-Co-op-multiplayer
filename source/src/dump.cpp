#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include "dump.h"
#include "config.h"
#include "log.h"
namespace dump {
void Image(const char* why) {
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lszt2mod_dump.bin", config::GameDir());
    HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { LOG("dump: cannot write"); return; }
    static BYTE zero[4096];
    DWORD bad = 0;
    for (DWORD o = 0; o < size; o += 4096) {
        MEMORY_BASIC_INFORMATION mi{}; VirtualQuery(base + o, &mi, sizeof mi);
        bool rd = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        DWORD w; WriteFile(h, rd ? base + o : zero, 4096, &w, nullptr); if (!rd) ++bad;
    }
    CloseHandle(h);
    LOG("dump: image %p size %lx written (%lu unreadable pages) [%s]", base, size, bad, why);
    swprintf(p, MAX_PATH, L"%lszt2mod_modules.txt", config::GameDir());
    FILE* f = _wfopen(p, L"w");
    if (f) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        MODULEENTRY32W me{ sizeof me };
        for (BOOL ok = Module32FirstW(s, &me); ok; ok = Module32NextW(s, &me))
            fprintf(f, "%08lx %08lx %ls\n", (DWORD)(UINT_PTR)me.modBaseAddr, me.modBaseSize, me.szExePath);
        CloseHandle(s); fclose(f);
    }
}
}

#pragma once
// Inline hook for zt.exe functions whose first instructions we have disassembled from the runtime dump.
// 'expect' = the exact prologue bytes (checked before patching), len >= 5 = whole instructions moved to the trampoline
// (none of them may be relative jumps/calls).
namespace hook {
    bool Install(unsigned addr, const unsigned char* expect, int len, void* fn, void** tramp, const char* name);
}

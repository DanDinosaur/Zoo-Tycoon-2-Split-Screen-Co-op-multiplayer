#pragma once
// ZT10 research tool: hardware data breakpoint (DR0) on one dword, set on the game's main thread through our own
// exception handler; logs every distinct game code address that reads/writes it for a few seconds (zt2mod_watch.flag).
namespace dbgwatch {
    void Install();
    void Arm(void* addr, float now);   // main thread
    void Tick(float now);              // main thread, every frame (disarms after 3 s)
}

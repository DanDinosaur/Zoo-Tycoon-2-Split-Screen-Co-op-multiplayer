#pragma once
// ZT14 test aid: counts Miles sample starts (mss32 AIL_start_sample / AIL_start_3D_sample) and logs the ones that start
// within a few seconds of a riding action, so "did the roar make a sound" can be read from zt2mod.log.
namespace sndwatch {
    void Install();               // once, game thread (mss32.dll is loaded by then)
    long Count3D();               // ZT17: 3D samples started so far
    void Watch(const char* what, float secs);   // log sample starts for the next secs seconds
}

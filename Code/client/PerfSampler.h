#pragma once

#include <cstdint>

// A sampling profiler for performance work, off unless the environment variable RD_PROFILE is set. About 1000 times a
// second the game's main thread is stopped for as long as it takes to copy its stack, which is then walked on the
// sampler's own thread (nothing is read outside that copy and the loaded images: our crash handler dumps on any access
// violation, even a caught one). Every 10 s the log gets where the frames went ("Profile:" lines): inclusive and own
// milliseconds per frame by function, for all frames, for the slow ones against the rest, and for the frames that attached
// cells against the rest; where the main thread waits; and the process's other threads (two sampled a tick, in turn): how
// busy each is and on what. The game's functions show as game+<offset> (Tools/profile-names.js names them from the address
// library), ours as rd+<offset>, other modules by file name.
namespace PerfSampler
{
[[nodiscard]] bool Enabled() noexcept;

// Main thread, once per frame (World::Update): starts the sampler on the first call, then closes the frame that just
// ended with its length
void Frame(double aFrameMs) noexcept;

// Time the game spent attaching cells (Smooth loading's outermost cell pass), any thread: counted in the frame it's in
void AddCellWork(uint64_t aNanoseconds) noexcept;
} // namespace PerfSampler

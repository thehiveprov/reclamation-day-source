#pragma once

// "Match the host's load order", offered when a server refuses us for our plugins: switches on the host's plugins in
// the host's order and switches off the others in plugins.txt (a backup of it is kept next to it), then restarts the
// game, which reads its load order at start. What plugins.txt can't change is reported instead: plugins that aren't
// installed, and the ones the game loads by itself (DLC, Creation Club).
namespace LoadOrderMatch
{
// From the overlay (any thread): the host's plugins in load order, one per line. Done on the game's next frame.
void Queue(std::string aHostOrder) noexcept;

// Every frame, on the game's main thread. Restarts the game, or sends the overlay "loadOrderResult" (JSON: ok, reason
// "missing" | "fixed" | "no_plugins_txt" | "write_failed", plugins).
void RunQueued() noexcept;
} // namespace LoadOrderMatch

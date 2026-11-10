#pragma once

// The game's own menu sounds for the overlay's buttons (UIMenuOK, UIMenuFocus...), so it sounds like Fallout's menus:
// played by the game (its sound records, its volume settings), nothing of ours to ship.
namespace MenuSounds
{
// From the overlay (any thread): played on the game's next frame. Only the menu sounds (see the .cpp).
void Queue(const std::string& acSound) noexcept;

// Every frame, on the game's main thread
void PlayQueued() noexcept;
} // namespace MenuSounds

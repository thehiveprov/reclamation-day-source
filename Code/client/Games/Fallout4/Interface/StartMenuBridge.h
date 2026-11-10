#pragma once

#include <string>

struct UI;

// The game's start menu (MainMenu.swf: the pause menu, and the main menu's screens past its main list) drawn by the
// overlay. The game's movie stays open underneath, hidden, and does all the work: its state and lists are mirrored to
// the overlay as the "startMenu" event, the overlay's choices played back through the movie's own handlers. See
// StartMenuBridge.cpp and docs/MENUS.md.
namespace StartMenuBridge
{
// Every frame on the main loop (at the main menu and in game)
void Run(UI* apUI) noexcept;
// One of the overlay's choices (mainMenuAction "startMenu.<action>"), on the main loop
void OnAction(const std::string& acAction, const std::string& acPayload) noexcept;
// The overlay shows the game's menu: Escape and F2 go to the page (Escape is "back" there)
[[nodiscard]] bool IsOpen() noexcept;
// A state of the game's menu the overlay draws (the main menu's main list is the main menu screen's, not ours)
[[nodiscard]] bool Covers(const std::string& acState, bool aPauseMenu) noexcept;
// The overlay's page started again: it's told again
void OnPageRestarted() noexcept;
} // namespace StartMenuBridge

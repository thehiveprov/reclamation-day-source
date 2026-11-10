#pragma once

#include <string>

namespace GFx
{
struct Movie;
}

// The Mod Configuration Menu's pages (MCM: "MOD CONFIG" in the pause menu) drawn by the overlay. Each mod's
// Data\MCM\Config\<mod>\config.json is read here; values, keybinds and actions go through MCM's own native object
// (root.mcm, which mcm.dll puts on MainMenu.swf's root) and F4SE's SendExternalEvent, the way MCM.swf uses them. The
// overlay gets the mods and the open page as the "modConfig" event. See ModConfigBridge.cpp and docs/MENUS.md.
namespace ModConfigBridge
{
// Every frame with the start menu's movie (MainMenu.swf: the pause menu's), nullptr once it's gone
void Run(GFx::Movie* apMovie) noexcept;
// One of the overlay's choices (mainMenuAction "modConfig.<action>"), on the main loop
void OnAction(const std::string& acAction, const std::string& acPayload) noexcept;
[[nodiscard]] bool IsOpen() noexcept;
// The overlay's page started again: it's told again
void OnPageRestarted() noexcept;
} // namespace ModConfigBridge

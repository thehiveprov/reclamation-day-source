#pragma once

struct TESObjectREFR;

// Terminal menu items. Picking one on a terminal (or in a holotape played on one) runs its script: the menu item's
// fragment and the terminal script's OnMenuItemRun event, both through the game's GameScript::FragmentEventHandler
// (its TerminalMenuItemRunEvent sink). What the local player runs is sent to the players nearby
// (TerminalItemRunEvent, ObjectService), who run the same script on their copy of the terminal: turrets turned off or
// against their owners, doors unlocked, robots given their orders happen in every game.
namespace Terminals
{
// Runs a terminal menu item's script here, as the game does when the local player picks it. aTerminalFormId is the
// terminal (TERM) the item belongs to: the reference's own, or a holotape's played on it.
bool RunMenuItem(TESObjectREFR* apTerminalRef, uint32_t aTerminalFormId, uint16_t aMenuItemId) noexcept;
} // namespace Terminals

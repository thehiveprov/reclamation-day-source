#pragma once

// Fallout 4's random encounters (the story manager's RE branches, the checkpoint quests, corpse swarms, the UFO crash,
// Far Harbor's and Nuka-World's) are only picked while their conditions see the REEnabled global at 1. This makes those
// conditions see 0 while this game shouldn't start encounters, without ever changing the global itself (so a save made
// in the meantime keeps the real value, and the scripts that toggle it keep working).
namespace RandomEncounters
{
void SetAllowed(bool aAllowed) noexcept;
[[nodiscard]] bool IsAllowed() noexcept;
} // namespace RandomEncounters

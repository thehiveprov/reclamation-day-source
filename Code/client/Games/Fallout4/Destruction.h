#pragma once

struct TESObjectREFR;

// Destructible objects (cars, barrels, generators, ...). Each game replays the projectiles, so the same object usually
// takes the same damage everywhere, but not always (a melee hit, an NPC's shot in its owner's game only, a burning car's
// timer): when a destruction stage goes up in the local game, the players nearby bring their copy down to the same
// health through the game's own object damage, which runs that stage there (model, explosion, debris, fire).
// TESDestructionStageChangedEvent -> DestructionStageEvent -> ObjectService.
namespace Destruction
{
// Starts sending the local game's stage changes (call once the game's event sources exist)
void Install() noexcept;

// The object's health and the most it has (from its base form's destructible data), 0 when it isn't destructible
[[nodiscard]] float GetHealth(TESObjectREFR* apObject) noexcept;
[[nodiscard]] float GetMaxHealth(TESObjectREFR* apObject) noexcept;

// Damages the object down to aHealth through the game's object damage (nothing when it's already as low); the stage
// changes this causes aren't sent back
bool DamageTo(TESObjectREFR* apObject, float aHealth) noexcept;
} // namespace Destruction

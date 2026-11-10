#pragma once

struct Actor;

// The Pip-Boy light. The game only gives one to the player (PlayerCharacter::ShowPipboyLight, CommonLibF4 1304102): it
// looks for the Pip-Boy's light in the body (1575000) or takes the PipboyLight default object, and makes it a dynamic
// light on that node (TESObjectLIGH 30546). Other players' characters get the same light here, the same way, while their
// light is on in their game (RequestPipboyLight / NotifyPipboyLight, ObjectService).
namespace PipboyLights
{
[[nodiscard]] bool IsLocalOn() noexcept;

// Gives these characters the light (again when their body was rebuilt) and takes it off every other one
void Update(const Vector<Actor*>& acLit) noexcept;
} // namespace PipboyLights

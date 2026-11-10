#pragma once

struct PlayerCharacter;

// The first-person left arm raised to look at the Pip-Boy and the right one lowered out of view: Realtime Pip-Boy Map's
// Check.kf (PipboyGlanceAnimation.h), put on the first-person arm bones relative to the first-person camera, over
// whatever the game animates, blended in and out like the original's (0.4 s). The game's own animation goes on
// underneath (walking, the fingers).
namespace PipboyGlancePose
{
// Main thread: finds the bones (which can make nodes, so never during the bone tree's update) and starts blending in
bool Start(PlayerCharacter* apPlayer) noexcept;
// Blends out from where it is
void Stop() noexcept;
// Off the arm from the next frame, no blend (the game's own Pip-Boy is about to animate it)
void StopNow() noexcept;
// Raised all the way (blended in and past the clip's start)
bool IsUp() noexcept;
// Anything of it still on the arm
bool IsActive() noexcept;
} // namespace PipboyGlancePose

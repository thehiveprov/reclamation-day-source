#pragma once

// Start Me Up Redux (Dashboarderian), the quick start new co-op characters begin with
namespace StartMeUp
{
// Whether its plugin is in the load order: from the game once its data is loaded, until then from plugins.txt
bool IsInstalled(bool aGameDataLoaded) noexcept;

// Removes its "This is a memory. Let's stay here a while." choice, which keeps playing the pre-war prologue: co-op
// characters always start past it. Returns true once done (or found to be impossible); false while its plugin isn't
// loaded yet, to be tried again.
bool BlockPrewarMemory() noexcept;
} // namespace StartMeUp

#pragma once

// The third-party mods Reclamation Day bundles, and the settings co-op needs from them. Each mod reads the player's
// MCM settings file last, over its own defaults: those settings are written there before the script extender loads
// the mods, at every start, so a value changed in the MCM before comes back to what co-op needs.
namespace BundledMods
{
void ApplyForcedSettings() noexcept;
} // namespace BundledMods

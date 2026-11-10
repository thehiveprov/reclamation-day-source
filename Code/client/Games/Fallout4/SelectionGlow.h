#pragma once

struct TESObjectREFR;

// The game's target glow (the one VATS shows its targets with; the workshop and companion commands use it too) on one
// reference's 3D. Shared with those: ours is only put up while nobody else's is, and only cleared while it's still
// ours. Main thread only.
struct SelectionGlow
{
    // Glows the reference with aHandle in the colour (0..1); the same reference and colour again changes nothing
    static void Set(TESObjectREFR* apRef, uint32_t aHandle, float aRed, float aGreen, float aBlue) noexcept;
    // Takes ours down
    static void Clear() noexcept;
    // Forgets ours without touching the glow when it isn't ours any more (VATS or the workshop cleared it to show their
    // own)
    static void Release() noexcept;
    // The reference that glows (0 for none)
    [[nodiscard]] static uint32_t Current() noexcept;
};

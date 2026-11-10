#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The game's own HUD: its pieces (HUDMenu's components) told "not visible" while the overlay draws them, and the data
// the game would have shown in them (HUDDataModel). Fallout 4 1.10.163; every fact used is in docs/HUD.md.
namespace GameHud
{
enum class Piece : uint8_t
{
    kCompass,
    kHealth,
    kRadiation,
    kActionPoints,
    kEnemyHealth,
    kAmmo,
    kExplosives,
    // the crosshair: hidden on its own (HideCrosshair), not with the rest
    kCrosshair,
    kCount,
};

// Puts our "should it be visible" in each piece's vtable, once (main thread, once the game runs). Our answer is the
// game's own until Hide(true)
void Install() noexcept;
// True: the pieces say "not visible" and the HUD fades them out. The game's own answer is kept either way (WouldShow)
void Hide(bool aHide) noexcept;
// True: the game's crosshair says "not visible" (two guns out: the overlay draws its own, docs/DUAL_WIELD.md)
void HideCrosshair(bool aHide) noexcept;
// What the game's own answer was the last time the HUD asked (it asks every frame while it's up), false when it hasn't
// asked lately (the HUD isn't there: a menu, a loading screen)
[[nodiscard]] bool WouldShow(Piece aPiece) noexcept;

// The bar of the one aimed at or last hit (HUDEnemyHealthDisplayEvent)
struct EnemyHealth
{
    bool Valid{false};
    std::string Name;
    // 0..100
    float Health{0.f};
    // the second bar (VATS: what the shot would take), 0..100
    float Optional{0.f};
    bool Hostile{false};
    bool Legendary{false};
    bool Skull{false};
};
[[nodiscard]] EnemyHealth GetEnemyHealth() noexcept;

// PlayerAmmoCountEvent's two numbers and HUDExplosiveAmmoCounterData's, as the game sent them (which is which: see
// docs/HUD.md)
struct Counts
{
    bool Valid{false};
    uint32_t First{0};
    uint32_t Second{0};
};
[[nodiscard]] Counts GetAmmo() noexcept;
[[nodiscard]] Counts GetExplosives() noexcept;

// A compass marker as the game made it (a 0x38 byte record)
struct CompassMarker
{
    std::array<uint8_t, 0x38> Raw{};
    // the direction from the player, as the compass reads it (+0x18)
    float Heading{0.f};
    // sizes the marker; written on it when 99 or less (+0x20)
    float Distance{0.f};
    // 1 Quest, 2 QuestDoor, 3 PlayerSet, 4 Enemy, 5/7/8/9 Location, 6 EnemyTargeted, 10 Recon (+0x24)
    int32_t Kind{0};
    // quest kinds: 1 below, 2 level, 3 above (+0x28)
    int32_t Height{0};
    // locations: 1 undiscovered, 2 discovered, 3 targeted (+0x2C)
    int32_t State{0};
    // the map marker icon, 0-0x59 (+0x30)
    int32_t Icon{0};
    // the colour type byte (+0x35)
    uint8_t Colour{0};
};
// The compass's markers, copied as the compass copies them (main thread: the copy is made on its scrap heap)
void GetCompassMarkers(std::vector<CompassMarker>& aOut) noexcept;

// The globals near the model the compass reads its heading from (ids 1176552, 896827), for the probe
[[nodiscard]] std::array<float, 2> GetCompassGlobals() noexcept;
} // namespace GameHud

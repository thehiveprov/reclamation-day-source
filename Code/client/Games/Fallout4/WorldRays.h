#pragma once

struct Actor;
struct TESObjectCELL;

// Rays through the physics world of a cell, as the game picks (bhkPickData, TESObjectCELL::Pick), from the game's update
// (UpdateEvent), the way the parkour's ledge rays are cast. Each ray lets go of the physics world it looked in
// (6f764200: left holding it, after 65536 rays the exterior's world was freed while still in use).
namespace WorldRays
{
struct RayHit
{
    bool Hit{false};
    NiPoint3 Point{};
    float NormalZ{0.f};
    uint32_t Layer{0};
    // The 3D hit (TESObjectCELL::Pick's answer, a NiAVObject), only to compare with during the same frame
    const void* pObject{nullptr};
};

// The actor's own collision group with the line of sight layer, as the game looks along a line from it (the actor's own
// body isn't hit)
uint32_t GetLineOfSightFilter(Actor* apActor) noexcept;

// The closest thing on the line for the filter's one collision layer (with the line of sight's, see-through things such
// as glass and chain-link fences aren't hit)
RayHit PickOnce(TESObjectCELL* apCell, uint32_t aFilter, const NiPoint3& acFrom, const NiPoint3& acTo) noexcept;

// Whether a hit's layer is one of a character's
bool IsCharacterLayer(uint32_t aLayer) noexcept;
} // namespace WorldRays

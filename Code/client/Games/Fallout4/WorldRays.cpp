#include <HivemindPCH.h>

#include <WorldRays.h>

#include <Actor.h>

namespace
{
// L_LOS collision layer (hits statics, terrain, props; and characters, told apart by their layers; not see-through
// things: glass, chain-link fences, grates)
constexpr uint32_t kLayerLineOfSight = 41;
constexpr size_t kPickDataSize = 0xE0;
} // namespace

namespace WorldRays
{
uint32_t GetLineOfSightFilter(Actor* apActor) noexcept
{
    using TGetCollisionFilter = uint32_t*(Actor*, uint32_t*);
    POINTER_FALLOUT4(TGetCollisionFilter, s_getCollisionFilter, 1474996);

    uint32_t filter = 0;
    s_getCollisionFilter.Get()(apActor, &filter);
    return (filter & 0xFFFF0000u) | kLayerLineOfSight;
}

RayHit PickOnce(TESObjectCELL* apCell, uint32_t aFilter, const NiPoint3& acFrom, const NiPoint3& acTo) noexcept
{
    using TConstruct = void*(void*);
    using TSetStartEnd = void(void*, const NiPoint3*, const NiPoint3*);
    using TPick = void*(TESObjectCELL*, void*);
    using THasHit = bool(void*);
    using TGetHitFraction = float(void*);
    using TDestruct = void(void*);
    POINTER_FALLOUT4(TConstruct, s_construct, 526784);
    POINTER_FALLOUT4(TSetStartEnd, s_setStartEnd, 747471);
    POINTER_FALLOUT4(TPick, s_pick, 434718);
    POINTER_FALLOUT4(THasHit, s_hasHit, 1181585);
    POINTER_FALLOUT4(TGetHitFraction, s_getHitFraction, 476688);
    // bhkPickData's destructor (true id 109425): the pick keeps a reference to the physics world it looked in (at 0xC0,
    // taken by bhkWorld::PickObject, id 547249), which only this lets go of. Havok counts references in 16 bits: left
    // out, every ray leaked one and after 65536 of them the exterior's world was freed while still in use (the crashes
    // after "Havok: world ... destroyed", 2026-10-13 to 16)
    POINTER_FALLOUT4(TDestruct, s_destruct, 109426);

    alignas(16) uint8_t pickData[kPickDataSize]{};
    s_construct.Get()(pickData);
    *reinterpret_cast<uint32_t*>(pickData + 0x0C) = aFilter;
    s_setStartEnd.Get()(pickData, &acFrom, &acTo);
    const void* pObject = s_pick.Get()(apCell, pickData);

    RayHit hit{};
    if (s_hasHit.Get()(pickData))
    {
        const float fraction = std::clamp(s_getHitFraction.Get()(pickData), 0.f, 1.f);
        hit.Hit = true;
        hit.pObject = pObject;
        hit.Point = NiPoint3(glm::vec3(acFrom) + (glm::vec3(acTo) - glm::vec3(acFrom)) * fraction);
        hit.NormalZ = *reinterpret_cast<const float*>(pickData + 0x78);
        hit.Layer = *reinterpret_cast<const uint32_t*>(pickData + 0xAC) & 0x7F;
    }
    s_destruct.Get()(pickData);
    return hit;
}

bool IsCharacterLayer(uint32_t aLayer) noexcept
{
    return aLayer == 8 || aLayer == 30 || aLayer == 32;
}
} // namespace WorldRays

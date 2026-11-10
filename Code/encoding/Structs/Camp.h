#pragma once

#include <Structs/GameId.h>

using TiltedPhoques::String;
using TiltedPhoques::Vector;

// A player's C.A.M.P. (Fallout 4, like Fallout 76's): one per player, set down anywhere outside settlements with the
// C.A.M.P. device, built like a settlement its owner leads (see SettlementClaim). Its workbench (the module) is a
// settlement object like any other, so every game builds, keeps and removes it as it does the rest.
struct Camp
{
    // Settlement changes and claims know a C.A.M.P. by GameId{kModId, Id}: its module is placed while playing, with a
    // form id of its own in each game
    static constexpr uint32_t kModId = 0xFFFFFFFE;
    // How far from its module a C.A.M.P. can be built (game units, a sphere); C.A.M.P.s keep twice that apart
    static constexpr float kRadius = 2400.f;

    uint32_t Id{};
    String OwnerName{};
    uint32_t ModuleItemId{}; // the module, as a settlement object; 0 while stored
    GameId WorldSpaceId{};
    std::array<float, 3> Position{};
    float Heading{}; // radians, about Z
    // Picked up by its owner: nothing of it stands in the world, the server keeps its layout (as it stood at Position,
    // Heading) and builds it again where it's next set down
    bool Stored{};

    [[nodiscard]] GameId GetWorkshopId() const noexcept { return GameId(kModId, Id); }
    [[nodiscard]] static bool IsCamp(const GameId& acWorkshopId) noexcept { return acWorkshopId.ModId == kModId; }

    bool operator==(const Camp& acRhs) const noexcept
    {
        return Id == acRhs.Id && OwnerName == acRhs.OwnerName && ModuleItemId == acRhs.ModuleItemId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position &&
               Heading == acRhs.Heading && Stored == acRhs.Stored;
    }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};

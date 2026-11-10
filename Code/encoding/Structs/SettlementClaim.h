#pragma once

#include <Structs/GameId.h>

using TiltedPhoques::String;
using TiltedPhoques::Vector;

// A claimed settlement (Fallout 4): its leader, and who else may build or scrap there. Players are known by name, so
// leaders and rights stay with them across sessions (player ids change every time they connect).
struct SettlementClaim
{
    struct Right
    {
        String Name{};
        bool Build{false}; // place and move
        bool Scrap{false}; // scrap and store

        bool operator==(const Right& acRhs) const noexcept { return Name == acRhs.Name && Build == acRhs.Build && Scrap == acRhs.Scrap; }
    };

    GameId WorkshopId{}; // the settlement's workbench
    String Name{};       // as the claimer's game names its location
    String LeaderName{};
    uint32_t LeaderPlayerId{}; // 0 while the leader isn't connected
    Vector<Right> Rights{};

    bool operator==(const SettlementClaim& acRhs) const noexcept
    {
        return WorkshopId == acRhs.WorkshopId && Name == acRhs.Name && LeaderName == acRhs.LeaderName && LeaderPlayerId == acRhs.LeaderPlayerId && Rights == acRhs.Rights;
    }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};

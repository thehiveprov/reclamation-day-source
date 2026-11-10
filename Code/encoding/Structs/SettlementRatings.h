#pragma once

#include <Structs/GameId.h>

using TiltedPhoques::Vector;

// A settlement's ratings as the host's game has them (Fallout 4): the actor values on its workbench that the workshop
// menu and the Pip-Boy show, in the order of WorkshopParent's WorkshopRatings list (the client's kRatingValues), two
// per rating: its base value, then its current value (base with damage taken off).
struct SettlementRatings
{
    // Upper bound on what's read: WorkshopRatings has 45, a mod could add a few
    static constexpr size_t kMaxValues = 256;

    GameId WorkshopId{}; // the settlement's workbench
    Vector<float> Values{};

    bool operator==(const SettlementRatings& acRhs) const noexcept { return WorkshopId == acRhs.WorkshopId && Values == acRhs.Values; }
    bool operator!=(const SettlementRatings& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};

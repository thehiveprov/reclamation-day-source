#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// A settlement object built, moved or scrapped by a player, see RequestWorkshopChange. Sent to everyone (the builder
// too when placed, with its LocalRefId, to learn the item id) and, for the settlement's current state, to players
// joining later.
struct NotifyWorkshopChange final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWorkshopChange;

    NotifyWorkshopChange()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyWorkshopChange& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ChangeType == acRhs.ChangeType && PlayerId == acRhs.PlayerId && WorkshopId == acRhs.WorkshopId && ItemId == acRhs.ItemId &&
               RefId == acRhs.RefId && BaseId == acRhs.BaseId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position &&
               Rotation == acRhs.Rotation && Scale == acRhs.Scale && LocalRefId == acRhs.LocalRefId && ActorServerId == acRhs.ActorServerId &&
               EndItemIds == acRhs.EndItemIds && EndRefIds == acRhs.EndRefIds && WireThickness == acRhs.WireThickness;
    }

    uint8_t ChangeType{}; // RequestWorkshopChange::Type
    uint32_t PlayerId{};  // who did it
    GameId WorkshopId{};
    uint32_t ItemId{};
    GameId RefId{};
    GameId BaseId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    std::array<float, 3> Position{};
    std::array<float, 3> Rotation{};
    float Scale{1.f};
    uint32_t LocalRefId{};
    uint32_t ActorServerId{}; // a turret's character (placed), a scrapped body's or turret's (removed)
    std::array<uint32_t, 2> EndItemIds{}; // a wire: the two objects it connects (see RequestWorkshopChange)
    std::array<GameId, 2> EndRefIds{};
    float WireThickness{};
};

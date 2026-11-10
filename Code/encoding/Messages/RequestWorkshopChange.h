#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// The local player built, moved or scrapped an object at a settlement's workbench (Fallout 4 workshop mode). Every
// other player's game does the same at that settlement, so settlements are shared.
struct RequestWorkshopChange final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWorkshopChange;

    enum Type : uint8_t
    {
        kPlaced,
        kMoved,
        kRemoved,  // scrapped or stored
        kRestored, // the world's own object scrapped earlier, back where it stood (undo): by its RefId
    };

    RequestWorkshopChange()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestWorkshopChange& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ChangeType == acRhs.ChangeType && WorkshopId == acRhs.WorkshopId && ItemId == acRhs.ItemId && RefId == acRhs.RefId &&
               BaseId == acRhs.BaseId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position && Rotation == acRhs.Rotation &&
               Scale == acRhs.Scale && LocalRefId == acRhs.LocalRefId && ActorServerId == acRhs.ActorServerId && EndItemIds == acRhs.EndItemIds &&
               EndRefIds == acRhs.EndRefIds && WireThickness == acRhs.WireThickness;
    }

    uint8_t ChangeType{kPlaced};
    GameId WorkshopId{}; // the settlement's workbench
    // Which object: an object built while connected has an item id from the server; one that comes with the game or a
    // plugin (a car wreck being scrapped) is known by its own id instead
    uint32_t ItemId{};
    GameId RefId{};
    GameId BaseId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    std::array<float, 3> Position{};
    std::array<float, 3> Rotation{};
    float Scale{1.f};
    uint32_t LocalRefId{}; // placed: the object on the builder's side, echoed back with its item id
    // A settlement actor (a turret, synced as a character): placed, its character; removed, a dead body's or a turret's
    // character (removed from the server so it doesn't come back)
    uint32_t ActorServerId{};
    // A wire (placed): the two objects it connects, each by item id or by its own id
    std::array<uint32_t, 2> EndItemIds{};
    std::array<GameId, 2> EndRefIds{};
    float WireThickness{};
};

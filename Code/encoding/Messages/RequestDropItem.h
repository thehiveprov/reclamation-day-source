#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>

// The local player dropped an item (a single item or one stack) into the world. It becomes one shared world item:
// the other players see it at the same spot, and whoever picks it up takes it for everyone.
struct RequestDropItem final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDropItem;

    RequestDropItem()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDropItem& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ActorServerId == acRhs.ActorServerId && Item == acRhs.Item && LocalRefId == acRhs.LocalRefId && CellId == acRhs.CellId &&
               WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position && Rotation == acRhs.Rotation;
    }

    uint32_t ActorServerId{}; // the dropping player's character
    Inventory::Entry Item{};   // what was dropped, with its count and mods
    uint32_t LocalRefId{};    // the dropped reference on the dropper's side, echoed back with the item id
    GameId CellId{};
    GameId WorldSpaceId{};
    std::array<float, 3> Position{};
    std::array<float, 3> Rotation{};
};

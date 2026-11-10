#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>

// A shared world item, see RequestDropItem. Sent to everyone when it is dropped (the dropper too, with its LocalRefId,
// to learn the item id) and to players joining later for the items still lying around.
struct NotifyItemDropped final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyItemDropped;

    NotifyItemDropped()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyItemDropped& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ItemId == acRhs.ItemId && PlayerId == acRhs.PlayerId && ActorServerId == acRhs.ActorServerId && Item == acRhs.Item &&
               LocalRefId == acRhs.LocalRefId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position && Rotation == acRhs.Rotation;
    }

    uint32_t ItemId{};
    uint32_t PlayerId{}; // who dropped it
    uint32_t ActorServerId{};
    Inventory::Entry Item{};
    uint32_t LocalRefId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    std::array<float, 3> Position{};
    std::array<float, 3> Rotation{};
};

#pragma once

#include "Message.h"

#include <Structs/Inventory.h>

// Another player traded with a character this game owns (see RequestActorTrade): sent to its owner only.
struct NotifyActorTrade final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyActorTrade;

    NotifyActorTrade()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyActorTrade& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Changes == acRhs.Changes && Worn == acRhs.Worn;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    Inventory Changes{};
    Inventory Worn{};
};

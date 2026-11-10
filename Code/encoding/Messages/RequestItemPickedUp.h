#pragma once

#include "Message.h"

// A player picked up a shared world item (see RequestDropItem): it is gone for everyone.
struct RequestItemPickedUp final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestItemPickedUp;

    RequestItemPickedUp()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestItemPickedUp& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ItemId == acRhs.ItemId; }

    uint32_t ItemId{};
};

#pragma once

#include "Message.h"

// A shared world item was picked up by another player: remove it here. See RequestItemPickedUp.
struct NotifyItemPickedUp final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyItemPickedUp;

    NotifyItemPickedUp()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyItemPickedUp& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ItemId == acRhs.ItemId; }

    uint32_t ItemId{};
};

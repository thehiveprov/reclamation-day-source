#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// A player's power armor state or frame claim changed, see RequestPowerArmor.
struct NotifyPowerArmor final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPowerArmor;

    enum Action : uint8_t
    {
        kEnter,  // player PlayerId (character ServerId) got into frame FrameId, which is now theirs
        kExit,   // player PlayerId got out of their frame
        kClaim,  // frame FrameId belongs to player PlayerId (sent to players joining later)
        kDenied, // sent back to a player who got into a frame someone else claimed first: get out of it
    };

    NotifyPowerArmor()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPowerArmor& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && PlayerId == acRhs.PlayerId && Type == acRhs.Type && FrameId == acRhs.FrameId; }

    uint32_t ServerId{};
    uint32_t PlayerId{};
    uint8_t Type{};
    GameId FrameId{};
};

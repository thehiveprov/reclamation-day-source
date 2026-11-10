#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// The local player got into (Enter) or out of (Exit) a power armor frame. Frames are claimed like in Fallout 76: the
// first player to get into one owns it, the other players can't use it. While in power armor, Enter is resent now
// and then so copies of the character spawned later on other screens are put into the frame too.
struct RequestPowerArmor final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPowerArmor;

    enum Action : uint8_t
    {
        kEnter,
        kExit,
    };

    RequestPowerArmor()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestPowerArmor& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && Type == acRhs.Type && FrameId == acRhs.FrameId; }

    uint32_t ServerId{}; // the player's character
    uint8_t Type{};
    GameId FrameId{}; // the frame reference, empty if it is a temporary one (not shared between players)
};

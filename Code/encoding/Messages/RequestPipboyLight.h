#pragma once

#include "Message.h"

// The player's Pip-Boy light turned on or off (resent now and then while it's on, for copies of the character spawned
// since on other screens): the others light their copy of the character.
struct RequestPipboyLight final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPipboyLight;

    RequestPipboyLight()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestPipboyLight& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && On == acRhs.On; }

    uint32_t ServerId{}; // the player's character
    bool On{};
};

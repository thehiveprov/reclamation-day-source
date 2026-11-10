#pragma once

#include "Message.h"

// Another player's Pip-Boy light turned on or off (see RequestPipboyLight).
struct NotifyPipboyLight final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPipboyLight;

    NotifyPipboyLight()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPipboyLight& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && On == acRhs.On; }

    uint32_t ServerId{}; // the player's character
    bool On{};
};

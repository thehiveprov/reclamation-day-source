#pragma once

#include "Message.h"
#include <Structs/MovementRules.h>

// The host's game sends its movement and parkour rules (RD's MCM page Movement), as it does its difficulty: only the
// host's (an admin session) is taken, and the server sends them to everyone in its settings.
struct RequestMovementRules final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestMovementRules;

    RequestMovementRules()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestMovementRules& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Rules == acRhs.Rules; }

    MovementRules Rules{};
};

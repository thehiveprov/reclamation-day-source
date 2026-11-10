#pragma once

#include "Message.h"

// The host's game asks the server to play at its difficulty (0 Very Easy to 5 Survival): set from the host's pause menu,
// as weather and time are the host's. Only the host's (an admin session) is taken; the server then sends its settings.
struct RequestDifficulty final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDifficulty;

    RequestDifficulty()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDifficulty& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Difficulty == acRhs.Difficulty; }

    uint8_t Difficulty{};
};

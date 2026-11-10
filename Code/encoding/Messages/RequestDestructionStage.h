#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// A destructible object's stage went up in the player's game (a car on fire, a barrel blown up): the players nearby
// bring their copy down to the same health, which runs that stage in their game too.
struct RequestDestructionStage final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDestructionStage;

    RequestDestructionStage()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDestructionStage& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ObjectId == acRhs.ObjectId && Stage == acRhs.Stage && Health == acRhs.Health;
    }

    GameId ObjectId{};
    uint32_t Stage{};   // the stage it reached
    float Health{};     // its health after that
};

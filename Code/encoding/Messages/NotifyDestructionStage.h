#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// A destructible object's stage went up in a nearby player's game (see RequestDestructionStage).
struct NotifyDestructionStage final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDestructionStage;

    NotifyDestructionStage()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDestructionStage& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && PlayerId == acRhs.PlayerId && ObjectId == acRhs.ObjectId && Stage == acRhs.Stage && Health == acRhs.Health;
    }

    uint32_t PlayerId{}; // whose game it happened in
    GameId ObjectId{};
    uint32_t Stage{};
    float Health{};
};

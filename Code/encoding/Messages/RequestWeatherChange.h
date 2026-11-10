#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// Where the player is and the weather there: sent when either changes (worldspace empty: inside, no weather)
struct RequestWeatherChange final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWeatherChange;

    RequestWeatherChange()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestWeatherChange& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && WorldSpaceId == acRhs.WorldSpaceId && Id == acRhs.Id; }

    GameId WorldSpaceId{};
    GameId Id{};
};

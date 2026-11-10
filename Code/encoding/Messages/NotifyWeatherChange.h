#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// The weather of a worldspace. Lead: the receiver decides it (its game's own weather runs and is shared), otherwise
// the receiver shows Id.
struct NotifyWeatherChange final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWeatherChange;

    NotifyWeatherChange()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyWeatherChange& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && WorldSpaceId == acRhs.WorldSpaceId && Id == acRhs.Id && Lead == acRhs.Lead;
    }

    GameId WorldSpaceId{};
    GameId Id{};
    bool Lead = false;
};

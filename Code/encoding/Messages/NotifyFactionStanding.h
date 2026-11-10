#pragma once

#include "Message.h"
#include <Structs/FactionStanding.h>

// Another player's standing with the factions (see RequestFactionStanding).
struct NotifyFactionStanding final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyFactionStanding;

    NotifyFactionStanding()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyFactionStanding& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && PlayerId == acRhs.PlayerId && Standing == acRhs.Standing; }

    uint32_t PlayerId{};
    FactionStanding Standing{};
};

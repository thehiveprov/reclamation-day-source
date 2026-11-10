#pragma once

#include "Message.h"
#include <Structs/FactionStanding.h>

// The local player's standing with the factions, when it changes and on connecting (docs/FACTIONS.md).
struct RequestFactionStanding final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestFactionStanding;

    RequestFactionStanding()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestFactionStanding& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Standing == acRhs.Standing; }

    FactionStanding Standing{};
};

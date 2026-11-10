#pragma once

#include "Message.h"
#include <Structs/SharedQuest.h>

// The local player got one of a faction's radiant quests: for the party members in the same faction (docs/FACTIONS.md).
struct RequestSharedQuest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSharedQuest;

    RequestSharedQuest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestSharedQuest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Quest == acRhs.Quest; }

    SharedQuest Quest{};
};

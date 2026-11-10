#pragma once

#include "Message.h"
#include <Structs/SharedQuest.h>

// A party member in the same faction got one of its radiant quests: start it with the same fills (see RequestSharedQuest).
struct NotifySharedQuest final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySharedQuest;

    NotifySharedQuest()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifySharedQuest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && StarterId == acRhs.StarterId && Quest == acRhs.Quest; }

    uint32_t StarterId{}; // the player who got it
    SharedQuest Quest{};
};

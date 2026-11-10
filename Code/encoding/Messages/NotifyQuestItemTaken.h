#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// A player we're teamed up with on the item's quest took a quest item; remove it from our copy. See RequestQuestItemTaken.
struct NotifyQuestItemTaken final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestItemTaken;

    NotifyQuestItemTaken()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyQuestItemTaken& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && SourceId == acRhs.SourceId && ItemId == acRhs.ItemId && Count == acRhs.Count && QuestId == acRhs.QuestId; }

    GameId SourceId{};
    GameId ItemId{};
    uint32_t Count{};
    GameId QuestId{};
};

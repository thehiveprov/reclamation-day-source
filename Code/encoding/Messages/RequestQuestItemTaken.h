#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// A player took a quest item: out of a container or off a body (loot in those is per player), or from the world (SourceId
// is then the item's own reference). Quest progress is per player too, but players teamed up on the item's quest share
// it, so the item must only exist once among them: their copies of that container/body/world lose it.
struct RequestQuestItemTaken final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestItemTaken;

    RequestQuestItemTaken()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestQuestItemTaken& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && SourceId == acRhs.SourceId && ItemId == acRhs.ItemId && Count == acRhs.Count && QuestId == acRhs.QuestId; }

    GameId SourceId{}; // the container or actor reference the item was taken from, or the item's reference
    GameId ItemId{};
    uint32_t Count{};
    GameId QuestId{}; // the quest the item is for: the players teamed up on it lose it
};

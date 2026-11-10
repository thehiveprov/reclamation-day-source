#include <Messages/NotifyQuestItemTaken.h>

void NotifyQuestItemTaken::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SourceId.Serialize(aWriter);
    ItemId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Count);
    QuestId.Serialize(aWriter);
}

void NotifyQuestItemTaken::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    SourceId.Deserialize(aReader);
    ItemId.Deserialize(aReader);
    Count = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    QuestId.Deserialize(aReader);
}

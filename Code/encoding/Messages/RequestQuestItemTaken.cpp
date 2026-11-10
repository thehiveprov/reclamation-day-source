#include <Messages/RequestQuestItemTaken.h>

void RequestQuestItemTaken::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SourceId.Serialize(aWriter);
    ItemId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Count);
    QuestId.Serialize(aWriter);
}

void RequestQuestItemTaken::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    SourceId.Deserialize(aReader);
    ItemId.Deserialize(aReader);
    Count = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    QuestId.Deserialize(aReader);
}

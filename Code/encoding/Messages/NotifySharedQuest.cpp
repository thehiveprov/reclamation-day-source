#include <Messages/NotifySharedQuest.h>

void NotifySharedQuest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, StarterId);
    Quest.Serialize(aWriter);
}

void NotifySharedQuest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    StarterId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Quest.Deserialize(aReader);
}

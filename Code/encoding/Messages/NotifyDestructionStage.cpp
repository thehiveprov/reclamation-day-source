#include <Messages/NotifyDestructionStage.h>

void NotifyDestructionStage::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, PlayerId);
    ObjectId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Stage);
    Serialization::WriteFloat(aWriter, Health);
}

void NotifyDestructionStage::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ObjectId.Deserialize(aReader);
    Stage = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Health = Serialization::ReadFloat(aReader);
}

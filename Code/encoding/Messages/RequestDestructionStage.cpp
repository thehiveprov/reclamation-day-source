#include <Messages/RequestDestructionStage.h>

void RequestDestructionStage::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    ObjectId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Stage);
    Serialization::WriteFloat(aWriter, Health);
}

void RequestDestructionStage::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ObjectId.Deserialize(aReader);
    Stage = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Health = Serialization::ReadFloat(aReader);
}

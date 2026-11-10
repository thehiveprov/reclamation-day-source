#include <Messages/NotifyPowerArmor.h>

void NotifyPowerArmor::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, PlayerId);
    aWriter.WriteBits(Type, 8);
    FrameId.Serialize(aWriter);
}

void NotifyPowerArmor::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Type = static_cast<uint8_t>(value);
    FrameId.Deserialize(aReader);
}

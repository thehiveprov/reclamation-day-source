#include <Messages/RequestPowerArmor.h>

void RequestPowerArmor::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(Type, 8);
    FrameId.Serialize(aWriter);
}

void RequestPowerArmor::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Type = static_cast<uint8_t>(value);
    FrameId.Deserialize(aReader);
}

#include <Messages/RequestCamp.h>

void RequestCamp::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    Serialization::WriteVarInt(aWriter, CampId);
    ModuleBaseId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    for (const float value : Position)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteFloat(aWriter, Heading);
}

void RequestCamp::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Action = static_cast<uint8_t>(value);
    CampId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ModuleBaseId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    for (float& position : Position)
        position = Serialization::ReadFloat(aReader);
    Heading = Serialization::ReadFloat(aReader);
}

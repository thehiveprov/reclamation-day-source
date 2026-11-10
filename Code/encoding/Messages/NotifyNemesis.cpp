#include <Messages/NotifyNemesis.h>

void NotifyNemesis::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    aWriter.WriteBits(Kind, 8);
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(Value));
    Serialization::WriteVarInt(aWriter, RecordId);
    Serialization::WriteVarInt(aWriter, ActorServerId);
    Serialization::WriteString(aWriter, Text);
    Serialization::WriteString(aWriter, Text2);
    if (Action == kSpawn)
        Spawn.Serialize(aWriter);
}

void NotifyNemesis::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Action = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Kind = static_cast<uint8_t>(value);
    Value = static_cast<int32_t>(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
    RecordId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ActorServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Text = Serialization::ReadString(aReader);
    Text2 = Serialization::ReadString(aReader);
    if (Action == kSpawn)
        Spawn.Deserialize(aReader);
}

#include <Messages/RequestNemesis.h>

void RequestNemesis::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    aWriter.WriteBits(Kind, 8);
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(Value));
    Serialization::WriteVarInt(aWriter, RecordId);
    Serialization::WriteVarInt(aWriter, ActorServerId);
    aWriter.WriteBits(Body, 8);
    Serialization::WriteString(aWriter, World);
    for (const float value : Position)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteString(aWriter, Text);
    Serialization::WriteString(aWriter, Text2);
    Serialization::WriteVarInt(aWriter, ServerIds.size());
    for (const uint32_t id : ServerIds)
        Serialization::WriteVarInt(aWriter, id);
    Serialization::WriteString(aWriter, Appearance);
    Serialization::WriteVarInt(aWriter, AppearanceFlags);
    Serialization::WriteFloat(aWriter, Range);
    Serialization::WriteVarInt(aWriter, Count);
}

void RequestNemesis::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Action = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Kind = static_cast<uint8_t>(value);
    Value = static_cast<int32_t>(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
    RecordId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ActorServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    aReader.ReadBits(value, 8);
    Body = static_cast<uint8_t>(value);
    World = Serialization::ReadString(aReader);
    for (float& position : Position)
        position = Serialization::ReadFloat(aReader);
    Text = Serialization::ReadString(aReader);
    Text2 = Serialization::ReadString(aReader);
    // An encounter's men
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 16);
    ServerIds.resize(count);
    for (uint32_t& id : ServerIds)
        id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Appearance = Serialization::ReadString(aReader);
    AppearanceFlags = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Range = Serialization::ReadFloat(aReader);
    Count = static_cast<uint16_t>(std::min<uint64_t>(Serialization::ReadVarInt(aReader), 0xFFFF));
}

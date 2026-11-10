#include <Structs/NemesisSpawn.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

void NemesisMan::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, RecordId);
    aWriter.WriteBits(Body, 8);
    Serialization::WriteString(aWriter, Appearance);
    Serialization::WriteVarInt(aWriter, AppearanceFlags);
    Serialization::WriteString(aWriter, Name);
    aWriter.WriteBits(Rank, 8);
    aWriter.WriteBits(Injuries, 8);
    aWriter.WriteBits(static_cast<uint8_t>(Alignment), 8);
    Serialization::WriteBool(aWriter, Legendary);
}

void NemesisMan::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    RecordId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Body = static_cast<uint8_t>(value);
    Appearance = Serialization::ReadString(aReader);
    AppearanceFlags = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Name = Serialization::ReadString(aReader);
    aReader.ReadBits(value, 8);
    Rank = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Injuries = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Alignment = static_cast<int8_t>(static_cast<uint8_t>(value));
    Legendary = Serialization::ReadBool(aReader);
}

void NemesisSpawn::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, EncounterId);
    aWriter.WriteBits(Kind, 8);
    Serialization::WriteString(aWriter, Gang);
    Serialization::WriteString(aWriter, Taunt);
    Serialization::WriteVarInt(aWriter, LocationLocalId);
    Serialization::WriteString(aWriter, LocationPlugin);
    Serialization::WriteFloat(aWriter, Distance);
    Serialization::WriteVarInt(aWriter, Men.size());
    for (const NemesisMan& man : Men)
        man.Serialize(aWriter);
}

void NemesisSpawn::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    EncounterId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Kind = static_cast<uint8_t>(value);
    Gang = Serialization::ReadString(aReader);
    Taunt = Serialization::ReadString(aReader);
    LocationLocalId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    LocationPlugin = Serialization::ReadString(aReader);
    Distance = Serialization::ReadFloat(aReader);
    // A nemesis and at most a handful of men
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 16);
    Men.resize(count);
    for (NemesisMan& man : Men)
        man.Deserialize(aReader);
}

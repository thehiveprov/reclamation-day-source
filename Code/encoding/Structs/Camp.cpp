#include <Structs/Camp.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

void Camp::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Serialization::WriteString(aWriter, OwnerName);
    Serialization::WriteVarInt(aWriter, ModuleItemId);
    WorldSpaceId.Serialize(aWriter);
    for (const float value : Position)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteFloat(aWriter, Heading);
    Serialization::WriteBool(aWriter, Stored);
}

void Camp::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnerName = Serialization::ReadString(aReader);
    ModuleItemId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    WorldSpaceId.Deserialize(aReader);
    for (float& value : Position)
        value = Serialization::ReadFloat(aReader);
    Heading = Serialization::ReadFloat(aReader);
    Stored = Serialization::ReadBool(aReader);
}

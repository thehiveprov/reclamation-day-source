#include <Structs/FactionStanding.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

void FactionStanding::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Members);
    Serialization::WriteVarInt(aWriter, Out);
    Serialization::WriteVarInt(aWriter, Hostile);
    Serialization::WriteVarInt(aWriter, Allied);
}

void FactionStanding::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Members = Serialization::ReadVarInt(aReader) & 0xFF;
    Out = Serialization::ReadVarInt(aReader) & 0xFF;
    Hostile = Serialization::ReadVarInt(aReader) & 0xFF;
    Allied = Serialization::ReadVarInt(aReader) & 0xFF;
}

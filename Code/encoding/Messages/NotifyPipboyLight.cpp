#include <Messages/NotifyPipboyLight.h>

void NotifyPipboyLight::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteBool(aWriter, On);
}

void NotifyPipboyLight::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    On = Serialization::ReadBool(aReader);
}

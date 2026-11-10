#include <Messages/AuthenticationResponse.h>

void AuthenticationResponse::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(Type));
    Serialization::WriteBool(aWriter, SKSEActive);
    Serialization::WriteBool(aWriter, MO2Active);
    Serialization::WriteString(aWriter, Version);
    UserMods.Serialize(aWriter);
    Settings.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, PlayerId);

    Serialization::WriteVarInt(aWriter, HostLoadOrder.size());
    for (const String& plugin : HostLoadOrder)
        Serialization::WriteString(aWriter, plugin);
}

void AuthenticationResponse::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Type = static_cast<ResponseType>(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
    SKSEActive = Serialization::ReadBool(aReader);
    MO2Active = Serialization::ReadBool(aReader);
    Version = Serialization::ReadString(aReader);
    UserMods.Deserialize(aReader);
    Settings.Deserialize(aReader);
    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    const uint64_t pluginCount = Serialization::ReadVarInt(aReader);
    for (uint64_t i = 0; i < pluginCount; i++)
        HostLoadOrder.push_back(Serialization::ReadString(aReader));
}

#include <Messages/NotifyActorTrade.h>

void NotifyActorTrade::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Changes.Serialize(aWriter);
    Worn.Serialize(aWriter);
}

void NotifyActorTrade::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Changes.Deserialize(aReader);
    Worn.Deserialize(aReader);
}

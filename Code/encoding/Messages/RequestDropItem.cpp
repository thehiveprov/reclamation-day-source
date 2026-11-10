#include <Messages/RequestDropItem.h>

void RequestDropItem::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ActorServerId);
    Item.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, LocalRefId);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    for (const float value : Position)
        Serialization::WriteFloat(aWriter, value);
    for (const float value : Rotation)
        Serialization::WriteFloat(aWriter, value);
}

void RequestDropItem::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ActorServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
    LocalRefId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    for (float& value : Position)
        value = Serialization::ReadFloat(aReader);
    for (float& value : Rotation)
        value = Serialization::ReadFloat(aReader);
}

#include <Messages/RequestWorkshopChange.h>

void RequestWorkshopChange::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(ChangeType, 8);
    WorkshopId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, ItemId);
    RefId.Serialize(aWriter);
    BaseId.Serialize(aWriter);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    for (const float value : Position)
        Serialization::WriteFloat(aWriter, value);
    for (const float value : Rotation)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteFloat(aWriter, Scale);
    Serialization::WriteVarInt(aWriter, LocalRefId);
    Serialization::WriteVarInt(aWriter, ActorServerId);
    for (const uint32_t id : EndItemIds)
        Serialization::WriteVarInt(aWriter, id);
    for (const GameId& id : EndRefIds)
        id.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, WireThickness);
}

void RequestWorkshopChange::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t changeType = 0;
    aReader.ReadBits(changeType, 8);
    ChangeType = static_cast<uint8_t>(changeType);
    WorkshopId.Deserialize(aReader);
    ItemId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    RefId.Deserialize(aReader);
    BaseId.Deserialize(aReader);
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    for (float& value : Position)
        value = Serialization::ReadFloat(aReader);
    for (float& value : Rotation)
        value = Serialization::ReadFloat(aReader);
    Scale = Serialization::ReadFloat(aReader);
    LocalRefId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ActorServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    for (uint32_t& id : EndItemIds)
        id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    for (GameId& id : EndRefIds)
        id.Deserialize(aReader);
    WireThickness = Serialization::ReadFloat(aReader);
}

#include <Messages/NotifyEquipmentChanges.h>
#include <TiltedCore/Serialization.hpp>

void NotifyEquipmentChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    ItemId.Serialize(aWriter);
    EquipSlotId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Count);
    Serialization::WriteBool(aWriter, Unequip);
    Serialization::WriteBool(aWriter, IsSpell);
    Serialization::WriteBool(aWriter, IsShout);
    Inventory::ObjectMod::SerializeList(aWriter, ObjectMods);
    Serialization::WriteBool(aWriter, FullSync);
    if (FullSync)
        CurrentEquipment.Serialize(aWriter);
}

void NotifyEquipmentChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ItemId.Deserialize(aReader);
    EquipSlotId.Deserialize(aReader);
    Count = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Unequip = Serialization::ReadBool(aReader);
    IsSpell = Serialization::ReadBool(aReader);
    IsShout = Serialization::ReadBool(aReader);
    Inventory::ObjectMod::DeserializeList(aReader, ObjectMods);
    FullSync = Serialization::ReadBool(aReader);
    if (FullSync)
        CurrentEquipment.Deserialize(aReader);
}

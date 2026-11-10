#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>

struct NotifyEquipmentChanges final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyEquipmentChanges;

    NotifyEquipmentChanges()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyEquipmentChanges& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && ItemId == acRhs.ItemId && EquipSlotId == acRhs.EquipSlotId && Count == acRhs.Count && Unequip == acRhs.Unequip && IsSpell == acRhs.IsSpell &&
               IsShout == acRhs.IsShout && ObjectMods == acRhs.ObjectMods && FullSync == acRhs.FullSync && CurrentEquipment == acRhs.CurrentEquipment;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    GameId ItemId{};
    GameId EquipSlotId{};
    uint32_t Count{};
    bool Unequip = false;
    bool IsSpell = false;
    bool IsShout = false;
    // Fallout 4: OMODs of the equipped instance, so receivers can create/equip the exact same item.
    Vector<Inventory::ObjectMod> ObjectMods{};
    // Fallout 4: CurrentEquipment is the complete worn set, reconcile the actor to it
    bool FullSync = false;
    Inventory CurrentEquipment{};
};

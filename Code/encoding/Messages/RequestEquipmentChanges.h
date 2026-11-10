#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Inventory.h>

struct RequestEquipmentChanges final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestEquipmentChanges;

    RequestEquipmentChanges()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestEquipmentChanges() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestEquipmentChanges& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && ItemId == acRhs.ItemId && EquipSlotId == acRhs.EquipSlotId && Count == acRhs.Count && Unequip == acRhs.Unequip && IsSpell == acRhs.IsSpell &&
               IsShout == acRhs.IsShout && IsAmmo == acRhs.IsAmmo && CurrentInventory == acRhs.CurrentInventory && ObjectMods == acRhs.ObjectMods && FullSync == acRhs.FullSync;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    GameId ItemId{};
    GameId EquipSlotId{};
    uint32_t Count{};
    bool Unequip = false;
    bool IsSpell = false;
    bool IsShout = false;
    bool IsAmmo = false;

    Inventory CurrentInventory{};
    // Fallout 4: OMODs of the stack being (un)equipped
    Vector<Inventory::ObjectMod> ObjectMods{};
    // Fallout 4: no single item changed, CurrentInventory is the complete worn set receivers should match
    bool FullSync = false;
};

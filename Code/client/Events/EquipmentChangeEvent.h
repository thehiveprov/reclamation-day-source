#pragma once

#include <Structs/Inventory.h>

/**
 * @brief Dispatched when a local actor changes its equipment.
 */
struct EquipmentChangeEvent
{
    uint32_t ActorId = 0;
    uint32_t ServerId = 0;
    uint32_t OwnershipEpoch = 0;
    uint32_t ItemId = 0;
    uint32_t EquipSlotId = 0;
    uint32_t Count = 0;
    bool Unequip = false;
    bool IsSpell = false;
    bool IsShout = false;
    bool IsAmmo = false;
    // Fallout 4: OMODs of the stack being (un)equipped
    Vector<Inventory::ObjectMod> ObjectMods{};
};

#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Inventory.h>

struct RemoteComponent
{
    RemoteComponent(uint32_t aId, uint32_t aRefId, uint32_t aOwnershipEpoch) noexcept
        : Id(aId)
        , CachedRefId(aRefId)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    uint32_t Id;
    uint32_t CachedRefId;
    uint32_t OwnershipEpoch;
    // Fallout 4: what the owner wore the last time this copy's equipment was matched to its owner's
    // (InventoryService::ReconcileEquipment), nothing after the copy's inventory was set as a whole
    std::optional<Inventory> AppliedEquipment{};
};

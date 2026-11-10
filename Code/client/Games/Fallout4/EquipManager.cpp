#include <HivemindPCH.h>

#include <EquipManager.h>
#include <Games/References.h>
#include <Games/Overrides.h>
#include <Forms/BGSObjectInstance.h>

#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#include <Utils.h>
#include <Services/WorkshopService.h>
#include <Services/InventoryService.h>

struct BGSEquipSlot;

struct ObjectEquipParams
{
    uint32_t uiStackID;
    uint32_t uiNumber;
    const BGSEquipSlot* pEquipSlot;
    const BGSEquipSlot* pSlotBeingReplaced;
    bool bQueueEquip;
    bool bForceEquip;
    bool bPlayEquipSounds;
    bool bApplyNow;
    bool bLocked;
    bool bExtraWasDeleted;
};

namespace
{
// Only the actor's current owner reports equipment changes, stamped with the ownership it had at the time: the
// server drops changes from a client whose ownership was taken away in the meantime. Nothing is read from the actor
// for one that isn't ours to report (every actor of a game that isn't connected, for one).
void QueueEquipmentChange(Actor* apActor, const BGSObjectInstance& acItem, const ObjectEquipParams& acParams, bool aUnequip)
{
    const auto ownershipToken = Utils::GetLocalOwnershipToken(apActor->formID);
    if (!ownershipToken)
    {
        spdlog::debug("equipment_event_not_queued reason=no_local_ownership actor_form_id={:X}", apActor->formID);
        return;
    }

    EquipmentChangeEvent evt{};
    evt.ActorId = apActor->formID;
    evt.ServerId = ownershipToken->ServerId;
    evt.OwnershipEpoch = ownershipToken->OwnershipEpoch;
    evt.Count = acParams.uiNumber;
    evt.ItemId = acItem.pObject->formID;
    evt.ObjectMods = apActor->GetStackObjectMods(Cast<TESBoundObject>(acItem.pObject), acParams.uiStackID);
    evt.Unequip = aUnequip;

    World::Get().GetRunner().Trigger(std::move(evt));
}
} // namespace

TP_THIS_FUNCTION(TEquip, void*, EquipManager, Actor* apActor, BGSObjectInstance* apItem, ObjectEquipParams& arParams);
TP_THIS_FUNCTION(TUnEquip, void*, EquipManager, Actor* apActor, BGSObjectInstance* apItem, ObjectEquipParams& arParams);

TEquip* RealEquip = nullptr;
TUnEquip* RealUnEquip = nullptr;

EquipManager* EquipManager::Get() noexcept
{
    POINTER_FALLOUT4(EquipManager*, s_singleton, 1174341);

    return *s_singleton.Get();
}

bool EquipManager::EquipObject(Actor* apActor, BGSObjectInstance& arObject, uint32_t auiStackID, uint32_t auiNumber, const BGSEquipSlot* apSlot, bool abQueueEquip, bool abForceEquip, bool abPlaySounds, bool abApplyNow, bool abLocked)
{
    TP_THIS_FUNCTION(TEquipObject, bool, EquipManager, Actor* apActor, BGSObjectInstance& arObject, uint32_t auiStackID, uint32_t auiNumber, const BGSEquipSlot* apSlot, bool abQueueEquip, bool abForceEquip, bool abPlaySounds, bool abApplyNow, bool abLocked);
    POINTER_FALLOUT4(TEquipObject, equipObject, 988030);

    ScopedEquipOverride _;

    return TiltedPhoques::ThisCall(equipObject, this, apActor, arObject, auiStackID, auiNumber, apSlot, abQueueEquip, abForceEquip, abPlaySounds, abApplyNow, abLocked);
}

bool EquipManager::UnequipObject(Actor* apActor, BGSObjectInstance& arObject, uint32_t auiNumber, const BGSEquipSlot* apSlot, uint32_t auiStackID, bool abQueueEquip, bool abForceEquip, bool abPlaySounds, bool abApplyNow, const BGSEquipSlot* apSlotBeingReplaced)
{
    // The stack says which of the actor's items of this form comes off (see Actor::UnequipItem)
    TP_THIS_FUNCTION(TUnequipObject, bool, EquipManager, Actor* apActor, BGSObjectInstance& arObject, uint32_t auiNumber, const BGSEquipSlot* apSlot, uint32_t auiStackID, bool abQueueEquip, bool abForceEquip, bool abPlaySounds, bool abApplyNow, const BGSEquipSlot* apSlotBeingReplaced);
    POINTER_FALLOUT4(TUnequipObject, unequipObject, 1292494);

    ScopedEquipOverride _;

    return TiltedPhoques::ThisCall(unequipObject, this, apActor, arObject, auiNumber, apSlot, auiStackID, abQueueEquip, abForceEquip, abPlaySounds, abApplyNow, apSlotBeingReplaced);
}

void* TP_MAKE_THISCALL(EquipHook, EquipManager, Actor* apActor, BGSObjectInstance* apItem, ObjectEquipParams& arParams)
{
    if (!apActor)
        return nullptr;

    const auto pExtension = apActor->GetExtension();
    // What another player's character wears is its owner's to decide: the game's own equips on it are dropped. Not
    // what the local player gives it to wear in the trade menu: its owner makes the same trade (RunTradeUpdates).
    if (pExtension->IsRemote() && !ScopedEquipOverride::IsOverriden() && apActor->formID != InventoryService::GetTradePartnerId())
    {
        spdlog::debug("Actor[{:X}]::Equip(), item form id: {:X}", apActor->formID, apItem->pObject->formID);
        return nullptr;
    }

    // The C.A.M.P. device (RD_Camps.esp) used from the Pip-Boy: sets the player's C.A.M.P. down there instead, and
    // isn't used up
    if (apActor->formID == 0x14 && apItem && apItem->pObject && WorkshopService::GetCampDeviceId() && apItem->pObject->formID == WorkshopService::GetCampDeviceId())
    {
        World::Get().GetRunner().Queue([]() { World::Get().ctx().at<WorkshopService>().OnCampDeviceUsed(); });
        return nullptr;
    }

    if (pExtension->IsLocal())
        QueueEquipmentChange(apActor, *apItem, arParams, false);

    ScopedUnequipOverride _;

    return TiltedPhoques::ThisCall(RealEquip, apThis, apActor, apItem, arParams);
}

void* TP_MAKE_THISCALL(UnEquipHook, EquipManager, Actor* apActor, BGSObjectInstance* apItem, ObjectEquipParams& arParams)
{
    if (!apActor)
        return nullptr;

    const auto pExtension = apActor->GetExtension();
    if (pExtension->IsRemote() && !ScopedEquipOverride::IsOverriden() && apActor->formID != InventoryService::GetTradePartnerId())
        return nullptr;

    if (pExtension->IsLocal())
        QueueEquipmentChange(apActor, *apItem, arParams, true);

    return TiltedPhoques::ThisCall(RealUnEquip, apThis, apActor, apItem, arParams);
}

static TiltedPhoques::Initializer s_equipmentHooks(
    []()
    {
        POINTER_FALLOUT4(TEquip, s_equipFunc, 1474879);
        POINTER_FALLOUT4(TUnEquip, s_unequipFunc, 1265293);

        RealUnEquip = s_unequipFunc.Get();
        RealEquip = s_equipFunc.Get();

        TP_HOOK(&RealUnEquip, UnEquipHook);
        TP_HOOK(&RealEquip, EquipHook);
    });

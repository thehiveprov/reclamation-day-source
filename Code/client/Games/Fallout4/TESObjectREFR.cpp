#include <HivemindPCH.h>
#include <Games/References.h>
#include <World.h>

#include <Events/ActivateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/DropItemEvent.h>

#include <Forms/TESBoundObject.h>
#include <Forms/TESObjectARMO.h>
#include <Forms/TESObjectWEAP.h>
#include <Forms/BGSObjectInstance.h>
#include <ExtraData/BGSObjectInstanceExtra.h>
#include <ExtraData/ExtraDataList.h>

#include <EquipManager.h>

#include <Games/Overrides.h>
#include <Services/PapyrusService.h>
#include <Services/CompanionService.h>
#include <Services/NemesisService.h>
#include <Services/BetterInteractService.h>

TP_THIS_FUNCTION(TActivate, bool, TESObjectREFR, TESObjectREFR* apActivator, TESBoundObject* apObjectToGet, int32_t aCount, bool aDefaultProcessing, bool aFromScript, bool aIsLooping);
TP_THIS_FUNCTION(TAddInventoryItem, void, TESObjectREFR, TESBoundObject* apObject, ExtraDataList* apExtraData, uint32_t aCount, TESObjectREFR* apOldContainer, void* apUnk1, void* apUnk2);
TP_THIS_FUNCTION(TRemoveInventoryItem, uint32_t*, TESObjectREFR, uint32_t* apUnk1, void* apUnk2);

static TActivate* RealActivate = nullptr;
static TAddInventoryItem* RealAddInventoryItem = nullptr;
static TRemoveInventoryItem* RealRemoveInventoryItem = nullptr;

namespace
{
// Copies the OMODs attached to an item instance into the network entry, as plugin-relative ids so that
// attachments added by mods work as long as everyone has the same load order; and the name a player gave it.
void ReadObjectMods(Inventory::Entry& arEntry, ExtraDataList* apExtraData) noexcept
{
    if (!apExtraData)
        return;

    arEntry.CustomName = apExtraData->GetCustomName();

    auto* pInstanceExtra = static_cast<BGSObjectInstanceExtra*>(apExtraData->GetByType(ExtraDataType::ObjectInstance));
    if (!pInstanceExtra)
        return;

    auto& modSystem = World::Get().GetModSystem();

    for (const auto& data : pInstanceExtra->GetIndexData())
    {
        if (data.disabled)
            continue;

        Inventory::ObjectMod mod{};
        if (!modSystem.GetServerModId(data.objectID, mod.ModId))
        {
            // Said once per mod: worn items are read over and over
            static TiltedPhoques::Set<uint32_t> s_reported;
            if (s_reported.insert(data.objectID).second)
                spdlog::warn("{}: object mod {:X} has no server id (a plugin the server doesn't have?), skipping it", __FUNCTION__, data.objectID);
            continue;
        }

        mod.AttachIndex = data.index;
        mod.Rank = data.rank;
        arEntry.ObjectMods.push_back(mod);
    }

    // Keep the game's order: a mod's attach index refers to the position of the mod it attaches to in this list, so
    // reordering it misattached parts on the receiving side (extra attachments, or an invisible weapon).
    // Comparisons are order independent instead (Inventory::ObjectMod::SameMods).
}

// Builds the instance extra for an entry's OMODs, or nullptr when the entry carries none.
BGSObjectInstanceExtra* CreateObjectInstanceExtra(const Inventory::Entry& acEntry) noexcept
{
    if (acEntry.ObjectMods.empty())
        return nullptr;

    auto& modSystem = World::Get().GetModSystem();
    BGSObjectInstanceExtra* pInstanceExtra = BGSObjectInstanceExtra::New();

    spdlog::debug("Building instance of {:X} with {} object mods", modSystem.GetGameId(acEntry.BaseId), acEntry.ObjectMods.size());

    for (const Inventory::ObjectMod& mod : acEntry.ObjectMods)
    {
        TESForm* pMod = TESForm::GetById(modSystem.GetGameId(mod.ModId));
        spdlog::debug("  object mod {:X} (form {}), attach index {}, rank {}", modSystem.GetGameId(mod.ModId), fmt::ptr(pMod), mod.AttachIndex, mod.Rank);
        if (!pMod)
        {
            spdlog::warn("{}: object mod {:X}:{:X} not found, is the load order the same?", __FUNCTION__, mod.ModId.ModId, mod.ModId.BaseId);
            continue;
        }

        pInstanceExtra->AddMod(pMod, mod.AttachIndex, mod.Rank);
    }

    return pInstanceExtra;
}
} // namespace

Vector<Inventory::ObjectMod> TESObjectREFR::GetStackObjectMods(TESBoundObject* apObject, uint32_t aStackIndex) const noexcept
{
    if (!pInventoryList || !apObject)
        return {};

    for (auto& item : pInventoryList->DataA)
    {
        if (item.pObject != apObject)
            continue;

        uint32_t index = 0;
        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack, ++index)
        {
            if (index != aStackIndex)
                continue;

            Inventory::Entry stackEntry{};
            ReadObjectMods(stackEntry, pStack->spExtra);
            return stackEntry.ObjectMods;
        }
    }

    return {};
}

TiltedPhoques::Map<uint32_t, TESObjectREFR::QuestItem> TESObjectREFR::GetQuestItems() const noexcept
{
    TiltedPhoques::Map<uint32_t, QuestItem> items;
    if (!pInventoryList)
        return items;

    for (auto& item : pInventoryList->DataA)
    {
        if (!item.pObject)
            continue;

        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack)
        {
            if (!pStack->spExtra || !pStack->spExtra->Contains(ExtraDataType::AliasInstanceArray))
                continue;

            QuestItem& questItem = items[item.pObject->formID];
            questItem.Count += pStack->uiCount;
            for (const uint32_t questId : pStack->spExtra->GetAliasQuests())
            {
                if (std::find(questItem.Quests.begin(), questItem.Quests.end(), questId) == questItem.Quests.end())
                    questItem.Quests.push_back(questId);
            }
        }
    }

    return items;
}

std::optional<uint32_t> TESObjectREFR::FindStackIndex(TESBoundObject* apObject, const Inventory::Entry& acEntry) const noexcept
{
    if (!pInventoryList)
        return std::nullopt;

    for (auto& item : pInventoryList->DataA)
    {
        if (item.pObject != apObject)
            continue;

        uint32_t index = 0;
        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack, ++index)
        {
            Inventory::Entry stackEntry{};
            ReadObjectMods(stackEntry, pStack->spExtra);

            if (Inventory::ObjectMod::SameMods(stackEntry.ObjectMods, acEntry.ObjectMods))
                return index;
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> TESObjectREFR::FindWornStackIndex(TESBoundObject* apObject) const noexcept
{
    if (!pInventoryList)
        return std::nullopt;

    for (auto& item : pInventoryList->DataA)
    {
        if (item.pObject != apObject)
            continue;

        uint32_t index = 0;
        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack, ++index)
        {
            if (pStack->IsEquipped())
                return index;
        }
    }

    return std::nullopt;
}

Vector<TESObjectREFR::StackState> TESObjectREFR::GetStacks(TESBoundObject* apObject) const noexcept
{
    Vector<StackState> stacks;
    if (!pInventoryList)
        return stacks;

    for (auto& item : pInventoryList->DataA)
    {
        if (item.pObject != apObject)
            continue;

        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack)
            stacks.push_back({pStack, pStack->uiCount});
    }

    return stacks;
}

Inventory TESObjectREFR::GetInventory() const noexcept
{
    // TODO: inventory list has RWLock, should probably use?

    Inventory inventory{};
    if (!pInventoryList)
        return inventory;

    auto& modSystem = World::Get().GetModSystem();

    for (auto& item : pInventoryList->DataA)
    {
        if (!item.pObject || !item.spStackData)
            continue;

        GameId baseId{};
        modSystem.GetServerModId(item.pObject->formID, baseId);

        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack)
        {
            Inventory::Entry entry{};
            entry.BaseId = baseId;
            entry.Count = pStack->uiCount;

            // If an item with multiple copies is equipped, they will be in the same stack.
            // The stacks only make distinctions between modified items.
            entry.ExtraWorn = pStack->IsEquipped();

            ReadObjectMods(entry, pStack->spExtra);

            inventory.Entries.push_back(std::move(entry));
        }
    }

    return inventory;
}

void TESObjectREFR::SetInventory(const Inventory& aInventory) noexcept
{
    ScopedInventoryOverride _;

    RemoveAllItems();

    for (const Inventory::Entry& entry : aInventory.Entries)
    {
        if (entry.Count != 0)
            AddOrRemoveItem(entry);
    }
}

void TESObjectREFR::RemoveAllItems() noexcept
{
    using TRemoveAllItems = void(void*, void*, TESObjectREFR*, TESObjectREFR*, bool);
    POINTER_FALLOUT4(TRemoveAllItems, s_removeAllItems, 534059);
    s_removeAllItems(nullptr, nullptr, this, nullptr, false);
}

void TESObjectREFR::MoveAllItemsTo(TESObjectREFR* apTarget) noexcept
{
    // The same native as RemoveAllItems, with a reference to transfer to
    using TRemoveAllItems = void(void*, void*, TESObjectREFR*, TESObjectREFR*, bool);
    POINTER_FALLOUT4(TRemoveAllItems, s_removeAllItems, 534059);
    s_removeAllItems(nullptr, nullptr, this, apTarget, false);
}

void TESObjectREFR::AddOrRemoveItem(const Inventory::Entry& arEntry) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("{}: Object to add not found, {:X}:{:X}.", __FUNCTION__, arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        return;
    }

    if (arEntry.Count > 0)
    {
        constexpr int32_t kNoReason = 0; // ITEM_REMOVE_REASON::kNone

        BGSObjectInstanceExtra* pInstanceExtra = CreateObjectInstanceExtra(arEntry);
        if (pInstanceExtra || !arEntry.CustomName.empty())
        {
            ExtraDataList* pList = ExtraDataList::New();
            if (pInstanceExtra)
            {
                // Without this the game builds the instance from the base form's default template,
                // e.g. a remote player's pipe pistol showing up with a suppressor it never had.
                pList->Add(ExtraDataType::ObjectInstance, pInstanceExtra);
                // What these mods make of the item: which of an armor piece's models it has (leather, raider, metal...
                // limbs have none without their size mod, so they were worn but not drawn), its material and paint, a
                // weapon's grip and sound. The game only works this out for an item it gives its own mods.
                pList->CreateInstanceData(pObject);
            }
            // A player's own name for it, over the one the mods give it
            if (!arEntry.CustomName.empty())
                pList->SetCustomName(arEntry.CustomName.c_str());

            // The reference held by the smart pointer argument, released by the game when it's done with it; the
            // inventory stack keeps its own
            pList->refCount = 1;
            ExtraDataListHandle handle{pList};
            AddObjectToContainer(pObject, &handle, arEntry.Count, nullptr, kNoReason);
        }
        else
        {
            ExtraDataListHandle handle{nullptr};
            AddObjectToContainer(pObject, &handle, arEntry.Count, nullptr, kNoReason);
        }

        if (arEntry.IsWorn())
        {
            // The stack that has these mods, not just the first stack of the base object
            if (Actor* pActor = Cast<Actor>(this))
                pActor->EquipItem(pObject, FindStackIndex(pObject, arEntry).value_or(0), arEntry.Count);
        }
    }
    else if (arEntry.Count < 0)
    {
        RemoveItemData data{};
        data.pObject = pObject;
        // Entry counts are negative for removals, the game wants the number of items to remove
        data.iNumber = -arEntry.Count;
        // TODO(cosideci): is NONE valid?
        data.eReason = ITEM_REMOVE_REASON::IRR_NONE;
        data.pOtherContainer = nullptr;
        data.pDropLoc = nullptr;
        data.pRotate = nullptr;

        RemoveItem(&data);
    }
}

ActorValueInfo* TESObjectREFR::GetActorValueInfo(uint32_t aId) const noexcept
{
    using TGetActorValueInfoArray = ActorValueInfo**();

    POINTER_FALLOUT4(TGetActorValueInfoArray, s_getActorValueInfoArray, 405391);

    ActorValueInfo** actorValueInfoArray = s_getActorValueInfoArray.Get()();

    return actorValueInfoArray[aId];
}

const BGSEquipSlot* TESObjectREFR::GetEquipSlot(uint32_t aEquipIndex) const noexcept
{
    TP_THIS_FUNCTION(TGetEquipSlot, const BGSEquipSlot*, const TESObjectREFR, uint32_t);

    POINTER_FALLOUT4(TGetEquipSlot, getEquipSlot, 180924);

    return TiltedPhoques::ThisCall(getEquipSlot, this, aEquipIndex);
}

bool TESObjectREFR::Activate(TESObjectREFR* apActivator, TESBoundObject* apObjectToGet, int32_t aCount, bool aDefaultProcessing, bool aFromScript, bool aIsLooping) noexcept
{
    return TiltedPhoques::ThisCall(RealActivate, this, apActivator, apObjectToGet, aCount, aDefaultProcessing, aFromScript, aIsLooping);
}

bool TP_MAKE_THISCALL(HookActivate, TESObjectREFR, TESObjectREFR* apActivator, TESBoundObject* apObjectToGet, int aCount, bool aDefaultProcessing, bool aFromScript, bool aIsLooping)
{
    auto* pActivator = Cast<Actor>(apActivator);
    // A dog to adopt (or already someone's): handled there instead of the game's talk
    if (World::Get().ctx().at<CompanionService>().OnActivate(apThis, pActivator))
        return true;
    // A surrendered gang man: his questioning card instead of the game's talk
    if (World::Get().ctx().at<NemesisService>().OnActivate(apThis, pActivator))
        return true;

    // Remote characters replaying another player's activation must not send it back
    if (pActivator && !pActivator->GetExtension()->IsRemote())
        World::Get().GetRunner().Trigger(ActivateEvent(apThis, pActivator, apObjectToGet, aCount, aDefaultProcessing, aFromScript, aIsLooping));

    // Better Interact's hand only looks on: before the game's own (a picked up item's model is still there), after it
    auto& betterInteract = World::Get().ctx().at<BetterInteractService>();
    betterInteract.BeforeActivate(apThis, apActivator, aFromScript, aIsLooping);
    const bool activated = TiltedPhoques::ThisCall(RealActivate, apThis, apActivator, apObjectToGet, aCount, aDefaultProcessing, aFromScript, aIsLooping);
    betterInteract.AfterActivate(activated);
    return activated;
}

void TP_MAKE_THISCALL(HookAddInventoryItem, TESObjectREFR, TESBoundObject* apObject, ExtraDataList* apExtraData, uint32_t aCount, TESObjectREFR* apOldContainer, void* apUnk1, void* apUnk2)
{
    // Not used on Fallout on purpose: container/body loot is per player and worn equipment is synced by state
    // (InventoryService::RunEquipmentStateUpdates), so individual inventory changes aren't sent.
#if TP_SKYRIM64
    World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID));
#endif
    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apObject, apExtraData, aCount, apOldContainer, apUnk1, apUnk2);
}

uint32_t* TP_MAKE_THISCALL(HookRemoveInventoryItem, TESObjectREFR, uint32_t* apUnk1, void* apUnk2)
{
    // Not used on Fallout on purpose, see HookAddInventoryItem.
#if TP_SKYRIM64
    World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID));
#endif
    return TiltedPhoques::ThisCall(RealRemoveInventoryItem, apThis, apUnk1, apUnk2);
}

// TESObjectREFR::RemoveItemData (alandtse/CommonLibF4), 0x48 bytes
struct RemoveItemData
{
    uint8_t stackData[0x20]; // BSTSmallArray<uint32_t, 4>
    TESBoundObject* object;  // 20
    int32_t count;           // 28
    int32_t reason;          // 2C, ITEM_REMOVE_REASON: 3 = dropping
    TESObjectREFR* otherContainer; // 30
    const NiPoint3* dropLoc; // 38
    const NiPoint3* rotate;  // 40
};
static_assert(sizeof(RemoveItemData) == 0x48);

// Actor's RemoveItem (vtable slot 0x6D of Actor and PlayerCharacter, CommonLibF4 id 522078). Member function returning
// an ObjectRefHandle: MSVC passes the result pointer after this.
TP_THIS_FUNCTION(TActorRemoveItem, uint32_t*, TESObjectREFR, uint32_t* apResult, RemoveItemData& arData);
static TActorRemoveItem* RealActorRemoveItem = nullptr;

uint32_t* TP_MAKE_THISCALL(HookActorRemoveItem, TESObjectREFR, uint32_t* apResult, RemoveItemData& arData)
{
    // Read before the call: the game counts the data down while removing the items (drops arrived with a count of 0)
    const int32_t count = arData.count;
    const int32_t reason = arData.reason;
    const bool hasDropLocation = arData.dropLoc != nullptr;
    const uint32_t objectId = arData.object ? arData.object->formID : 0;

    uint32_t* pResult = TiltedPhoques::ThisCall(RealActorRemoveItem, apThis, apResult, arData);

    if (apThis->formID != 0x14)
        return pResult;

    const uint32_t handle = pResult ? *pResult : 0;
    spdlog::info("Player RemoveItem {:X} x{}, reason {}, drop location {}, result handle {:X}", objectId, count, reason, hasDropLocation, handle);

    // The local player dropping something: the result is the handle of the reference now lying in the world
    constexpr int32_t kDropping = 3;
    if ((reason == kDropping || hasDropLocation) && handle)
    {
        if (TESObjectREFR* pDropped = TESObjectREFR::GetByHandle(handle))
            World::Get().GetRunner().Trigger(DropItemEvent(pDropped->formID, count));
    }

    return pResult;
}

static TiltedPhoques::Initializer s_objectReferencesHooks(
    []()
    {
        POINTER_FALLOUT4(TActivate, s_activate, 753532);
        POINTER_FALLOUT4(TAddInventoryItem, s_addItem, 78186);
        POINTER_FALLOUT4(TRemoveInventoryItem, s_removeItem, 943234);
        POINTER_FALLOUT4(TActorRemoveItem, s_actorRemoveItem, 522079);

        RealActivate = s_activate.Get();
        RealAddInventoryItem = s_addItem.Get();
        RealRemoveInventoryItem = s_removeItem.Get();
        RealActorRemoveItem = s_actorRemoveItem.Get();

        TP_HOOK(&RealActivate, HookActivate);
        TP_HOOK(&RealAddInventoryItem, HookAddInventoryItem);
        TP_HOOK(&RealRemoveInventoryItem, HookRemoveInventoryItem);
        TP_HOOK(&RealActorRemoveItem, HookActorRemoveItem);
    });

Vector<Inventory::ObjectMod> TESObjectREFR::GetObjectMods() const noexcept
{
    Inventory::Entry entry{};
    ReadObjectMods(entry, extraData);
    return entry.ObjectMods;
}

String TESObjectREFR::GetCustomName() const noexcept
{
    return extraData ? extraData->GetCustomName() : String{};
}

void TESObjectREFR::BlockActivation(bool aBlocked) noexcept
{
    using ObjectReference = TESObjectREFR;

    if (!World::Get().ctx().at<PapyrusService>().Get("ObjectReference", "BlockActivation"))
    {
        spdlog::warn("{}: ObjectReference.BlockActivation not found", __FUNCTION__);
        return;
    }

    // BlockActivation(abBlocked, abHideActivateText)
    PAPYRUS_FUNCTION(void, ObjectReference, BlockActivation, bool, bool);
    spdlog::info("{}: {:X} {}", __FUNCTION__, formID, aBlocked);
    s_pBlockActivation(this, aBlocked, false);
}

void TESObjectREFR::SetTransform(const NiPoint3& acPosition, const NiPoint3& acRotation, float aScale) noexcept
{
    // SetLocationOnReference, SetAngleOnReference and SetScale (CommonLibF4 ids 1101832, 1049748, 817930)
    TP_THIS_FUNCTION(TSetPoint, void, TESObjectREFR, const NiPoint3&);
    TP_THIS_FUNCTION(TSetScale, void, TESObjectREFR, float);
    POINTER_FALLOUT4(TSetPoint, s_setLocation, 1101833);
    POINTER_FALLOUT4(TSetPoint, s_setAngle, 1049749);
    POINTER_FALLOUT4(TSetScale, s_setScale, 817931);

    TiltedPhoques::ThisCall(s_setLocation, this, acPosition);
    TiltedPhoques::ThisCall(s_setAngle, this, acRotation);
    if (std::abs(GetScale() - aScale) > 0.001f)
        TiltedPhoques::ThisCall(s_setScale, this, aScale);

    Update3DPosition(true);
}

void TESObjectREFR::SetLinkedRef(TESObjectREFR* apTarget, BGSKeyword* apKeyword) noexcept
{
    // CommonLibF4 id 192840
    TP_THIS_FUNCTION(TSetLinkedRef, void, TESObjectREFR, TESObjectREFR*, BGSKeyword*);
    POINTER_FALLOUT4(TSetLinkedRef, s_setLinkedRef, 192841);

    TiltedPhoques::ThisCall(s_setLinkedRef, this, apTarget, apKeyword);
}

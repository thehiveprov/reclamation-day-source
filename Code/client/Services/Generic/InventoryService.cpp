#include <Services/InventoryService.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>

#include <Events/UpdateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#if TP_SKYRIM64
#include <Games/Skyrim/Interface/UI.h>
#endif
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <Services/PapyrusService.h>
#include <Components/BGSKeywordForm.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>

#include <DefaultObjectManager.h>
#include <PerfProbe.h>

#if TP_FALLOUT4
#include <Messages/RequestActorTrade.h>
#include <Messages/NotifyActorTrade.h>
#include <Interface/UI.h>
#include <Interface/IMenu.h>
#endif

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
#if TP_FALLOUT4
    m_actorTradeConnection = m_dispatcher.sink<NotifyActorTrade>().connect<&InventoryService::OnNotifyActorTrade>(this);
#endif
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    PerfProbe::Section _perf("Inventory");
    RunWeaponStateUpdates();
#if TP_FALLOUT4
    RunEquipmentStateUpdates();
    RunTradeUpdates();
#endif
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.FormId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    uint32_t serverId = 0;
    if (acEvent.OwnershipEpoch != 0)
    {
        bool ownershipMatches = false;
        if (acEvent.IsRemoteActorInteraction)
        {
            const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*iter);
            ownershipMatches = pRemoteComponent && pRemoteComponent->Id == acEvent.ServerId && pRemoteComponent->OwnershipEpoch == acEvent.OwnershipEpoch;
        }
        else
        {
            const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
            ownershipMatches = pLocalComponent && pLocalComponent->Id == acEvent.ServerId && pLocalComponent->OwnershipEpoch == acEvent.OwnershipEpoch;
        }

        if (!ownershipMatches)
        {
            spdlog::debug("Discarded an inventory change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
            return;
        }

        serverId = acEvent.ServerId;
    }
    else
    {
        if (Cast<Actor>(TESForm::GetById(acEvent.FormId)))
            return;

        const std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
        if (!serverIdRes)
        {
            spdlog::warn(
                "Discarded inventory change for form {:X} because it has no server entity (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }
        serverId = *serverIdRes;
    }

    RequestInventoryChanges request;
    request.ServerId = serverId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;

    m_transport.Send(request);

    spdlog::info("Sending item request, item: {:X}, count: {}, target object: {:X}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.ActorId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
    if (acEvent.OwnershipEpoch == 0 || !pLocalComponent || pLocalComponent->Id != acEvent.ServerId || pLocalComponent->OwnershipEpoch != acEvent.OwnershipEpoch)
    {
        spdlog::debug("Discarded an equipment change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

#if TP_FALLOUT4
    // The player's worn set is watched. The others send theirs once this change is done (RunEquipmentStateUpdates):
    // the armor they wear only travels that way.
    if (acEvent.ActorId != 0x14)
        m_changedEquipment[acEvent.ActorId] = std::chrono::steady_clock::now();
#endif

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = acEvent.ServerId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();
    request.ObjectMods = acEvent.ObjectMods;

    m_transport.Send(request);

    spdlog::debug("Sending equipment request, item: {:X}, count: {}, target object: {:X}", acEvent.ItemId, acEvent.Count, acEvent.ActorId);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch != 0)
    {
        Actor* pActor = nullptr;

        auto remoteView = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
        const auto remoteIt = std::find_if(remoteView.begin(), remoteView.end(), [remoteView, &acMessage](const entt::entity aEntity)
        {
            const auto& remoteComponent = remoteView.get<RemoteComponent>(aEntity);
            return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
        });

        if (remoteIt != remoteView.end())
            pActor = Cast<Actor>(TESForm::GetById(remoteView.get<FormIdComponent>(*remoteIt).Id));
        else
        {
            auto localView = m_world.view<LocalComponent, FormIdComponent>();
            const auto localIt = std::find_if(localView.begin(), localView.end(), [localView, &acMessage](const entt::entity aEntity)
            {
                const auto& localComponent = localView.get<LocalComponent>(aEntity);
                return localComponent.Id == acMessage.ServerId && localComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
            });

            if (localIt != localView.end())
                pActor = Cast<Actor>(TESForm::GetById(localView.get<FormIdComponent>(*localIt).Id));
        }

        if (!pActor)
        {
            spdlog::debug("Discarded an inventory update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        ScopedInventoryOverride _;

        if (acMessage.Drop)
            pActor->DropOrPickUpObject(acMessage.Item, nullptr, nullptr);
        else
            pActor->AddOrRemoveItem(acMessage.Item);

        return;
    }

    TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
    if (!pObject)
        return;

    ScopedInventoryOverride _;
    pObject->AddOrRemoveItem(acMessage.Item);
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    const auto it = std::find_if(view.begin(), view.end(), [view, &acMessage](const entt::entity aEntity)
    {
        const auto& remoteComponent = view.get<RemoteComponent>(aEntity);
        return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
    });
    if (it == view.end())
    {
        spdlog::debug("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor)
        return;

#if TP_FALLOUT4
    if (acMessage.FullSync)
    {
        // A copy that's still being built gets its inventory once its 3D is there: the state goes into that
        if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(*it))
            pWaitingFor3D->SpawnRequest.InventoryContent.UpdateEquipment(acMessage.CurrentEquipment);
        else
            ReconcileEquipment(pActor, view.get<RemoteComponent>(*it), acMessage.CurrentEquipment);
        return;
    }
#endif

    auto& modSystem = World::Get().GetModSystem();

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

#if TP_FALLOUT4
    // Armor is synced by state (full equipment syncs, which know about power armor frames): equipped one by one here,
    // power armor pieces were put on the normal body of a player who was getting into a frame
    if (pItem->formType == FormType::Armor)
        return;
#endif

#if TP_SKYRIM64
    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }
#endif

    auto* pObject = Cast<TESBoundObject>(pItem);

    if (acMessage.Unequip)
    {
#if TP_SKYRIM64
        // TODO: ExtraData necessary? probably
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
#elif TP_FALLOUT4
        // Only the one with these mods: after a swap between two of the same weapon, the old one is what comes off,
        // and the copy already holds the new one
        if (pObject)
            pActor->UnequipItem(pObject, &acMessage.ObjectMods);
#endif
    }
    else
    {
        // Unequip all armor first, since the game won't auto unequip armor
#if TP_SKYRIM64
        Inventory wornArmor{};
        if (pItem->formType == FormType::Armor)
        {
            wornArmor = pActor->GetWornArmor();
            for (const auto& armor : wornArmor.Entries)
            {
                uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                TESForm* pArmor = TESForm::GetById(armorId);
                if (pArmor)
                    pEquipManager->UnEquip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
            }
        }

        pEquipManager->Equip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);

        for (const auto& armor : wornArmor.Entries)
        {
            uint32_t armorId = modSystem.GetGameId(armor.BaseId);
            TESForm* pArmor = TESForm::GetById(armorId);
            if (pArmor)
                pEquipManager->Equip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false);
        }
#elif TP_FALLOUT4
        if (!pObject)
            return;

        // Equip the remote actor's copy with the same OMODs. Inventory changes (pickups, crafting, ...) aren't synced
        // on Fallout, so the remote copy may not have it yet: then add it, which also equips it.
        Inventory::Entry entry{};
        entry.BaseId = acMessage.ItemId;
        entry.Count = std::max<int32_t>(1, static_cast<int32_t>(acMessage.Count));
        entry.ObjectMods = acMessage.ObjectMods;
        entry.ExtraWorn = true;

        if (const auto stackId = pActor->FindStackIndex(pObject, entry))
            pActor->EquipItem(pObject, *stackId, entry.Count);
        else
        {
            spdlog::info("{}: remote actor {:X} doesn't have {:X} with these mods yet, adding it", __FUNCTION__, pActor->formID, pObject->formID);
            ScopedInventoryOverride _;
            pActor->AddOrRemoveItem(entry);
        }
#endif
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}

#if TP_FALLOUT4
namespace
{
// What a character wears, as the others are told. Counts change constantly for worn ammo: only the set of worn items
// (and their mods) matters.
Inventory GetWornSet(const Actor* apActor) noexcept
{
    Inventory equipment = apActor->GetEquipment();
    for (auto& entry : equipment.Entries)
        entry.Count = 1;
    return equipment;
}

// Power armor pieces only work on a frame, on a normal body they show up floating/misplaced
bool IsPowerArmorPiece(TESForm* apObject) noexcept
{
    constexpr uint32_t kArmorTypePower = 0x4D8A1; // KYWD in Fallout4.esm

    if (!apObject || apObject->formType != FormType::Armor)
        return false;

    // Read from the form's keyword list, not through Papyrus' HasKeyword (calling that native directly crashed)
    const auto* pKeywords = Cast<BGSKeywordForm>(apObject);
    const TESForm* pKeyword = TESForm::GetById(kArmorTypePower);
    return pKeywords && pKeyword && pKeywords->HasKeyword(pKeyword);
}

// The same item: the same form with the same set of mods, whatever their order
bool IsSameItem(const Inventory::Entry& acLeft, const Inventory::Entry& acRight) noexcept
{
    return acLeft.BaseId == acRight.BaseId && Inventory::ObjectMod::SameMods(acLeft.ObjectMods, acRight.ObjectMods);
}

bool HasItem(const Inventory& acInventory, const Inventory::Entry& acItem) noexcept
{
    return std::any_of(acInventory.Entries.begin(), acInventory.Entries.end(), [&acItem](const Inventory::Entry& acEntry) { return IsSameItem(acEntry, acItem); });
}

int32_t CountOf(const Inventory& acInventory, const Inventory::Entry& acItem) noexcept
{
    int32_t count = 0;
    for (const auto& entry : acInventory.Entries)
    {
        if (IsSameItem(entry, acItem))
            count += entry.Count;
    }
    return count;
}
} // namespace

void InventoryService::RunTradeUpdates() noexcept
{
    static const BSFixedString s_containerMenu("ContainerMenu");

    UI* pUI = UI::Get();
    const bool open = m_transport.IsConnected() && pUI && pUI->GetMenuOpen(s_containerMenu);
    if (!open)
    {
        if (m_trade.ActorId)
            FinishTrade();
        m_trade = {};
        s_tradePartnerId = 0;
        m_tradeMenuSeen = false;
        return;
    }

    // Whose inventory the menu shows is read once per opening
    if (m_tradeMenuSeen)
        return;
    m_tradeMenuSeen = true;

    // ContainerMenuBase::containerRef (CommonLibF4): the handle of what the menu shows next to the player's inventory
    constexpr size_t kContainerRefOffset = 0x428;
    IMenu* pMenu = pUI->FindMenuByName("ContainerMenu");
    if (!pMenu)
        return;
    const uint32_t handle = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pMenu) + kContainerRefOffset);

    // Only a living character that another player's game runs, not a player: this game's own are synced as they are,
    // and what's on a body is each player's own loot
    Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(handle));
    if (!pActor || pActor->IsDead())
        return;
    ActorExtension* pExtension = pActor->GetExtension();
    if (!pExtension || !pExtension->IsRemote() || pExtension->IsPlayer())
        return;

    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view, formId = pActor->formID](const entt::entity aEntity) { return view.get<FormIdComponent>(aEntity).Id == formId; });
    if (it == view.end())
        return;

    const auto& remote = view.get<RemoteComponent>(*it);
    m_trade = {pActor->formID, remote.Id, remote.OwnershipEpoch, pActor->GetInventory(), GetWornSet(pActor)};
    s_tradePartnerId = pActor->formID;

    spdlog::info("{}: trading with {:X}, run by another player's game (server id {:X})", __FUNCTION__, pActor->formID, remote.Id);
}

void InventoryService::FinishTrade() noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(m_trade.ActorId));
    if (!pActor)
        return;

    RequestActorTrade request{};
    request.ServerId = m_trade.ServerId;
    request.OwnershipEpoch = m_trade.OwnershipEpoch;

    // Each item once, by how many more or fewer the copy has now
    const Inventory after = pActor->GetInventory();
    const auto addChange = [&](const Inventory::Entry& acItem)
    {
        if (HasItem(request.Changes, acItem))
            return;
        const int32_t delta = CountOf(after, acItem) - CountOf(m_trade.Before, acItem);
        if (delta == 0)
            return;
        Inventory::Entry change = acItem;
        change.Count = delta;
        change.ExtraWorn = false;
        change.ExtraWornLeft = false;
        request.Changes.Entries.push_back(std::move(change));
    };
    for (const auto& entry : after.Entries)
        addChange(entry);
    for (const auto& entry : m_trade.Before.Entries)
        addChange(entry);

    // What it was given to wear (1) and had taken off (-1): only what the trade changed, so a copy that was dressed
    // differently from its owner's for whatever reason can't undress it
    const Inventory wornAfter = GetWornSet(pActor);
    for (const auto& entry : wornAfter.Entries)
    {
        if (!HasItem(m_trade.WornBefore, entry))
            request.Worn.Entries.push_back(entry);
    }
    for (const auto& entry : m_trade.WornBefore.Entries)
    {
        if (!HasItem(wornAfter, entry))
        {
            Inventory::Entry takenOff = entry;
            takenOff.Count = -1;
            request.Worn.Entries.push_back(std::move(takenOff));
        }
    }

    if (request.Changes.Entries.empty() && request.Worn.Entries.empty())
        return;

    m_transport.Send(request);

    spdlog::info("{}: trade with {:X} sent to its owner: {} item changes, {} worn changes", __FUNCTION__, m_trade.ActorId, request.Changes.Entries.size(), request.Worn.Entries.size());
}

void InventoryService::OnNotifyActorTrade(const NotifyActorTrade& acMessage) noexcept
{
    auto view = m_world.view<LocalComponent, FormIdComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view, &acMessage](const entt::entity aEntity)
    {
        const auto& local = view.get<LocalComponent>(aEntity);
        return local.Id == acMessage.ServerId && local.OwnershipEpoch == acMessage.OwnershipEpoch;
    });
    if (it == view.end())
    {
        spdlog::debug("{}: discarded a trade with {:X}: epoch {} is no longer this game's", __FUNCTION__, acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor || pActor->IsDead())
        return;

    auto& modSystem = World::Get().GetModSystem();
    const auto getObject = [&modSystem](const Inventory::Entry& acEntry) { return Cast<TESBoundObject>(TESForm::GetById(modSystem.GetGameId(acEntry.BaseId))); };

    // The items first, quest items left where their quest put them
    const auto questItems = pActor->GetQuestItems();
    for (const auto& change : acMessage.Changes.Entries)
    {
        const TESBoundObject* pObject = getObject(change);
        if (!pObject || questItems.find(pObject->formID) != questItems.end())
            continue;
        pActor->AddOrRemoveItem(change);
    }

    // Then dressed as the trade left it. In power armor its armor is the frame's: only weapons.
    const bool inPowerArmor = pActor->IsInPowerArmor();
    const Inventory actual = pActor->GetEquipment();
    for (const auto& change : acMessage.Worn.Entries)
    {
        TESBoundObject* pObject = getObject(change);
        if (!pObject || (inPowerArmor && pObject->formType == FormType::Armor) || (!inPowerArmor && IsPowerArmorPiece(pObject)))
            continue;

        if (change.Count < 0)
        {
            if (HasItem(actual, change))
                pActor->UnequipItem(pObject, &change.ObjectMods);
        }
        else if (!HasItem(actual, change))
        {
            if (const auto stackId = pActor->FindStackIndex(pObject, change))
                pActor->EquipItem(pObject, *stackId);
            else
                spdlog::warn("{}: {:X} was given {:X} to wear but doesn't have it", __FUNCTION__, pActor->formID, pObject->formID);
        }
    }

    spdlog::info("{}: {:X} traded by another player: {} item changes, {} worn changes", __FUNCTION__, pActor->formID, acMessage.Changes.Entries.size(), acMessage.Worn.Entries.size());
}

void InventoryService::RunEquipmentStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
    {
        m_hasSentEquipment = false;
        m_changedEquipment.clear();
        return;
    }

    static std::chrono::steady_clock::time_point lastCheckTimePoint;
    constexpr auto cDelayBetweenChecks = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastCheckTimePoint < cDelayBetweenChecks)
        return;

    lastCheckTimePoint = now;

    // A change is sent once it's this old: the game can take a moment to put an item on, and a character that changes
    // several things in a row sends one state.
    constexpr auto cSettleTime = 500ms;
    const auto hasSettled = [now](const auto& acChange) { return now - acChange.second >= cSettleTime; };

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (auto entity : view)
    {
        const uint32_t formId = view.get<FormIdComponent>(entity).Id;
        const auto& localComponent = view.get<LocalComponent>(entity);

        if (formId != 0x14)
        {
            const auto change = m_changedEquipment.find(formId);
            if (change == m_changedEquipment.end() || !hasSettled(*change))
                continue;

            // Not of the dead: what's taken off a body is each player's own loot
            Actor* const pActor = Cast<Actor>(TESForm::GetById(formId));
            if (pActor && !pActor->IsDead())
                SendEquipmentState(localComponent, GetWornSet(pActor));
            continue;
        }

        Actor* const pPlayer = Cast<Actor>(TESForm::GetById(0x14));
        if (!pPlayer)
            continue;

        Inventory equipment = GetWornSet(pPlayer);

        // Also resent every few seconds without changes: the other players' copy of this character can be respawned
        // (cell changes, reloads) and come back wearing other things, and only a new state corrects it.
        static std::chrono::steady_clock::time_point lastSendTimePoint;
        constexpr auto cDelayBetweenResends = 5s;

        const bool changed = !m_hasSentEquipment || equipment != m_lastSentEquipment;
        if (!changed && now - lastSendTimePoint < cDelayBetweenResends)
            continue;

        lastSendTimePoint = now;
        SendEquipmentState(localComponent, equipment);

        if (changed)
            spdlog::info("Sending full equipment state, {} worn items", equipment.Entries.size());

        m_lastSentEquipment = std::move(equipment);
        m_hasSentEquipment = true;
    }

    // Sent, or of a character that isn't this game's to report anymore
    for (auto it = m_changedEquipment.begin(); it != m_changedEquipment.end();)
        it = hasSettled(*it) ? m_changedEquipment.erase(it) : std::next(it);
}

void InventoryService::SendEquipmentState(const LocalComponent& acLocal, const Inventory& acEquipment) const noexcept
{
    RequestEquipmentChanges request{};
    request.ServerId = acLocal.Id;
    request.OwnershipEpoch = acLocal.OwnershipEpoch;
    request.FullSync = true;
    request.CurrentInventory = acEquipment;

    m_transport.Send(request);
}

void InventoryService::ReconcileEquipment(Actor* apActor, RemoteComponent& arRemote, const Inventory& acWanted) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    // The same item: the same form with the same set of mods, whatever their order (the order, ranks and attach
    // indexes of a rebuilt copy can differ from its owner's)
    const auto hasItem = [](const Inventory& acInventory, const Inventory::Entry& acItem)
    {
        return std::any_of(acInventory.Entries.begin(), acInventory.Entries.end(), [&acItem](const Inventory::Entry& acEntry)
        { return acEntry.BaseId == acItem.BaseId && Inventory::ObjectMod::SameMods(acEntry.ObjectMods, acItem.ObjectMods); });
    };
    const auto hasForm = [](const Inventory& acInventory, const GameId& acBaseId)
    { return std::any_of(acInventory.Entries.begin(), acInventory.Entries.end(), [&acBaseId](const Inventory::Entry& acEntry) { return acEntry.BaseId == acBaseId; }); };
    const auto getObject = [&modSystem](const Inventory::Entry& acEntry) { return Cast<TESBoundObject>(TESForm::GetById(modSystem.GetGameId(acEntry.BaseId))); };
    const auto isWeaponOrArmor = [](const TESForm* apObject) { return apObject->formType == FormType::Armor || apObject->formType == FormType::Weapon; };

    // Compared with what the copy wears right now, so a copy that isn't dressed like its owner's for whatever reason is
    // put right by the next state (a player's is sent every few seconds). What the owner wore the last time is only
    // for what can't be read off the copy, see below.
    const Inventory actual = apActor->GetEquipment();
    const std::optional<Inventory> previous = std::exchange(arRemote.AppliedEquipment, acWanted);

    // In power armor, armor is handled by SyncPowerArmorPieces: the frame's pieces by item only, clothes underneath
    // not at all
    const bool inPowerArmor = apActor->IsInPowerArmor();
    const auto isFramePiece = [inPowerArmor](const TESForm* apObject) { return inPowerArmor && apObject->formType == FormType::Armor; };

    if (inPowerArmor)
        SyncPowerArmorPieces(apActor, acWanted);

    // Take off what the owner doesn't wear in any form (another one of the same item goes when the right one is put
    // on). Weapons and armor whoever put them on: the game dresses a character it creates by itself. Of the rest
    // (ammo, and some books read as worn too) only what came from the owner: those kept being "taken off" otherwise.
    for (const auto& worn : actual.Entries)
    {
        if (hasForm(acWanted, worn.BaseId))
            continue;

        TESBoundObject* pObject = getObject(worn);
        if (!pObject || isFramePiece(pObject))
            continue;
        if (!isWeaponOrArmor(pObject) && !(previous && hasForm(*previous, worn.BaseId)))
            continue;

        spdlog::info("{}: {:X} takes off {:X}", __FUNCTION__, apActor->formID, pObject->formID);
        apActor->UnequipItem(pObject);
    }

    // Put on what the owner wears and the copy doesn't
    for (const auto& wanted : acWanted.Entries)
    {
        if (hasItem(actual, wanted))
            continue;

        // Whether the owner already wore it the last time: then the copy was given it then
        const bool isNew = !previous || !hasItem(*previous, wanted);

        TESBoundObject* pObject = getObject(wanted);
        if (!pObject)
        {
            if (isNew)
                spdlog::warn("{}: worn item {:X}:{:X} not found, is the load order the same?", __FUNCTION__, wanted.BaseId.ModId, wanted.BaseId.BaseId);
            continue;
        }

        if (isFramePiece(pObject))
            continue;

        // The owner is in power armor but this copy isn't in a frame (yet): the pieces come with the frame
        if (!inPowerArmor && IsPowerArmorPiece(pObject))
            continue;

        // Weapons and armor again with every state until the copy wears them, the rest once
        if (!isNew && !isWeaponOrArmor(pObject))
            continue;

        if (const auto stackId = apActor->FindStackIndex(pObject, wanted))
        {
            spdlog::info("{}: {:X} puts on {:X} (stack {})", __FUNCTION__, apActor->formID, pObject->formID, *stackId);
            apActor->EquipItem(pObject, *stackId);
            continue;
        }

        // The copy doesn't have it in this form: it gets one, once. If the game makes something else of it (it then
        // reads back with other mods), asking again with every state would fill the copy's inventory.
        if (!isNew)
            continue;

        spdlog::info("{}: {:X} doesn't have {:X} with these mods, it gets one to put on", __FUNCTION__, apActor->formID, pObject->formID);

        Inventory::Entry entry = wanted;
        entry.ExtraWorn = true;

        ScopedInventoryOverride _;
        apActor->AddOrRemoveItem(entry);
    }
}

void InventoryService::SyncPowerArmorPieces(Actor* apActor, const Inventory& acWanted) noexcept
{
    // The copy wears its own copy of the frame's pieces, whose mods (paint, linings) can differ from the owner's:
    // compared by item only. The helmet in particular can be put on and taken off while in the frame.
    auto& modSystem = World::Get().GetModSystem();
    const auto getPiece = [&modSystem](const Inventory::Entry& acEntry) -> TESBoundObject*
    {
        auto* pObject = Cast<TESBoundObject>(TESForm::GetById(modSystem.GetGameId(acEntry.BaseId)));
        return IsPowerArmorPiece(pObject) ? pObject : nullptr;
    };
    const auto hasForm = [](const Vector<Inventory::Entry>& acEntries, const GameId& acBaseId)
    { return std::any_of(acEntries.begin(), acEntries.end(), [&](const auto& acEntry) { return acEntry.BaseId == acBaseId; }); };

    const Inventory actual = apActor->GetEquipment();

    for (const auto& worn : actual.Entries)
    {
        auto* pPiece = getPiece(worn);
        if (!pPiece || hasForm(acWanted.Entries, worn.BaseId))
            continue;

        spdlog::info("{}: {:X} takes off power armor piece {:X}", __FUNCTION__, apActor->formID, pPiece->formID);
        apActor->UnequipItem(pPiece);
    }

    for (const auto& wantedEntry : acWanted.Entries)
    {
        auto* pPiece = getPiece(wantedEntry);
        if (!pPiece || hasForm(actual.Entries, wantedEntry.BaseId))
            continue;

        // Any copy of the piece will do
        std::optional<uint32_t> stackId;
        for (const auto& owned : apActor->GetActorInventory().Entries)
        {
            if (owned.BaseId == wantedEntry.BaseId && (stackId = apActor->FindStackIndex(pPiece, owned)))
                break;
        }

        if (stackId)
        {
            spdlog::info("{}: {:X} puts on power armor piece {:X} (stack {})", __FUNCTION__, apActor->formID, pPiece->formID, *stackId);
            apActor->EquipItem(pPiece, *stackId);
        }
        else
        {
            spdlog::info("{}: {:X} doesn't have power armor piece {:X}, it gets one to put on", __FUNCTION__, apActor->formID, pPiece->formID);
            Inventory::Entry entry = wantedEntry;
            entry.ExtraWorn = true;
            ScopedInventoryOverride _;
            apActor->AddOrRemoveItem(entry);
        }
    }
}
#endif

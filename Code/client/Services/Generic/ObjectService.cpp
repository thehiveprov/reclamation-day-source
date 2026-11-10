#include <Services/ObjectService.h>

#include <World.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/RequestQuestItemTaken.h>
#include <Messages/NotifyQuestItemTaken.h>
#include <Components/BGSKeywordForm.h>
#include <Messages/RequestPowerArmor.h>
#include <Messages/NotifyPowerArmor.h>
#include <Messages/RequestDropItem.h>
#include <Messages/NotifyItemDropped.h>
#include <Messages/RequestItemPickedUp.h>
#include <Messages/NotifyItemPickedUp.h>
#include <Events/DropItemEvent.h>
#if TP_FALLOUT4
#include <Events/TerminalItemRunEvent.h>
#include <Messages/RequestTerminalItemRun.h>
#include <Messages/NotifyTerminalItemRun.h>
#include <Terminals.h>
#include <Events/DestructionStageEvent.h>
#include <Messages/RequestDestructionStage.h>
#include <Messages/NotifyDestructionStage.h>
#include <Destruction.h>
#include <Messages/RequestPipboyLight.h>
#include <Messages/NotifyPipboyLight.h>
#include <PipboyLights.h>
#endif
#include <Services/OverlayService.h>
#include <Services/TransportService.h>
#include <Games/Overrides.h>
#include <Services/PapyrusService.h>
#include <Services/QuestService.h>
#include <Games/ActorExtension.h>
#include <ExtraData/ExtraDataList.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#if TP_SKYRIM64
#include <Forms/BGSEncounterZone.h>
#endif

#include <inttypes.h>
#include <PerfProbe.h>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ObjectService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&ObjectService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&ObjectService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&ObjectService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&ObjectService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&ObjectService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&ObjectService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&ObjectService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&ObjectService::OnNotifyScriptAnimation>(this);
#if TP_FALLOUT4
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ObjectService::OnUpdate>(this);
    m_questItemTakenConnection = aDispatcher.sink<NotifyQuestItemTaken>().connect<&ObjectService::OnNotifyQuestItemTaken>(this);
    m_powerArmorConnection = aDispatcher.sink<NotifyPowerArmor>().connect<&ObjectService::OnNotifyPowerArmor>(this);
    m_dropItemConnection = aDispatcher.sink<DropItemEvent>().connect<&ObjectService::OnDropItemEvent>(this);
    m_itemDroppedConnection = aDispatcher.sink<NotifyItemDropped>().connect<&ObjectService::OnNotifyItemDropped>(this);
    m_itemPickedUpConnection = aDispatcher.sink<NotifyItemPickedUp>().connect<&ObjectService::OnNotifyItemPickedUp>(this);
    m_terminalItemRunConnection = aDispatcher.sink<TerminalItemRunEvent>().connect<&ObjectService::OnTerminalItemRun>(this);
    m_terminalItemRunNotifyConnection = aDispatcher.sink<NotifyTerminalItemRun>().connect<&ObjectService::OnNotifyTerminalItemRun>(this);
    m_destructionStageConnection = aDispatcher.sink<DestructionStageEvent>().connect<&ObjectService::OnDestructionStage>(this);
    m_destructionStageNotifyConnection = aDispatcher.sink<NotifyDestructionStage>().connect<&ObjectService::OnNotifyDestructionStage>(this);

    m_pipboyLightConnection = aDispatcher.sink<NotifyPipboyLight>().connect<&ObjectService::OnNotifyPipboyLight>(this);

    Destruction::Install();
#endif

#if TP_SKYRIM64
    EventDispatcherManager::Get()->activateEvent.RegisterSink(this);
#else
    GetEventDispatcher_TESActivateEvent()->RegisterSink(this);
#endif
}

#if TP_SKYRIM64
bool IsPlayerHome(const TESObjectCELL* pCell) noexcept
{
    if (pCell && pCell->loadedCellData && pCell->loadedCellData->encounterZone)
    {
        // Only return true if cell has the NoResetZone encounter zone
        if (pCell->loadedCellData->encounterZone->formID == 0xf90b1)
        {
            switch (pCell->formID)
            {
            case 0xeec55: // one known exception: Sinderion's Field Lab
                return false;
            default: return true;
            }
        }
    }

    return false;
}

bool ShouldSyncObject(const TESObjectREFR* apObject) noexcept
{
    if (!apObject)
        return false;

    switch (apObject->formID)
    {
        // Don't sync the chest in the "Diplomatic Immunity" quest
    case 0x39CF1: return false;
    default: return true;
    }
}
#endif

#if TP_FALLOUT4
// Loot rules: items lying in the world are shared (picking one up removes it for everyone), but what's in containers
// and on bodies is per player, so activating those (opening a container, looting a body) must never be replayed on
// other clients, and their contents aren't synced.
// TODO: quest items in containers/on bodies must still only exist once.
bool IsPerPlayerLoot(const TESObjectREFR* apObject) noexcept
{
    if (!apObject || !apObject->baseForm)
        return false;

    switch (apObject->baseForm->formType)
    {
    case FormType::Container:
    case FormType::Npc:
    case FormType::LeveledCharacter: return true;
    default: return apObject->formType == FormType::Character;
    }
}

// Loose items that can be picked up. Only the players' pickups are shared: NPCs (e.g. a disarmed raider grabbing a
// weapon) pick things up on their own client only, otherwise the item vanished from every world (a quest's laser
// musket went missing for everyone while its ammo was still there).
bool IsPickupItem(const TESObjectREFR* apObject) noexcept
{
    if (!apObject || !apObject->baseForm)
        return false;

    switch (apObject->baseForm->formType)
    {
    case FormType::Weapon:
    case FormType::Armor:
    case FormType::Ammo:
    case FormType::Misc:
    case FormType::Alchemy:
    case FormType::Book:
    case FormType::Note:
    case FormType::Key:
    case FormType::Ingredient:
    case FormType::Component: return true;
    default: return false;
    }
}
#endif

#if TP_FALLOUT4
namespace
{
constexpr uint32_t kFurnitureTypePowerArmor = 0x3430B; // KYWD in Fallout4.esm

bool IsPowerArmorFrame(TESObjectREFR* apObject) noexcept
{
    if (!apObject || !apObject->baseForm || apObject->baseForm->formType != FormType::Furniture)
        return false;

    // Read from the base form's keyword list. Not through Papyrus' HasKeyword: calling that native directly crashed.
    const auto* pKeywords = Cast<BGSKeywordForm>(apObject->baseForm);
    const TESForm* pKeyword = TESForm::GetById(kFurnitureTypePowerArmor);
    return pKeywords && pKeyword && pKeywords->HasKeyword(pKeyword);
}

uint64_t ToKey(const GameId& acId) noexcept
{
    return (static_cast<uint64_t>(acId.ModId) << 32) | acId.BaseId;
}
} // namespace

void ObjectService::RunPipboyLightUpdates() noexcept
{
    static std::chrono::steady_clock::time_point s_lastCheck;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastCheck < 250ms)
        return;
    s_lastCheck = now;

    if (!m_transport.IsConnected())
    {
        m_pipboyLightOn = false;
        if (!m_pipboyLights.empty())
        {
            m_pipboyLights.clear();
            PipboyLights::Update({});
        }
        return;
    }

    // The others' lights, on their characters here; one not heard of for a while is off (their game left)
    Vector<Actor*> lit;
    for (auto it = m_pipboyLights.begin(); it != m_pipboyLights.end();)
    {
        if (now - it->second > 15s)
        {
            it = m_pipboyLights.erase(it);
            continue;
        }

        if (Actor* pActor = Utils::GetByServerId<Actor>(it->first))
            lit.push_back(pActor);
        ++it;
    }
    PipboyLights::Update(lit);

    const bool on = PipboyLights::IsLocalOn();
    const bool changed = on != m_pipboyLightOn;
    if (!changed && !(on && now - m_lastPipboyLightSend > 5s))
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });
    if (it == view.end())
        return;

    RequestPipboyLight request{};
    request.ServerId = view.get<LocalComponent>(*it).Id;
    request.On = on;

    m_transport.Send(request);
    m_lastPipboyLightSend = now;
    m_pipboyLightOn = on;

    if (changed)
        spdlog::info("Local Pip-Boy light {}", on ? "on" : "off");
}

void ObjectService::OnNotifyPipboyLight(const NotifyPipboyLight& acMessage) noexcept
{
    if (acMessage.On)
        m_pipboyLights[acMessage.ServerId] = std::chrono::steady_clock::now();
    else
        m_pipboyLights.erase(acMessage.ServerId);
}

void ObjectService::RunPowerArmorUpdates() noexcept
{
    if (!m_transport.IsConnected())
    {
        m_inPowerArmor = false;
        return;
    }

    static std::chrono::steady_clock::time_point s_lastCheck;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastCheck < 250ms)
        return;
    s_lastCheck = now;

    // Claims are applied to frames as they get loaded
    static std::chrono::steady_clock::time_point s_lastClaimCheck;
    if (now - s_lastClaimCheck > 2s)
    {
        s_lastClaimCheck = now;
        ApplyPowerArmorClaims();
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const bool inPowerArmor = pPlayer->IsInPowerArmor();
    const bool changed = inPowerArmor != m_inPowerArmor;

    // While in power armor, Enter is resent now and then for copies of the character spawned since on other screens
    if (!changed && !(inPowerArmor && now - m_lastPowerArmorSend > 5s))
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });
    if (it == view.end())
        return;

    if (changed && inPowerArmor)
        m_powerArmorFrameId = m_lastFrameId;

    RequestPowerArmor request{};
    request.ServerId = view.get<LocalComponent>(*it).Id;
    request.Type = inPowerArmor ? RequestPowerArmor::kEnter : RequestPowerArmor::kExit;

    // Temporary frames (e.g. placed from the inventory) have no id the other players would know
    if (m_powerArmorFrameId && (m_powerArmorFrameId & 0xFF000000) != 0xFF000000)
        m_world.GetModSystem().GetServerModId(m_powerArmorFrameId, request.FrameId);

    m_transport.Send(request);
    m_lastPowerArmorSend = now;
    m_inPowerArmor = inPowerArmor;

    if (changed)
        spdlog::info("Local player got {} power armor frame {:X}", inPowerArmor ? "into" : "out of", m_powerArmorFrameId);

    if (!inPowerArmor)
        m_powerArmorFrameId = 0;
}

void ObjectService::ApplyPowerArmorClaims() noexcept
{
    auto& modSystem = m_world.GetModSystem();
    const uint32_t localPlayerId = m_transport.GetLocalPlayerId();

    for (const auto& [key, ownerId] : m_powerArmorClaims)
    {
        const GameId frameId(static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFF));
        auto* pFrame = Cast<TESObjectREFR>(TESForm::GetById(modSystem.GetGameId(frameId)));
        if (!pFrame)
            continue;

        // Applied once per frame and state, the block is kept on the reference
        static TiltedPhoques::Map<uint32_t, bool> s_applied;
        const bool blocked = ownerId != 0 && ownerId != localPlayerId;
        if (const auto it = s_applied.find(pFrame->formID); it != s_applied.end() && it->second == blocked)
            continue;

        s_applied[pFrame->formID] = blocked;
        pFrame->BlockActivation(blocked);
    }
}

void ObjectService::OnNotifyPowerArmor(const NotifyPowerArmor& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();
    const bool hasFrame = acMessage.FrameId != GameId{};
    TESObjectREFR* pFrame = hasFrame ? Cast<TESObjectREFR>(TESForm::GetById(modSystem.GetGameId(acMessage.FrameId))) : nullptr;

    switch (acMessage.Type)
    {
    case NotifyPowerArmor::kClaim:
    {
        if (!hasFrame)
            return;

        if (acMessage.PlayerId)
            m_powerArmorClaims[ToKey(acMessage.FrameId)] = acMessage.PlayerId;
        else
            m_powerArmorClaims.erase(ToKey(acMessage.FrameId));

        if (pFrame)
            pFrame->BlockActivation(acMessage.PlayerId != 0 && acMessage.PlayerId != m_transport.GetLocalPlayerId());
        return;
    }
    case NotifyPowerArmor::kDenied:
    {
        // Someone else got into this frame first
        spdlog::info("Power armor {:X} belongs to player {}, getting out", pFrame ? pFrame->formID : 0, acMessage.PlayerId);
        m_powerArmorClaims[ToKey(acMessage.FrameId)] = acMessage.PlayerId;
        PlayerCharacter::Get()->SwitchToPowerArmor(nullptr);
        m_world.GetOverlayService().SendSystemMessage("This power armor belongs to another player.");
        return;
    }
    default: break;
    }

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor)
    {
        // Not spawned here (yet), Enter is resent while they're in it
        return;
    }

    if (acMessage.Type == NotifyPowerArmor::kEnter)
    {
        // Enter is resent every few seconds, only block the frame when the claim is new
        if (hasFrame)
        {
            auto& owner = m_powerArmorClaims[ToKey(acMessage.FrameId)];
            const bool isNewClaim = owner != acMessage.PlayerId;
            owner = acMessage.PlayerId;
            if (pFrame && isNewClaim)
                pFrame->BlockActivation(true);
        }

        if (pActor->IsInPowerArmor())
            return;

        if (!pFrame)
        {
            spdlog::warn("{}: {:X} got into power armor, but its frame {:X}:{:X} isn't known here (temporary frame?)", __FUNCTION__, pActor->formID, acMessage.FrameId.ModId, acMessage.FrameId.BaseId);
            return;
        }

        spdlog::info("{:X} gets into power armor {:X}", pActor->formID, pFrame->formID);
        pActor->SwitchToPowerArmor(pFrame);
    }
    else if (acMessage.Type == NotifyPowerArmor::kExit)
    {
        if (!pActor->IsInPowerArmor())
            return;

        spdlog::info("{:X} gets out of power armor", pActor->formID);
        pActor->SwitchToPowerArmor(nullptr);
    }
}

void ObjectService::OnDropItemEvent(const DropItemEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto* pDropped = Cast<TESObjectREFR>(TESForm::GetById(acEvent.RefId));
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pDropped || !pDropped->baseForm || !pPlayer)
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });
    if (it == view.end())
        return;

    auto& modSystem = m_world.GetModSystem();

    RequestDropItem request{};
    request.ActorServerId = view.get<LocalComponent>(*it).Id;
    request.LocalRefId = pDropped->formID;
    if (!modSystem.GetServerModId(pDropped->baseForm->formID, request.Item.BaseId))
    {
        spdlog::warn("{}: dropped {:X} has no server id, not shared", __FUNCTION__, pDropped->baseForm->formID);
        return;
    }
    request.Item.Count = acEvent.Count;
    request.Item.ObjectMods = pDropped->GetObjectMods();
    request.Item.CustomName = pDropped->GetCustomName();

    if (TESObjectCELL* pCell = pDropped->GetParentCellEx())
        modSystem.GetServerModId(pCell->formID, request.CellId);
    if (pPlayer->GetWorldSpace())
        modSystem.GetServerModId(pPlayer->GetWorldSpace()->formID, request.WorldSpaceId);

    request.Position = {pDropped->position.x, pDropped->position.y, pDropped->position.z};
    request.Rotation = {pDropped->rotation.x, pDropped->rotation.y, pDropped->rotation.z};

    m_transport.Send(request);

    std::string mods;
    for (const auto& mod : request.Item.ObjectMods)
        mods += fmt::format("{:X} ", modSystem.GetGameId(mod.ModId));
    spdlog::info("Dropped {:X} x{} as {:X}, sharing it, mods: [{}]", pDropped->baseForm->formID, acEvent.Count, pDropped->formID, mods);
}

bool ObjectService::SpawnWorldItem(const NotifyItemDropped& acMessage) noexcept
{
    // Dropped by the local copy of the dropper's character, out of its inventory: the item keeps its mods, and the
    // game places it like any drop. Returns false while that character isn't here yet.
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActorServerId);
    if (!pActor)
        return false;

    auto& modSystem = m_world.GetModSystem();
    auto* pObject = Cast<TESBoundObject>(TESForm::GetById(modSystem.GetGameId(acMessage.Item.BaseId)));
    if (!pObject)
    {
        spdlog::warn("{}: world item {} ({:X}:{:X}) not found here, is the load order the same?", __FUNCTION__, acMessage.ItemId, acMessage.Item.BaseId.ModId, acMessage.Item.BaseId.BaseId);
        return true;
    }
    if (acMessage.Item.Count <= 0)
    {
        spdlog::warn("{}: world item {} has a count of {}", __FUNCTION__, acMessage.ItemId, acMessage.Item.Count);
        return true;
    }

    Inventory::Entry entry = acMessage.Item;
    entry.ExtraWorn = entry.ExtraWornLeft = false;

    // The stack the item goes to is the one that is new or grew, told apart by the stack itself rather than its position
    // or count: the character can already hold another copy (the pipe pistol it was given when its owner joined), and
    // two stacks of 1 look the same by count, which dropped that other pistol instead. Matching the mods would also
    // pick an identical copy the character is wearing.
    const auto stacksBefore = pActor->GetStacks(pObject);
    {
        ScopedInventoryOverride _;
        pActor->AddOrRemoveItem(entry);
    }
    const auto stacksAfter = pActor->GetStacks(pObject);

    std::optional<uint32_t> stackId;
    for (uint32_t i = 0; i < stacksAfter.size() && !stackId; ++i)
    {
        const auto before = std::find_if(stacksBefore.begin(), stacksBefore.end(), [&](const auto& acStack) { return acStack.pStack == stacksAfter[i].pStack; });
        if (before == stacksBefore.end() || stacksAfter[i].Count > before->Count)
            stackId = i;
    }

    std::string stacks;
    for (uint32_t i = 0; i < stacksAfter.size(); ++i)
    {
        std::string mods;
        for (const auto& mod : pActor->GetStackObjectMods(pObject, i))
            mods += fmt::format("{:X} ", modSystem.GetGameId(mod.ModId));
        stacks += fmt::format("{}{} x{} [{}] ", i, stackId == i ? " (new)" : "", stacksAfter[i].Count, mods);
    }
    spdlog::info("{}: {:X} had {} stacks of {:X}, now {}", __FUNCTION__, pActor->formID, stacksBefore.size(), pObject->formID, stacks);

    if (!stackId)
        stackId = pActor->FindStackIndex(pObject, entry);
    if (!stackId)
    {
        spdlog::warn("{}: couldn't give {:X} to {:X} to drop it", __FUNCTION__, pObject->formID, pActor->formID);
        return true;
    }

    const NiPoint3 position(glm::vec3(acMessage.Position[0], acMessage.Position[1], acMessage.Position[2]));
    const NiPoint3 rotation(glm::vec3(acMessage.Rotation[0], acMessage.Rotation[1], acMessage.Rotation[2]));
    TESObjectREFR* pRef = pActor->DropStack(pObject, *stackId, entry.Count, &position, &rotation);
    if (!pRef)
    {
        spdlog::warn("{}: dropping {:X} failed", __FUNCTION__, pObject->formID);
        return true;
    }

    m_worldItemRefs[acMessage.ItemId] = pRef->formID;
    m_refWorldItems[pRef->formID] = acMessage.ItemId;

    // The name its dropper gave it: the stack had it, but a drop by a character other than the player leaves it behind
    if (!entry.CustomName.empty() && pRef->extraData && pRef->GetCustomName() != entry.CustomName)
        pRef->extraData->SetCustomName(entry.CustomName.c_str());

    std::string mods;
    for (const auto& mod : pRef->GetObjectMods())
        mods += fmt::format("{:X} ", modSystem.GetGameId(mod.ModId));
    spdlog::info("World item {} ({:X} x{}) dropped here as {:X} from stack {}, its mods: [{}] (wanted {}), name '{}' (wanted '{}')", acMessage.ItemId, pObject->formID, entry.Count, pRef->formID,
                 *stackId, mods, entry.ObjectMods.size(), pRef->GetCustomName().c_str(), entry.CustomName.c_str());
    return true;
}

void ObjectService::OnNotifyItemDropped(const NotifyItemDropped& acMessage) noexcept
{
    // Our own drop coming back with its id
    if (acMessage.PlayerId == m_transport.GetLocalPlayerId() && acMessage.LocalRefId)
    {
        m_worldItemRefs[acMessage.ItemId] = acMessage.LocalRefId;
        m_refWorldItems[acMessage.LocalRefId] = acMessage.ItemId;
        return;
    }

    if (!SpawnWorldItem(acMessage))
        m_pendingWorldItems[acMessage.ItemId] = acMessage;
}

void ObjectService::OnNotifyItemPickedUp(const NotifyItemPickedUp& acMessage) noexcept
{
    m_pendingWorldItems.erase(acMessage.ItemId);

    const auto it = m_worldItemRefs.find(acMessage.ItemId);
    if (it == m_worldItemRefs.end())
        return;

    const uint32_t refId = it->second;
    m_worldItemRefs.erase(it);
    m_refWorldItems.erase(refId);

    if (auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(refId)))
    {
        pRef->Disable();
        pRef->Delete();
        spdlog::info("World item {} picked up by another player, removed {:X}", acMessage.ItemId, refId);
    }
}

void ObjectService::RunWorldItemUpdates() noexcept
{
    if (m_pendingWorldItems.empty())
        return;

    static std::chrono::steady_clock::time_point s_lastCheck;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastCheck < 2s)
        return;
    s_lastCheck = now;

    for (auto it = m_pendingWorldItems.begin(); it != m_pendingWorldItems.end();)
    {
        if (SpawnWorldItem(it->second))
            it = m_pendingWorldItems.erase(it);
        else
            ++it;
    }
}

void ObjectService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfProbe::Section _perf("Object");
    RunPowerArmorUpdates();
    RunPipboyLightUpdates();
    if (m_transport.IsConnected())
        RunWorldItemUpdates();

    if (!m_lootSourceId || !m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point s_lastCheck;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastCheck < 250ms)
        return;
    s_lastCheck = now;

    // Long enough to cover browsing a container; another loot activation restarts it
    if (now - m_lootSourceOpened > 120s)
    {
        m_lootSourceId = 0;
        return;
    }

    for (const auto& [itemId, questItem] : PlayerCharacter::Get()->GetQuestItems())
    {
        uint32_t& before = m_questItemsBeforeLooting[itemId];
        if (questItem.Count <= before)
            continue;

        ShareQuestItemTaken(m_lootSourceId, itemId, questItem.Count - before, questItem.Quests);
        before = questItem.Count;
    }
}

void ObjectService::ShareQuestItemTaken(uint32_t aSourceId, uint32_t aItemId, uint32_t aCount, const Vector<uint32_t>& acQuests) noexcept
{
    // Not for a quest done together: everyone keeps their own copy for when they do it
    const uint32_t questId = m_world.ctx().at<QuestService>().FindTeamedQuest(acQuests);
    if (!questId)
    {
        spdlog::info("Took quest item {:X} x{} from {:X}: not for a quest done together, the others keep theirs", aItemId, aCount, aSourceId);
        return;
    }

    auto& modSystem = m_world.GetModSystem();

    RequestQuestItemTaken request{};
    if (!modSystem.GetServerModId(aSourceId, request.SourceId) || !modSystem.GetServerModId(aItemId, request.ItemId) || !modSystem.GetServerModId(questId, request.QuestId))
    {
        // e.g. a body of an actor spawned at runtime (FF id): no id the other clients would know
        spdlog::warn("Quest item {:X} taken from {:X}, but they have no server id, can't remove it for the teammates", aItemId, aSourceId);
        return;
    }

    request.Count = aCount;
    m_transport.Send(request);

    spdlog::info("Took quest item {:X} x{} from {:X} for quest {:X} done together, removing it for the teammates", aItemId, aCount, aSourceId, questId);
}

void ObjectService::OnNotifyQuestItemTaken(const NotifyQuestItemTaken& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    auto* pSource = Cast<TESObjectREFR>(TESForm::GetById(modSystem.GetGameId(acMessage.SourceId)));
    if (!pSource)
    {
        spdlog::warn("{}: source {:X}:{:X} not found", __FUNCTION__, acMessage.SourceId.ModId, acMessage.SourceId.BaseId);
        return;
    }

    // Taken from the world: the item itself
    if (pSource->baseForm && pSource->baseForm->formID == modSystem.GetGameId(acMessage.ItemId) && IsPickupItem(pSource))
    {
        pSource->Disable();
        spdlog::info("A teammate picked up quest item {:X}, removed it here", pSource->formID);
        return;
    }

    Inventory::Entry entry{};
    entry.BaseId = acMessage.ItemId;
    entry.Count = -static_cast<int32_t>(acMessage.Count);

    ScopedInventoryOverride _;
    pSource->AddOrRemoveItem(entry);

    spdlog::info("Another player took quest item {:X} x{} from {:X}, removed it here", modSystem.GetGameId(acMessage.ItemId), acMessage.Count, pSource->formID);
}
#endif

void ObjectService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // TODO(cosideci): clear object components
}

void ObjectService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer->parentCell;

    // Player homes should not be synced, so that chest contents,
    // which are often used as storage, are never accidentally wiped.
#if TP_SKYRIM64
    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pCell))
        return;
#endif

    GameId cellId{};
    if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    GameId worldSpaceId{};
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId))
        {
            spdlog::error("Server world space id not found for world space form id {:X}", pWorldSpace->formID);
            return;
        }
    }

    Vector<FormType> formTypes = {FormType::Container, FormType::Door};
    // FormType::Door is 32 on Fallout 4 (verified against CommonLibF4's ENUM_FORM_ID)
    Vector<TESObjectREFR*> objects = pCell->GetRefsByFormTypes(formTypes);

    AssignObjectsRequest request{};

    for (TESObjectREFR* pObject : objects)
    {
#if TP_SKYRIM64
        if (!ShouldSyncObject(pObject))
        {
            spdlog::warn("Excluding sync for {:X}", pObject->formID);
            continue;
        }
#endif

        ObjectData objectData{};
        objectData.CellId = cellId;
        objectData.WorldSpaceId = worldSpaceId;
        objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

        if (!m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id))
        {
            spdlog::error("Server form id not found for object with form id {:X}", pObject->formID);
            continue;
        }

        if (Lock* pLock = pObject->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->IsLocked();
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

#if TP_SKYRIM64
        if (pObject->baseForm->formType == FormType::Container)
            objectData.CurrentInventory = pObject->GetInventory();
#endif
        // Fallout: container contents are per player, only their lock state is shared.

        request.Objects.push_back(objectData);
    }

    m_transport.Send(request);
}

void ObjectService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const ObjectData& objectData : acMessage.Objects)
    {
        const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(objectData.Id);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
        if (!pObject)
        {
            spdlog::error("Object not found for form id {:X}", objectData.Id);
            continue;
        }

        CreateObjectEntity(pObject->formID, objectData.ServerId);

        if (objectData.IsSenderFirst)
            continue;

        if (objectData.CurrentLockData != LockData{})
        {
            Lock* pLock = pObject->GetLock();

            if (!pLock)
            {
                pLock = pObject->CreateLock();
                if (!pLock)
                    continue;
            }

            pLock->lockLevel = objectData.CurrentLockData.LockLevel;
            pLock->SetLock(objectData.CurrentLockData.IsLocked);
            pObject->LockChange();
        }

#if TP_SKYRIM64
        if (pObject->baseForm->formType == FormType::Container)
            pObject->SetInventory(objectData.CurrentInventory);
#endif
    }
}

entt::entity ObjectService::CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();

    auto it = std::find_if(view.begin(), view.end(), [acServerId, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == acServerId; });

    if (it != view.end())
        return *it;

    entt::entity entity = m_world.create();
    spdlog::info("Created object entity, server id: {:X}, form id {:X}", acServerId, acFormId);

    m_world.emplace<FormIdComponent>(entity, acFormId);
    m_world.emplace<ObjectComponent>(entity, acServerId);

    return entity;
}

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (acEvent.ActivateFlag)
    {
#if TP_FALLOUT4
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing, acEvent.FromScript, acEvent.IsLooping);
#elif TP_SKYRIM64
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.Unk1, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);
#endif
    }

    if (!m_transport.IsConnected())
        return;

#if TP_FALLOUT4
    // Don't send back an activation we just applied for another player (it would bounce between the clients)
    if (const auto it = m_remotelyActivated.find(acEvent.pObject->formID); it != m_remotelyActivated.end() && std::chrono::steady_clock::now() - it->second < 1500ms)
        return;

    // A shared world item (dropped by a player): picking it up takes it for everyone
    if (const auto worldItem = m_refWorldItems.find(acEvent.pObject->formID); worldItem != m_refWorldItems.end())
    {
        if (acEvent.pActivator && acEvent.pActivator->formID == 0x14)
        {
            RequestItemPickedUp request{};
            request.ItemId = worldItem->second;
            m_transport.Send(request);

            spdlog::info("Picked up world item {} ({:X})", worldItem->second, acEvent.pObject->formID);
            m_worldItemRefs.erase(worldItem->second);
            m_refWorldItems.erase(worldItem);
        }
        return;
    }

    // A quest item picked up from the world: everyone has their own (their quests are their own) unless it's for a quest
    // the local player does with others, then it's gone for them too. Never replayed as an activation.
    if (IsPickupItem(acEvent.pObject) && acEvent.pActivator && acEvent.pActivator->formID == 0x14 && acEvent.pObject->baseForm)
    {
        Vector<uint32_t> quests;
        if (const ExtraDataList* pExtra = acEvent.pObject->GetExtraDataList())
            quests = pExtra->GetAliasQuests();

        // Already in the inventory by the time we hear of it: its stack carries the quest alias now
        if (quests.empty())
        {
            const auto items = PlayerCharacter::Get()->GetQuestItems();
            if (const auto it = items.find(acEvent.pObject->baseForm->formID); it != items.end())
                quests = it->second.Quests;
        }

        if (!quests.empty())
        {
            ShareQuestItemTaken(acEvent.pObject->formID, acEvent.pObject->baseForm->formID, 1, quests);
            return;
        }
    }

    if (IsPickupItem(acEvent.pObject) && !acEvent.pActivator->GetExtension()->IsPlayer())
        return;

    // Getting into power armor is synced by state instead (see RunPowerArmorUpdates): replaying the activation made
    // the other copies of the character try to get into the frame themselves.
    if (IsPowerArmorFrame(acEvent.pObject))
    {
        if (acEvent.pActivator && acEvent.pActivator->formID == 0x14)
            m_lastFrameId = acEvent.pObject->formID;
        return;
    }

    if (IsPerPlayerLoot(acEvent.pObject))
    {
        // Not synced, but remember what the local player is looting to keep quest items unique (see OnUpdate)
        if (acEvent.pActivator && acEvent.pActivator->formID == 0x14)
        {
            m_lootSourceId = acEvent.pObject->formID;
            m_lootSourceOpened = std::chrono::steady_clock::now();
            m_questItemsBeforeLooting.clear();
            for (const auto& [itemId, questItem] : PlayerCharacter::Get()->GetQuestItems())
                m_questItemsBeforeLooting[itemId] = questItem.Count;
        }
        return;
    }
#endif

    if (Lock* pLock = acEvent.pObject->GetLock())
    {
        if (pLock->flags & 0xFF)
        {
            spdlog::debug("Activation of {:X} not synced: it's locked", acEvent.pObject->formID);
            return;
        }
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->formID, request.Id))
    {
        spdlog::error("Server form id not found for object form id {:X}", acEvent.pObject->formID);
        return;
    }

    TESObjectCELL* pCell = acEvent.pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", acEvent.pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", acEvent.pObject->parentCell->formID);
        return;
    }

    auto view = m_world.view<FormIdComponent>();
    const auto pEntity = std::find_if(std::begin(view), std::end(view), [id = acEvent.pActivator->formID, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (pEntity == std::end(view))
    {
        spdlog::debug("Activation of {:X} not synced: activator {:X} has no entity", acEvent.pObject->formID, acEvent.pActivator->formID);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*pEntity);
    if (!serverIdRes.has_value())
        return;

    request.ActivatorId = serverIdRes.value();
    request.PreActivationOpenState = acEvent.PreActivationOpenState;

#if TP_FALLOUT4
    // Some activations reach us twice (activate event and hook) a few ms apart, send them once
    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_lastSent;
    const auto now = std::chrono::steady_clock::now();
    if (const auto it = s_lastSent.find(acEvent.pObject->formID); it != s_lastSent.end() && now - it->second < 250ms)
        return;
    s_lastSent[acEvent.pObject->formID] = now;
#endif

    m_transport.Send(request);
    spdlog::info("Sent activation of {:X} by {:X}", acEvent.pObject->formID, acEvent.pActivator->formID);
}

void ObjectService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActivatorId);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ActivatorId);
        return;
    }

    const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to activate.");
        return;
    }

#if TP_FALLOUT4
    // Never replay opening containers / looting bodies, or NPCs picking items up (e.g. from an older client)
    if (IsPerPlayerLoot(pObject) || (IsPickupItem(pObject) && !pActor->GetExtension()->IsPlayer()))
        return;

    m_remotelyActivated[pObject->formID] = std::chrono::steady_clock::now();
#endif

    if (pObject->baseForm->formType == FormType::Door)
    {
        auto remotePreActivationState = static_cast<TESObjectREFR::OpenState>(acMessage.PreActivationOpenState);

#if TP_FALLOUT4
        // Set the door to the state the other player's door ended up in rather than toggling it. Toggling relative to
        // our own state kept doors opposite forever once they got out of sync (e.g. someone standing in the doorway).
        const bool wasOpen = remotePreActivationState == TESObjectREFR::kOpen || remotePreActivationState == TESObjectREFR::kOpening;
        const bool wasClosed = remotePreActivationState == TESObjectREFR::kClosed || remotePreActivationState == TESObjectREFR::kClosing;
        if ((wasOpen || wasClosed) && World::Get().ctx().at<PapyrusService>().Get("ObjectReference", "SetOpen"))
        {
            const bool shouldBeOpen = wasClosed;
            const auto localState = pObject->GetOpenState();
            const bool isOpen = localState == TESObjectREFR::kOpen || localState == TESObjectREFR::kOpening;
            if (isOpen != shouldBeOpen)
            {
                using ObjectReference = TESObjectREFR;
                PAPYRUS_FUNCTION(void, ObjectReference, SetOpen, bool);
                s_pSetOpen(pObject, shouldBeOpen);
            }
            spdlog::info("Remote door {:X}: {} (was {} here)", pObject->formID, shouldBeOpen ? "open" : "closed", isOpen ? "open" : "closed");
            return;
        }
#endif

        TESObjectREFR::OpenState localState = pObject->GetOpenState();

        if (remotePreActivationState != localState)
        {
            // The doors are unsynced at this point. If we'll Activate the one on our side
            // it'll just continue to be unsynced (open remotely, closed locally and vice versa)
            return;
        }
    }

    // unsure if these flags are the best, but these are passed with the papyrus Activate fn
    // might be an idea to have the client send the flags through NotifyActivate
#if TP_FALLOUT4
    pObject->Activate(pActor, nullptr, 1, 0, 0, 0);
#elif TP_SKYRIM64
    pObject->Activate(pActor, 0, nullptr, 1, 0);
#endif
}

#if TP_FALLOUT4
void ObjectService::OnTerminalItemRun(const TerminalItemRunEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // Only a terminal the game's plugins place: one built in a settlement has another id in each game
    RequestTerminalItemRun request;
    request.MenuItemId = acEvent.MenuItemId;
    if (acEvent.TerminalRefId >= 0xFF000000 || !m_world.GetModSystem().GetServerModId(acEvent.TerminalRefId, request.TerminalRefId) ||
        !m_world.GetModSystem().GetServerModId(acEvent.TerminalFormId, request.TerminalFormId))
    {
        spdlog::debug("Terminal menu item {} of {:X} on {:X} not synced: no server id", acEvent.MenuItemId, acEvent.TerminalFormId, acEvent.TerminalRefId);
        return;
    }

    m_transport.Send(request);
    spdlog::info("Sent terminal menu item {} of {:X} run on {:X}", acEvent.MenuItemId, acEvent.TerminalFormId, acEvent.TerminalRefId);
}

void ObjectService::OnNotifyTerminalItemRun(const NotifyTerminalItemRun& acMessage) noexcept
{
    const uint32_t terminalRefId = m_world.GetModSystem().GetGameId(acMessage.TerminalRefId);
    const uint32_t terminalFormId = m_world.GetModSystem().GetGameId(acMessage.TerminalFormId);

    // Not loaded here (a reference of an unloaded cell), or another load order: nothing to run it on
    auto* pTerminal = Cast<TESObjectREFR>(TESForm::GetById(terminalRefId));
    const TESForm* pTerminalForm = TESForm::GetById(terminalFormId);
    if (!pTerminal || !pTerminalForm || pTerminalForm->formType != FormType::Terminal)
    {
        spdlog::info("Terminal menu item {} of {:X} run by player {:X} on {:X} not run here: not found", acMessage.MenuItemId, terminalFormId, acMessage.PlayerId, terminalRefId);
        return;
    }

    if (Terminals::RunMenuItem(pTerminal, terminalFormId, acMessage.MenuItemId))
        spdlog::info("Ran terminal menu item {} of {:X} on {:X} for player {:X}", acMessage.MenuItemId, terminalFormId, terminalRefId, acMessage.PlayerId);
}

void ObjectService::OnDestructionStage(const DestructionStageEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // Only an object the game's plugins place: one built in a settlement has another id in each game
    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.ObjectId));
    RequestDestructionStage request;
    if (!pObject || acEvent.ObjectId >= 0xFF000000 || !m_world.GetModSystem().GetServerModId(acEvent.ObjectId, request.ObjectId))
        return;

    // Read now (the frame after the change): the damage has been taken by then
    request.Stage = acEvent.NewStage;
    request.Health = Destruction::GetHealth(pObject);

    m_transport.Send(request);
    spdlog::info("Sent destruction stage {} (from {}) of {:X}, health {}", acEvent.NewStage, acEvent.OldStage, acEvent.ObjectId, request.Health);
}

void ObjectService::OnNotifyDestructionStage(const NotifyDestructionStage& acMessage) noexcept
{
    const uint32_t objectId = m_world.GetModSystem().GetGameId(acMessage.ObjectId);
    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(objectId));
    if (!pObject)
        return;

    // Already as damaged here (the same shot hit it here too): nothing to do
    const float health = Destruction::GetHealth(pObject);
    if (Destruction::DamageTo(pObject, acMessage.Health))
        spdlog::info("Destruction stage {} of {:X} from player {:X}: damaged from {} to {}", acMessage.Stage, objectId, acMessage.PlayerId, health, acMessage.Health);
}
#endif

void ObjectService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.FormId, request.Id))
    {
        spdlog::error("Server form id for lock object not found, form id: {:X}", acEvent.FormId);
        return;
    }

    const auto* const pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id for cell not found, cell form id: {:X}", pObject->parentCell->formID);
        return;
    }

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
}

void ObjectService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    auto* pLock = pObject->GetLock();

    // Unlocked already: nothing to do (no lock made just to say it's open, upstream fix #781)
    if (!acMessage.IsLocked && (!pLock || !pLock->IsLocked()))
        return;

    if (!pLock)
    {
        pLock = pObject->CreateLock();
        if (!pLock)
        {
            spdlog::error("Failed to create lock for object form id {:X}", pObject->formID);
            return;
        }
    }

    pLock->lockLevel = acMessage.LockLevel;
    pLock->SetLock(acMessage.IsLocked);
    pObject->LockChange();
}

void ObjectService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    ScriptAnimationRequest request{};
    request.FormID = acEvent.FormID;
    request.Animation = acEvent.Animation;
    request.EventName = acEvent.EventName;

    m_transport.Send(request);
}

void ObjectService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
#if TP_SKYRIM64
    if (acMessage.FormID == 0)
        return;

    auto* pForm = TESForm::GetById(acMessage.FormID);
    auto* pObject = Cast<TESObjectREFR>(pForm);

    if (!pObject)
    {
        spdlog::error("Failed to fetch notify script animation object, form id: {:X}", acMessage.FormID);
        return;
    }

    BSFixedString eventName(acMessage.EventName.c_str());
    if (acMessage.Animation == String{})
    {
        pObject->PlayAnimation(&eventName);
    }
    else
    {
        BSFixedString animation(acMessage.Animation.c_str());
        pObject->PlayAnimationAndWait(&animation, &eventName);
    }
#endif
}

BSTEventResult ObjectService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>* aDispatcher)
{
#if TP_FALLOUT4
    // The player's own activations (opening doors, picking up items, opening containers) don't go through the
    // activate hook on Fallout, only NPCs' do, so take them from the game's activate event instead.
    if (acEvent && acEvent->object && acEvent->actionRef && acEvent->actionRef->formID == 0x14)
    {
        ActivateEvent activateEvent(acEvent->object, PlayerCharacter::Get(), nullptr, 1, true, false, false);

        if (acEvent->object->baseForm && acEvent->object->baseForm->formType == FormType::Door)
        {
            // The event comes in before the door starts moving (two consecutive activations both read "open" when
            // the inverse was assumed), so the current state is the state before activation.
            const auto state = acEvent->object->GetOpenState();
            activateEvent.PreActivationOpenState = state;

            spdlog::info("Player activated door {:X}, state now {}, before {}", acEvent->object->formID, static_cast<int>(state), static_cast<int>(activateEvent.PreActivationOpenState));
        }

        OnActivate(activateEvent);
    }
#endif

#if ENVIRONMENT_DEBUG
    auto view = m_world.view<ObjectComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acEvent->object->formID, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == id; });

    if (itor == std::end(view))
    {
        AddObjectComponent(acEvent->object);
    }
#endif

    return BSTEventResult::kOk;
}

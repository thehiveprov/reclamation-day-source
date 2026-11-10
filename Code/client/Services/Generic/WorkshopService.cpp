#if TP_FALLOUT4

#include <Services/WorkshopService.h>

#include <World.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/RequestWorkshopChange.h>
#include <Services/TransportService.h>
#include <Games/TES.h>
#include <Games/ActorExtension.h>

#include <PlayerCharacter.h>
#include <SaveFolder.h>
#include <ExtraData/ExtraDataList.h>
#include <Forms/BGSKeyword.h>
#include <Forms/TESBoundObject.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

#include <fstream>
#include <sstream>
#include <PerfProbe.h>

namespace
{
// Fallout4.esm: links an object to the workbench of the settlement it belongs to
constexpr uint32_t kWorkshopItemKeyword = 0x54BA6;
// Form type of wires (bendable splines, BNDS)
constexpr uint8_t kBendableSplineType = 0x35;
// Exterior cells are 4096 units wide; spots this close to the player are in cells it has loaded (the game keeps the
// 5 x 5 cells around the player's loaded, so 1.5 cells away is loaded wherever the player stands in its cell)
constexpr float kExteriorCellSize = 4096.f;
constexpr float kLoadedRadius = 1.5f * kExteriorCellSize;
// How far apart two copies of an object can be and still be the same object
constexpr float kSameSpot = 1.f;
// About where the player's eyes are above its feet
constexpr float kEyeHeight = 120.f;

using Clock = std::chrono::steady_clock;

uint64_t PendingKey(const NotifyWorkshopChange& acMessage) noexcept
{
    // Built while connected: by item id, tagged so it can't meet an object id (Fallout4.esm's objects have mod id 0)
    if (acMessage.ItemId)
        return (1ull << 63) | acMessage.ItemId;
    return (static_cast<uint64_t>(acMessage.RefId.ModId) << 32) | acMessage.RefId.BaseId;
}

const char* ChangeName(uint8_t aType) noexcept
{
    switch (aType)
    {
    case RequestWorkshopChange::kPlaced: return "built";
    case RequestWorkshopChange::kMoved: return "moved";
    case RequestWorkshopChange::kRemoved: return "scrapped";
    case RequestWorkshopChange::kRestored: return "restored";
    default: return "changed";
    }
}

// The world's own object scrapped earlier, back: TESForm::SetDelete(false) (vtable slot 0x28; TESObjectREFR's, 1.10.163
// RVA 0x3F3CF0, hands an undelete to TESForm's 0x153D20, which clears the deleted flag 0x20 and marks the form changed
// so the save keeps it), enabled, where it stood
void RestoreRef(TESObjectREFR* apRef, const NiPoint3& acPosition, const NiPoint3& acRotation, float aScale) noexcept
{
    using TSetDelete = void(TESObjectREFR*, bool);
    (*reinterpret_cast<TSetDelete***>(apRef))[0x28](apRef, false);
    apRef->Enable();
    apRef->SetTransform(acPosition, acRotation, aScale);
}

NiPoint3 ToPoint(const std::array<float, 3>& acValues) noexcept
{
    return NiPoint3(glm::vec3(acValues[0], acValues[1], acValues[2]));
}

std::string NameOf(const TESForm* apForm) noexcept
{
    const char* pName = apForm ? apForm->GetName() : nullptr;
    return pName && *pName ? pName : "an object";
}

bool IsWire(const TESObjectREFR* apRef) noexcept
{
    return apRef && apRef->baseForm && static_cast<uint8_t>(apRef->baseForm->formType) == kBendableSplineType;
}

bool IsCharacterBase(const TESForm* apBase) noexcept
{
    return apBase && (apBase->formType == FormType::Npc || apBase->formType == FormType::LeveledCharacter);
}

// ExtraPowerLinks: what the reference is connected to (form id, link type). A wire's are the two objects it connects,
// an object's are its wires (and what it touches, for objects that connect without wires).
Vector<std::pair<uint32_t, uint32_t>> GetPowerLinks(TESObjectREFR* apRef) noexcept
{
    Vector<std::pair<uint32_t, uint32_t>> links;
    ExtraDataList* pExtra = apRef ? apRef->GetExtraDataList() : nullptr;
    const auto* pData = pExtra ? reinterpret_cast<const uint8_t*>(pExtra->GetByType(ExtraDataType::PowerLinks)) : nullptr;
    if (!pData)
        return links;

    // BSTArray<{formID, linkType}> at 0x18: data, capacity, size at 0x28
    const auto* pEntries = *reinterpret_cast<const uint32_t* const*>(pData + 0x18);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pData + 0x28);
    for (uint32_t i = 0; pEntries && i < count && i < 64; ++i)
        links.emplace_back(pEntries[i * 2], pEntries[i * 2 + 1]);
    return links;
}

// ExtraBendableSplineParams: slack at 0x18, thickness at 0x1C
float* GetWireThickness(TESObjectREFR* apWire) noexcept
{
    ExtraDataList* pExtra = apWire ? apWire->GetExtraDataList() : nullptr;
    auto* pData = pExtra ? reinterpret_cast<uint8_t*>(pExtra->GetByType(ExtraDataType::BendableSplineParams)) : nullptr;
    return pData ? reinterpret_cast<float*>(pData + 0x1C) : nullptr;
}

// The settlement's Workshop::ExtraData (power grids) on its workbench
BSExtraData* GetWorkshopData(TESObjectREFR* apWorkshop) noexcept
{
    ExtraDataList* pExtra = apWorkshop ? apWorkshop->GetExtraDataList() : nullptr;
    return pExtra ? pExtra->GetByType(ExtraDataType::Workshop) : nullptr;
}

// Workshop::ContextData: where the player looks from, what the game's workshop functions take
struct WorkshopContext
{
    float LookPosition[3];
    float LookDirection[3];
    float Heading;
    TESObjectCELL* pCell;
    TESWorldSpace* pWorldSpace;
};
static_assert(sizeof(WorkshopContext) == 0x30);

WorkshopContext MakeContext() noexcept
{
    // Workshop::ContextData::ContextData(Actor*) (F4SE 0x1F8830, id 1523699)
    WorkshopContext context{};
    using TContext = WorkshopContext*(WorkshopContext*, Actor*);
    POINTER_FALLOUT4(TContext, s_context, 1523700);
    s_context.Get()(&context, PlayerCharacter::Get());
    return context;
}

// The game's wire and power functions work on Workshop::hCurrentWorkshop (the workbench in build mode): pointed at the
// settlement for the time of a change, then back (as F4SE's AttachWire does)
class ScopedCurrentWorkshop
{
public:
    explicit ScopedCurrentWorkshop(TESObjectREFR* apWorkshop) noexcept
    {
        // Workshop::hCurrentWorkshop (F4SE 0x58DFDBC, id 737927), a BSPointerHandle
        POINTER_FALLOUT4(uint32_t, s_currentWorkshop, 737928);
        m_pHandle = s_currentWorkshop.Get();
        m_previous = *m_pHandle;
        *m_pHandle = apWorkshop->GetHandle().handle.iBits;
    }
    ~ScopedCurrentWorkshop() noexcept { *m_pHandle = m_previous; }

    ScopedCurrentWorkshop(const ScopedCurrentWorkshop&) = delete;
    ScopedCurrentWorkshop& operator=(const ScopedCurrentWorkshop&) = delete;

private:
    uint32_t* m_pHandle;
    uint32_t m_previous;
};

// The game's own (1.10.163 addresses from F4SE 0.6.23's GameWorkshop.cpp, mapped to ids; the game's wire placement in
// Workshop::PlaceCurrentReference calls them in this order)
void UpdateMovingWirelessItem(TESObjectREFR* apRef, BSExtraData* apWorkshopData) noexcept
{
    // PowerUtils::UpdateMovingWirelessItem (0x201B10, id 958345): connects what it touches
    using TUpdate = void(TESObjectREFR*, BSExtraData*);
    POINTER_FALLOUT4(TUpdate, s_update, 958346);
    s_update.Get()(apRef, apWorkshopData);
}

void ConnectWire(WorkshopContext& aContext, TESObjectREFR* apWire, TESObjectREFR* apEndA, TESObjectREFR* apEndB, BSExtraData* apWorkshopData) noexcept
{
    // SplineUtils::UpdateSpline (0x200B50, id 1150803): the wire's shape between its ends
    using TUpdateSpline = void(WorkshopContext*, TESObjectREFR*, TESObjectREFR*, int32_t, TESObjectREFR*, int32_t);
    POINTER_FALLOUT4(TUpdateSpline, s_updateSpline, 1150804);
    // SplineUtils::ConnectSpline (0x200E50, id 59311): the power links between the wire and its ends
    using TConnectSpline = void(TESObjectREFR*, int32_t, TESObjectREFR*, int32_t, TESObjectREFR*);
    POINTER_FALLOUT4(TConnectSpline, s_connectSpline, 59312);
    // Workshop::ExtraData::AddItem (0x1F6890, id 1030944) and AddConnection (0x1F6E20, id 523652): the power grid
    using TAddItem = void(BSExtraData*, TESObjectREFR*);
    POINTER_FALLOUT4(TAddItem, s_addItem, 1030945);
    using TAddConnection = void(BSExtraData*, TESObjectREFR*, TESObjectREFR*, TESObjectREFR*);
    POINTER_FALLOUT4(TAddConnection, s_addConnection, 523653);
    // TerminalUtils::EstablishTerminalLinks (0x204610, id 608512): a terminal wired to what it controls
    using TEstablishTerminalLinks = void(TESObjectREFR*);
    POINTER_FALLOUT4(TEstablishTerminalLinks, s_establishTerminalLinks, 608513);

    s_updateSpline.Get()(&aContext, apWire, apEndB, 0, apEndA, 0);
    s_connectSpline.Get()(apEndA, 0, apEndB, 0, apWire);
    s_addItem.Get()(apWorkshopData, apWire);
    s_addConnection.Get()(apWorkshopData, apEndA, apEndB, apWire);
    UpdateMovingWirelessItem(apEndA, apWorkshopData);
    UpdateMovingWirelessItem(apEndB, apWorkshopData);
    s_establishTerminalLinks.Get()(apWire);
}

void RedrawWire(WorkshopContext& aContext, TESObjectREFR* apWire) noexcept
{
    // Called for a moved object's wires (id 73225): UpdateSpline with the ends the wire is linked to
    using TRedraw = bool(WorkshopContext*, TESObjectREFR*);
    POINTER_FALLOUT4(TRedraw, s_redraw, 73226);
    s_redraw.Get()(&aContext, apWire);
}

void DisconnectWire(TESObjectREFR* apWire) noexcept
{
    // SplineUtils::DisconnectSpline (CommonLibF4 id 750682, what the game's scrap uses for wires)
    using TDisconnect = void(TESObjectREFR*);
    POINTER_FALLOUT4(TDisconnect, s_disconnect, 750683);
    s_disconnect.Get()(apWire);
}

// Settlement budget: the workbench's WorkshopCurrentTriangles and WorkshopCurrentDraws. Read from the game's code
// (1.10.163): in build mode the game keeps them in counters, filled from the workbench when entering (id 923255);
// placing an object adds its cost (1002706, from PlaceCurrentReference), scrapping one takes it off (1161918, from
// ScrapReference, for anything scrapped), each writing the counters back to the workbench of Workshop::hCurrentWorkshop.
// The cost comes from the object's 3D (triangles and draws). What other players build or scrap never went through
// those here: called the same way for it, every game's budget counts everyone's objects.
void ChangeBudget(TESObjectREFR* apItem, TESObjectREFR* apWorkshop, bool aAdd, TESObjectREFR* apBuildWorkshop) noexcept
{
    using TLoad = void(TESObjectREFR*);
    POINTER_FALLOUT4(TLoad, s_loadBudget, 923256);
    using TChange = void(TESObjectREFR*);
    POINTER_FALLOUT4(TChange, s_addToBudget, 1002707);
    POINTER_FALLOUT4(TChange, s_removeFromBudget, 1161919);

    {
        ScopedCurrentWorkshop _{apWorkshop};
        s_loadBudget.Get()(apWorkshop);
        if (aAdd)
            s_addToBudget.Get()(apItem);
        else
            s_removeFromBudget.Get()(apItem);
    }

    // The counters are the local player's own while they're in build mode at another settlement
    if (apBuildWorkshop && apBuildWorkshop != apWorkshop)
        s_loadBudget.Get()(apBuildWorkshop);
}

float GetBudgetValue(TESObjectREFR* apWorkshop, uint32_t aActorValue) noexcept
{
    return apWorkshop->actorValueOwner.GetValue(apWorkshop->GetActorValueInfo(aActorValue));
}
} // namespace

WorkshopService::WorkshopService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WorkshopService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&WorkshopService::OnDisconnected>(this);
    m_workshopChangeConnection = aDispatcher.sink<NotifyWorkshopChange>().connect<&WorkshopService::OnNotifyWorkshopChange>(this);
    m_settlementClaimsConnection = aDispatcher.sink<NotifySettlementClaims>().connect<&WorkshopService::OnNotifySettlementClaims>(this);
    m_campsConnection = aDispatcher.sink<NotifyCamps>().connect<&WorkshopService::OnNotifyCamps>(this);
    m_settlementRatingsConnection = aDispatcher.sink<NotifySettlementRatings>().connect<&WorkshopService::OnNotifySettlementRatings>(this);
    m_settlementProductionConnection = aDispatcher.sink<NotifySettlementProduction>().connect<&WorkshopService::OnNotifySettlementProduction>(this);
}

WorkshopService::~WorkshopService() noexcept
{
    if (!m_eventsRegistered)
        return;

    // Workshop::UnregisterForItemPlaced / ItemMoved / ItemDestroyed / WorkshopModeEvent (CommonLibF4 ids 583255,
    // 569432, 1101379, 9236)
    using TUnregister = void(void*);
    POINTER_FALLOUT4(TUnregister, s_unregisterItemPlaced, 583256);
    POINTER_FALLOUT4(TUnregister, s_unregisterItemMoved, 569433);
    POINTER_FALLOUT4(TUnregister, s_unregisterItemDestroyed, 1101380);
    POINTER_FALLOUT4(TUnregister, s_unregisterWorkshopMode, 9237);

    s_unregisterItemPlaced.Get()(static_cast<BSTEventSink<Workshop::ItemPlacedEvent>*>(this));
    s_unregisterItemMoved.Get()(static_cast<BSTEventSink<Workshop::ItemMovedEvent>*>(this));
    s_unregisterItemDestroyed.Get()(static_cast<BSTEventSink<Workshop::ItemDestroyedEvent>*>(this));
    s_unregisterWorkshopMode.Get()(static_cast<BSTEventSink<Workshop::WorkshopModeEvent>*>(this));
}

void WorkshopService::RegisterEvents() noexcept
{
    // Workshop::RegisterForItemPlaced / ItemMoved / ItemDestroyed / WorkshopModeEvent (CommonLibF4 ids 849008, 835323,
    // 1367004, 275798; checked on 1.10.163: each gets its event source and registers the sink with it)
    using TRegister = void(void*);
    POINTER_FALLOUT4(TRegister, s_registerItemPlaced, 849009);
    POINTER_FALLOUT4(TRegister, s_registerItemMoved, 835324);
    POINTER_FALLOUT4(TRegister, s_registerItemDestroyed, 1367005);
    POINTER_FALLOUT4(TRegister, s_registerWorkshopMode, 275799);

    s_registerItemPlaced.Get()(static_cast<BSTEventSink<Workshop::ItemPlacedEvent>*>(this));
    s_registerItemMoved.Get()(static_cast<BSTEventSink<Workshop::ItemMovedEvent>*>(this));
    s_registerItemDestroyed.Get()(static_cast<BSTEventSink<Workshop::ItemDestroyedEvent>*>(this));
    s_registerWorkshopMode.Get()(static_cast<BSTEventSink<Workshop::WorkshopModeEvent>*>(this));

    m_eventsRegistered = true;
    spdlog::info("Listening to settlement building");
}

BSTEventResult WorkshopService::OnEvent(const Workshop::ItemPlacedEvent* acEvent, const EventDispatcher<Workshop::ItemPlacedEvent>*)
{
    // Only what the player builds in build mode: objects scripts place (a mod building a settlement by itself) come
    // through here too
    if (acEvent && m_inBuildMode)
        QueueLocalChange(RequestWorkshopChange::kPlaced, acEvent->pWorkshop, acEvent->pPlacedItem);
    return BSTEventResult::kOk;
}

BSTEventResult WorkshopService::OnEvent(const Workshop::ItemMovedEvent* acEvent, const EventDispatcher<Workshop::ItemMovedEvent>*)
{
    if (acEvent && m_inBuildMode)
        QueueLocalChange(RequestWorkshopChange::kMoved, acEvent->pWorkshop, acEvent->pMovedItem);
    return BSTEventResult::kOk;
}

BSTEventResult WorkshopService::OnEvent(const Workshop::ItemDestroyedEvent* acEvent, const EventDispatcher<Workshop::ItemDestroyedEvent>*)
{
    if (acEvent && m_inBuildMode)
        QueueLocalChange(RequestWorkshopChange::kRemoved, acEvent->pWorkshop, acEvent->pDestroyedItem);
    return BSTEventResult::kOk;
}

BSTEventResult WorkshopService::OnEvent(const Workshop::WorkshopModeEvent* acEvent, const EventDispatcher<Workshop::WorkshopModeEvent>*)
{
    if (acEvent)
    {
        m_inBuildMode = acEvent->Start;
        m_buildWorkshopId = acEvent->Start && acEvent->pWorkshop ? acEvent->pWorkshop->formID : 0;
        // Claims: the rights here from now (the game asks about every object as build mode starts), the next update
        // asks to claim it or ends build mode (CheckBuildModeClaim)
        if (acEvent->Start && m_transport.IsConnected())
            s_buildRights = GetRightsAt(m_buildWorkshopId);
        s_inBuildMode = acEvent->Start;
        m_claimCheckPending = true;
        spdlog::info("Build mode {} at {:X}", acEvent->Start ? "entered" : "left", acEvent->pWorkshop ? acEvent->pWorkshop->formID : 0);
    }
    return BSTEventResult::kOk;
}

void WorkshopService::ShareMove(const TESObjectREFR* apItem) noexcept
{
    if (!apItem || !m_buildWorkshopId)
        return;

    // The build tools record their moves themselves (several changes to an object in a row are one)
    std::scoped_lock _{m_localChangesLock};
    m_localChanges.push_back({RequestWorkshopChange::kMoved, m_buildWorkshopId, apItem->formID, false});
}

TESObjectREFR* WorkshopService::GetOutlinedObject() noexcept
{
    // Workshop::PlacementItem (CommonLibF4 id 526727): a reference handle
    POINTER_FALLOUT4(uint32_t, s_placementItem, 526728);
    const uint32_t handle = *s_placementItem.Get();
    if (!handle || handle == *TESObjectREFR::GetNullHandle())
        return nullptr;
    return TESObjectREFR::GetByHandle(handle);
}

WorkshopService::ObjectTransform WorkshopService::GetTransform(const TESObjectREFR* apItem) noexcept
{
    return {apItem->position, apItem->rotation, apItem->GetScale()};
}

void WorkshopService::NoteScrappedCharacter(const TESObjectREFR* apActor) noexcept
{
    if (!apActor)
        return;

    const uint32_t serverId = FindCharacterServerId(apActor->formID);
    if (!serverId)
        return;

    std::scoped_lock _{m_localChangesLock};
    m_scrappedBodies[apActor->formID] = serverId;
}

uint32_t WorkshopService::FindCharacterServerId(uint32_t aFormId) const noexcept
{
    const auto view = m_world.view<FormIdComponent>();
    for (const auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id != aFormId)
            continue;
        if (const auto* pLocal = m_world.try_get<LocalComponent>(entity))
            return pLocal->Id;
        if (const auto* pRemote = m_world.try_get<RemoteComponent>(entity))
            return pRemote->Id;
    }
    return 0;
}

uint32_t WorkshopService::FindCharacterFormId(uint32_t aServerId) const noexcept
{
    const auto view = m_world.view<FormIdComponent>();
    for (const auto entity : view)
    {
        const auto* pLocal = m_world.try_get<LocalComponent>(entity);
        const auto* pRemote = m_world.try_get<RemoteComponent>(entity);
        if ((pLocal && pLocal->Id == aServerId) || (pRemote && pRemote->Id == aServerId))
            return view.get<FormIdComponent>(entity).Id;
    }
    return 0;
}

void WorkshopService::QueueLocalChange(uint8_t aType, const TESObjectREFR* apWorkshop, const TESObjectREFR* apItem) noexcept
{
    if (!apWorkshop || !apItem)
        return;

    std::scoped_lock _{m_localChangesLock};
    m_localChanges.push_back({aType, apWorkshop->formID, apItem->formID});
}

void WorkshopService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfProbe::Section _perf("Workshop");
    if (!m_eventsRegistered)
        RegisterEvents();

    Vector<LocalChange> changes;
    {
        std::scoped_lock _{m_localChangesLock};
        std::swap(changes, m_localChanges);
    }

    for (const LocalChange& change : changes)
        RecordChange(change);
    TrackOutlined();
    RunPendingBudget();

    if (m_claimCheckPending.exchange(false))
        CheckBuildModeClaim(true);

    // Before the connection check: offline, a held day is let go
    RunSettlementDay();

    if (!m_transport.IsConnected())
        return;

    RunCamps();

    LoadItemMap();
    if (m_itemMapDirty && Clock::now() - m_itemMapSavedAt > 3s)
        SaveItemMap();

    // Those not ready yet first, they came first
    const auto now = Clock::now();
    for (auto it = m_deferredChanges.begin(); it != m_deferredChanges.end();)
    {
        if (SendLocalChange(it->Change))
            it = m_deferredChanges.erase(it);
        else if (now > it->GiveUpAt)
        {
            spdlog::warn("{} {:X} at workshop {:X}: still not ready to share after 15 s, the others won't get it", ChangeName(it->Change.Type), it->Change.ItemFormId,
                         it->Change.WorkshopFormId);
            it = m_deferredChanges.erase(it);
        }
        else
            ++it;
    }

    for (const LocalChange& change : changes)
    {
        if (!SendLocalChange(change))
            m_deferredChanges.push_back({change, now + 15s});
    }

    RunPendingChanges();
}

bool WorkshopService::FillWireEnds(const TESObjectREFR* apWire, RequestWorkshopChange& aRequest) const noexcept
{
    const auto links = GetPowerLinks(const_cast<TESObjectREFR*>(apWire));
    if (links.size() < 2)
        return false;

    auto& modSystem = m_world.GetModSystem();
    for (size_t end = 0; end < 2; ++end)
    {
        const uint32_t formId = links[end].first;
        if (const auto it = m_refItems.find(formId); it != m_refItems.end())
            aRequest.EndItemIds[end] = it->second;
        // Built here a moment ago: its item id is on its way
        else if (formId >= 0xFF000000)
            return false;
        else if (!modSystem.GetServerModId(formId, aRequest.EndRefIds[end]))
            return false;
    }

    if (const float* pThickness = GetWireThickness(const_cast<TESObjectREFR*>(apWire)))
        aRequest.WireThickness = *pThickness;

    spdlog::info("Wire {:X} connects {:X} and {:X} (link types {}, {})", apWire->formID, links[0].first, links[1].first, links[0].second, links[1].second);
    return true;
}

bool WorkshopService::SendLocalChange(const LocalChange& acChange) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    RequestWorkshopChange request{};
    request.ChangeType = acChange.Type;

    if (!GetSharedWorkshopId(acChange.WorkshopFormId, request.WorkshopId))
    {
        spdlog::warn("{} {:X} at workshop {:X}: that workbench was made in this game (e.g. placed by a mod), sharing its settlement isn't supported yet", ChangeName(acChange.Type),
                     acChange.ItemFormId, acChange.WorkshopFormId);
        return true;
    }

    // A dead body (build tools): known by its character
    if (acChange.Type == RequestWorkshopChange::kRemoved)
    {
        std::scoped_lock _{m_localChangesLock};
        if (const auto bodyIt = m_scrappedBodies.find(acChange.ItemFormId); bodyIt != m_scrappedBodies.end())
        {
            request.ActorServerId = bodyIt->second;
            m_scrappedBodies.erase(bodyIt);
        }
    }

    // Which object, for the other players
    const auto itemIt = m_refItems.find(acChange.ItemFormId);
    if (itemIt != m_refItems.end())
        request.ItemId = itemIt->second;
    else if (acChange.Type != RequestWorkshopChange::kPlaced && !modSystem.GetServerModId(acChange.ItemFormId, request.RefId) && !request.ActorServerId)
    {
        spdlog::warn("{} {:X} at workshop {:X}: it was built before connecting, the other players don't know it", ChangeName(acChange.Type), acChange.ItemFormId, acChange.WorkshopFormId);
        return true;
    }

    if (acChange.Type == RequestWorkshopChange::kRemoved)
    {
        if (request.ItemId)
        {
            UnmapItem(request.ItemId);
        }

        m_transport.Send(request);
        spdlog::info("Scrapped {:X} (item {}, character {:X}) at workshop {:X}, sharing it", acChange.ItemFormId, request.ItemId, request.ActorServerId, acChange.WorkshopFormId);
        return true;
    }

    auto* pItem = Cast<TESObjectREFR>(TESForm::GetById(acChange.ItemFormId));
    if (!pItem || !pItem->baseForm)
    {
        spdlog::warn("{} {:X} at workshop {:X}: the object is gone already", ChangeName(acChange.Type), acChange.ItemFormId, acChange.WorkshopFormId);
        return true;
    }

    // A turret (a character): the character sync brings it to the others and moves it; it's shared by its character
    // so they can make it part of the settlement
    if (IsCharacterBase(pItem->baseForm))
    {
        if (acChange.Type != RequestWorkshopChange::kPlaced)
            return true;

        request.ActorServerId = FindCharacterServerId(pItem->formID);
        if (!request.ActorServerId)
            return false;
    }

    // A wire: by the two objects it connects. It moves with them.
    if (IsWire(pItem))
    {
        if (acChange.Type != RequestWorkshopChange::kPlaced)
            return true;
        if (!FillWireEnds(pItem, request))
            return false;
    }

    if (!modSystem.GetServerModId(pItem->baseForm->formID, request.BaseId))
    {
        spdlog::warn("{} {:X} at workshop {:X}: its base {:X} was made in this game, the other players can't build it", ChangeName(acChange.Type), pItem->formID, acChange.WorkshopFormId,
                     pItem->baseForm->formID);
        return true;
    }

    if (TESObjectCELL* pCell = pItem->GetParentCellEx())
        modSystem.GetServerModId(pCell->formID, request.CellId);
    if (TESWorldSpace* pWorldSpace = pItem->GetWorldSpace())
        modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId);

    request.Position = {pItem->position.x, pItem->position.y, pItem->position.z};
    request.Rotation = {pItem->rotation.x, pItem->rotation.y, pItem->rotation.z};
    request.Scale = pItem->GetScale();
    if (acChange.Type == RequestWorkshopChange::kPlaced)
        request.LocalRefId = pItem->formID;

    m_transport.Send(request);

    spdlog::info("{} {:X} ({:X}, item {}) at workshop {:X}, sharing it: ({:.1f}, {:.1f}, {:.1f}) scale {:.2f}", ChangeName(acChange.Type), pItem->formID, pItem->baseForm->formID,
                 request.ItemId, acChange.WorkshopFormId, request.Position[0], request.Position[1], request.Position[2], request.Scale);
    return true;
}

void WorkshopService::OnNotifyWorkshopChange(const NotifyWorkshopChange& acMessage) noexcept
{
    LoadItemMap();

    // Our own build coming back with its item id
    if (acMessage.ChangeType == RequestWorkshopChange::kPlaced && acMessage.PlayerId == m_transport.GetLocalPlayerId() && acMessage.LocalRefId)
    {
        MapItem(acMessage.ItemId, acMessage.LocalRefId);
        return;
    }

    // A newer change replaces one still waiting for its spot to load; moving or scrapping an object that is still
    // waiting to be built moves it before it's built, or cancels it
    const uint64_t key = PendingKey(acMessage);
    const auto pending = m_pendingChanges.find(key);
    if (pending != m_pendingChanges.end())
    {
        NotifyWorkshopChange& waiting = pending.value();
        if (waiting.ChangeType == RequestWorkshopChange::kPlaced && acMessage.ChangeType == RequestWorkshopChange::kMoved)
        {
            waiting.Position = acMessage.Position;
            waiting.Rotation = acMessage.Rotation;
            waiting.Scale = acMessage.Scale;
            waiting.CellId = acMessage.CellId;
            waiting.WorldSpaceId = acMessage.WorldSpaceId;
            return;
        }
        if (waiting.ChangeType == RequestWorkshopChange::kPlaced && acMessage.ChangeType == RequestWorkshopChange::kRemoved)
        {
            m_pendingChanges.erase(pending);
            return;
        }
        m_pendingChanges.erase(pending);
    }

    if (!ApplyChange(acMessage))
        m_pendingChanges[key] = acMessage;
}

bool WorkshopService::ApplyChange(const NotifyWorkshopChange& acMessage) noexcept
{
    switch (acMessage.ChangeType)
    {
    case RequestWorkshopChange::kPlaced: return ApplyPlaced(acMessage);

    case RequestWorkshopChange::kMoved:
    {
        TESObjectREFR* pRef = FindChangedRef(acMessage);
        if (!pRef)
            // An object built while connected that never made it here can't be moved; one that comes with the game,
            // or that this game has (remembered from an earlier session), is moved once its cell is loaded
            return acMessage.ItemId != 0 && !IsRememberedElsewhere(acMessage);

        // Characters (turrets) are moved by the character sync
        if (pRef->formType == FormType::Character)
            return true;

        pRef->SetTransform(ToPoint(acMessage.Position), ToPoint(acMessage.Rotation), acMessage.Scale);
        UpdatePower(pRef, GetWorkshopRef(acMessage.WorkshopId));
        spdlog::info("Player {} moved {:X} (item {}) at workshop {:X}:{:X}", acMessage.PlayerId, pRef->formID, acMessage.ItemId, acMessage.WorkshopId.ModId, acMessage.WorkshopId.BaseId);
        return true;
    }

    case RequestWorkshopChange::kRemoved:
    {
        TESObjectREFR* pRef = FindChangedRef(acMessage);
        if (!pRef)
        {
            // This game has it (scrapped while this player was away) but its cell isn't loaded: once it is
            if (acMessage.ItemId && IsRememberedElsewhere(acMessage))
                return false;
            // A body's character is removed by the server too (its copy here may be gone already)
            return acMessage.ItemId != 0 || acMessage.ActorServerId != 0;
        }

        // The workbench the local player is in build mode at (a C.A.M.P.'s module, its C.A.M.P. moved or removed):
        // out of build mode first
        if (m_inBuildMode && pRef->formID == m_buildWorkshopId)
        {
            LeaveBuildMode();
            return false;
        }

        if (acMessage.ItemId)
            UnmapItem(acMessage.ItemId);

        // A turret: its character is removed by the server, the character sync takes its copy away here
        if (acMessage.ItemId && pRef->formType == FormType::Character)
            return true;

        auto* pWorkshop = GetWorkshopRef(acMessage.WorkshopId);
        DisconnectWires(pRef, pWorkshop);
        // Its cost off the budget, as the scrapper's game took it off theirs (not counted yet: nothing to take off)
        if (pWorkshop && !ForgetPendingBudget(pRef->formID))
            ApplyBudget(pRef, pWorkshop, false);
        pRef->Disable();
        pRef->Delete();
        spdlog::info("Player {} scrapped {:X} (item {}) at workshop {:X}:{:X}", acMessage.PlayerId, pRef->formID, acMessage.ItemId, acMessage.WorkshopId.ModId, acMessage.WorkshopId.BaseId);
        return true;
    }

    case RequestWorkshopChange::kRestored:
    {
        // The world's own object, back (its scrap undone): once its cell is loaded here
        TESObjectREFR* pRef = FindChangedRef(acMessage);
        if (!pRef)
            return false;
        RestoreRef(pRef, ToPoint(acMessage.Position), ToPoint(acMessage.Rotation), acMessage.Scale);
        spdlog::info("Player {} brought back {:X} (its scrap undone) at workshop {:X}:{:X}", acMessage.PlayerId, pRef->formID, acMessage.WorkshopId.ModId, acMessage.WorkshopId.BaseId);
        return true;
    }

    default: return true;
    }
}

bool WorkshopService::ApplyPlaced(const NotifyWorkshopChange& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();
    const NiPoint3 position = ToPoint(acMessage.Position);
    const NiPoint3 rotation = ToPoint(acMessage.Rotation);

    // Built here already (the server sends the settlement again after reconnecting, or this game has it from an
    // earlier session): just put it where it is now
    if (const auto it = m_itemRefs.find(acMessage.ItemId); it != m_itemRefs.end())
    {
        if (auto* pRef = GetMappedRef(acMessage.ItemId))
        {
            // Wires are where their objects are, turrets where the character sync has them
            if (!IsWire(pRef) && pRef->formType != FormType::Character)
                pRef->SetTransform(position, rotation, acMessage.Scale);
            return true;
        }

        // Its cell isn't loaded: once it is
        if (IsRememberedElsewhere(acMessage))
            return false;

        // Gone from this game (not saved, or scrapped): built again below
        UnmapItem(acMessage.ItemId);
    }

    TESForm* pBase = TESForm::GetById(modSystem.GetGameId(acMessage.BaseId));
    // A C.A.M.P.'s module is the workbench of its C.A.M.P., part of none
    const bool campModule = pBase && GetCampModuleId() && pBase->formID == GetCampModuleId();
    auto* pWorkshop = campModule ? nullptr : GetWorkshopRef(acMessage.WorkshopId);
    if (!pBase || (!pWorkshop && !campModule))
    {
        // A C.A.M.P.'s objects wait for its module to be built here
        if (pBase && Camp::IsCamp(acMessage.WorkshopId))
            return false;

        spdlog::warn("{}: item {} ({:X}:{:X} at workshop {:X}:{:X}) not found here, is the load order the same?", __FUNCTION__, acMessage.ItemId, acMessage.BaseId.ModId, acMessage.BaseId.BaseId,
                     acMessage.WorkshopId.ModId, acMessage.WorkshopId.BaseId);
        return true;
    }

    // A turret: the character sync brings it
    if (acMessage.ActorServerId)
        return ApplyCharacterPlaced(acMessage, pWorkshop);

    if (!IsLoadedHere(acMessage))
        return false;

    if (static_cast<uint8_t>(pBase->formType) == kBendableSplineType)
        return ApplyWire(acMessage, pBase, pWorkshop);

    const bool interior = acMessage.WorldSpaceId == GameId{};
    auto* pWorldSpace = interior ? nullptr : Cast<TESWorldSpace>(TESForm::GetById(modSystem.GetGameId(acMessage.WorldSpaceId)));
    TESObjectCELL* pCell = interior ? Cast<TESObjectCELL>(TESForm::GetById(modSystem.GetGameId(acMessage.CellId)))
                                    : ModManager::Get()->GetCellFromCoordinates(static_cast<int32_t>(std::floor(position.x / kExteriorCellSize)),
                                                                                static_cast<int32_t>(std::floor(position.y / kExteriorCellSize)), pWorldSpace, false);

    // The same object already standing there (this game has it from an earlier visit, saved with the game): use it
    // rather than building a second one
    TESObjectREFR* pRef = nullptr;
    if (pCell)
    {
        for (TESObjectREFR* pCandidate : pCell->objects)
        {
            if (!pCandidate || pCandidate->baseForm != pBase || pCandidate->IsDisabled() || m_refItems.find(pCandidate->formID) != m_refItems.end())
                continue;

            const float dx = pCandidate->position.x - position.x;
            const float dy = pCandidate->position.y - position.y;
            const float dz = pCandidate->position.z - position.z;
            if (dx * dx + dy * dy + dz * dz <= kSameSpot * kSameSpot)
            {
                pRef = pCandidate;
                break;
            }
        }
    }

    const bool existed = pRef != nullptr;
    if (!pRef)
    {
        const uint32_t handle = ModManager::Get()->SpawnObject(pBase, position, rotation, interior ? pCell : nullptr, pWorldSpace);
        pRef = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
        if (!pRef)
        {
            spdlog::warn("{}: couldn't build item {} ({:X}) here", __FUNCTION__, acMessage.ItemId, pBase->formID);
            return true;
        }

        if (std::abs(acMessage.Scale - 1.f) > 0.001f)
            pRef->SetTransform(position, rotation, acMessage.Scale);
    }

    // A C.A.M.P.'s module: its C.A.M.P.'s workbench here from now (its rights, its objects waiting for it)
    if (campModule)
    {
        MapItem(acMessage.ItemId, pRef->formID);
        RefreshRights();
        spdlog::info("Player {} set a C.A.M.P. down: module (item {}) {} as {:X} at ({:.1f}, {:.1f}, {:.1f})", acMessage.PlayerId, acMessage.ItemId, existed ? "already here" : "built here",
                     pRef->formID, position.x, position.y, position.z);
        return true;
    }

    // Part of that settlement here too, so it can be moved and scrapped in build mode like the builder's
    if (auto* pKeyword = Cast<BGSKeyword>(TESForm::GetById(kWorkshopItemKeyword)))
        pRef->SetLinkedRef(pWorkshop, pKeyword);

    MapItem(acMessage.ItemId, pRef->formID);

    if (!existed)
    {
        // Conduits and connectors connect to what they touch, as they did for the builder while placing them
        UpdatePower(pRef, pWorkshop);
        // Counted in this settlement's budget here too, as the builder's game counted it
        QueueBudget(pRef, pWorkshop);
    }

    spdlog::info("Player {} built {:X} at workshop {:X}: item {} {} as {:X} at ({:.1f}, {:.1f}, {:.1f}) scale {:.2f}", acMessage.PlayerId, pBase->formID, pWorkshop->formID, acMessage.ItemId,
                 existed ? "already here" : "built here", pRef->formID, position.x, position.y, position.z, acMessage.Scale);
    // The local player's C.A.M.P. just moved: whether it fits at the new spot
    QueueFitCheck(acMessage, pRef);
    return true;
}

bool WorkshopService::ApplyCharacterPlaced(const NotifyWorkshopChange& acMessage, TESObjectREFR* apWorkshop) noexcept
{
    const uint32_t formId = FindCharacterFormId(acMessage.ActorServerId);
    auto* pRef = formId ? Cast<TESObjectREFR>(TESForm::GetById(formId)) : nullptr;
    // Its copy comes when this player is near it
    if (!pRef)
        return false;

    // Its C.A.M.P. moved (the server's new item for it is at the new spot): the game running it takes it there, the
    // character sync takes its copies along
    auto* pActor = Cast<Actor>(pRef);
    const NiPoint3 position = ToPoint(acMessage.Position);
    const float dx = pRef->position.x - position.x, dy = pRef->position.y - position.y, dz = pRef->position.z - position.z;
    if (pActor && !pActor->GetExtension()->IsRemote() && dx * dx + dy * dy + dz * dz > 16.f * 16.f)
    {
        auto& modSystem = m_world.GetModSystem();
        auto* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(modSystem.GetGameId(acMessage.WorldSpaceId)));
        TESObjectCELL* pCell = pWorldSpace ? ModManager::Get()->GetCellFromCoordinates(static_cast<int32_t>(std::floor(position.x / kExteriorCellSize)),
                                                                                         static_cast<int32_t>(std::floor(position.y / kExteriorCellSize)), pWorldSpace, false)
                                           : nullptr;
        pRef->rotation = ToPoint(acMessage.Rotation);
        if (pCell)
            pRef->MoveTo(pCell, position);
        else
            pRef->SetTransform(position, ToPoint(acMessage.Rotation), pRef->GetScale());
        spdlog::info("Moved turret {:X} with its C.A.M.P. to ({:.0f}, {:.0f}, {:.0f})", pRef->formID, position.x, position.y, position.z);
    }

    if (auto* pKeyword = Cast<BGSKeyword>(TESForm::GetById(kWorkshopItemKeyword)))
        pRef->SetLinkedRef(apWorkshop, pKeyword);

    MapItem(acMessage.ItemId, pRef->formID);

    spdlog::info("Player {} built turret {:X} at workshop {:X}: item {}, its character {:X} is {:X} here", acMessage.PlayerId, acMessage.BaseId.BaseId, apWorkshop->formID, acMessage.ItemId,
                 acMessage.ActorServerId, pRef->formID);
    return true;
}

TESObjectREFR* WorkshopService::FindWireEnd(const NotifyWorkshopChange& acMessage, size_t aEnd) const noexcept
{
    if (acMessage.EndItemIds[aEnd])
        return GetMappedRef(acMessage.EndItemIds[aEnd]);

    if (acMessage.EndRefIds[aEnd] == GameId{})
        return nullptr;
    return Cast<TESObjectREFR>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.EndRefIds[aEnd])));
}

bool WorkshopService::ApplyWire(const NotifyWorkshopChange& acMessage, TESForm* apBase, TESObjectREFR* apWorkshop) noexcept
{
    // Both objects here and loaded: the wire is drawn between their 3D
    std::array<TESObjectREFR*, 2> ends{FindWireEnd(acMessage, 0), FindWireEnd(acMessage, 1)};
    if (!ends[0] || !ends[1] || ends[0]->IsDisabled() || ends[1]->IsDisabled() || !ends[0]->GetNiNode() || !ends[1]->GetNiNode())
        return false;

    // Wired here already (this game has it from an earlier visit, saved with the game): that wire
    for (const auto& [linkedId, linkType] : GetPowerLinks(ends[0]))
    {
        auto* pWire = Cast<TESObjectREFR>(TESForm::GetById(linkedId));
        if (!IsWire(pWire) || pWire->IsDisabled() || m_refItems.find(pWire->formID) != m_refItems.end())
            continue;

        const auto wireEnds = GetPowerLinks(pWire);
        const bool same = std::any_of(wireEnds.begin(), wireEnds.end(), [&](const auto& acEnd) { return acEnd.first == ends[1]->formID; });
        if (!same)
            continue;

        MapItem(acMessage.ItemId, pWire->formID);
        spdlog::info("Player {} wired {:X} and {:X} at workshop {:X}: item {} already here as {:X}", acMessage.PlayerId, ends[0]->formID, ends[1]->formID, apWorkshop->formID, acMessage.ItemId,
                     pWire->formID);
        return true;
    }

    BSExtraData* pWorkshopData = GetWorkshopData(apWorkshop);
    if (!pWorkshopData)
    {
        spdlog::warn("Wire {} at workshop {:X}: that workbench has no power grid here", acMessage.ItemId, apWorkshop->formID);
        return true;
    }

    // A wire starts where its first object is (as the game and F4SE's AttachWire place it)
    const bool interior = acMessage.WorldSpaceId == GameId{};
    auto* pWorldSpace = ends[0]->GetWorldSpace();
    TESObjectCELL* pCell = ends[0]->GetParentCellEx();
    const uint32_t handle = ModManager::Get()->SpawnObject(apBase, ends[0]->position, NiPoint3(glm::vec3(0.f)), interior || !pWorldSpace ? pCell : nullptr, pWorldSpace);
    TESObjectREFR* pWire = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
    if (!pWire)
    {
        spdlog::warn("Wire {} at workshop {:X}: couldn't make the wire here", acMessage.ItemId, apWorkshop->formID);
        return true;
    }

    if (auto* pKeyword = Cast<BGSKeyword>(TESForm::GetById(kWorkshopItemKeyword)))
        pWire->SetLinkedRef(apWorkshop, pKeyword);

    {
        ScopedCurrentWorkshop _{apWorkshop};
        WorkshopContext context = MakeContext();
        ConnectWire(context, pWire, ends[0], ends[1], pWorkshopData);
    }

    if (float* pThickness = GetWireThickness(pWire); pThickness && acMessage.WireThickness > 0.f)
        *pThickness = acMessage.WireThickness;

    MapItem(acMessage.ItemId, pWire->formID);
    QueueBudget(pWire, apWorkshop);

    spdlog::info("Player {} wired {:X} and {:X} at workshop {:X}: item {} wired here as {:X} ({} power link(s))", acMessage.PlayerId, ends[0]->formID, ends[1]->formID, apWorkshop->formID,
                 acMessage.ItemId, pWire->formID, GetPowerLinks(pWire).size());
    return true;
}

void WorkshopService::UpdatePower(TESObjectREFR* apItem, TESObjectREFR* apWorkshop) noexcept
{
    BSExtraData* pWorkshopData = GetWorkshopData(apWorkshop);
    if (!apItem || !pWorkshopData || !apItem->GetNiNode())
        return;

    ScopedCurrentWorkshop _{apWorkshop};
    WorkshopContext context = MakeContext();

    // Its wires follow it
    for (const auto& [linkedId, linkType] : GetPowerLinks(apItem))
    {
        auto* pWire = Cast<TESObjectREFR>(TESForm::GetById(linkedId));
        if (IsWire(pWire) && !pWire->IsDisabled())
            RedrawWire(context, pWire);
    }

    UpdateMovingWirelessItem(apItem, pWorkshopData);
}

void WorkshopService::DisconnectWires(TESObjectREFR* apItem, TESObjectREFR* apWorkshop) noexcept
{
    if (!apItem || !apWorkshop)
        return;

    ScopedCurrentWorkshop _{apWorkshop};

    // A wire itself: off the power grid and its objects' links
    if (IsWire(apItem))
    {
        DisconnectWire(apItem);
        return;
    }

    // An object: its wires go with it (the game scraps them along)
    for (const auto& [linkedId, linkType] : GetPowerLinks(apItem))
    {
        auto* pWire = Cast<TESObjectREFR>(TESForm::GetById(linkedId));
        if (!IsWire(pWire) || pWire->IsDisabled())
            continue;

        DisconnectWire(pWire);
        if (const auto it = m_refItems.find(pWire->formID); it != m_refItems.end())
            UnmapItem(it->second);
        if (!ForgetPendingBudget(pWire->formID))
            ApplyBudget(pWire, apWorkshop, false);
        pWire->Disable();
        pWire->Delete();
        spdlog::info("Wire {:X} went with {:X}", pWire->formID, apItem->formID);
    }
}

void WorkshopService::QueueBudget(const TESObjectREFR* apItem, const TESObjectREFR* apWorkshop) noexcept
{
    m_pendingBudget.push_back({apItem->formID, apWorkshop->formID, Clock::now() + 30s});
}

bool WorkshopService::ForgetPendingBudget(uint32_t aItemFormId) noexcept
{
    const auto it = std::find_if(m_pendingBudget.begin(), m_pendingBudget.end(), [aItemFormId](const PendingBudget& acEntry) { return acEntry.ItemFormId == aItemFormId; });
    if (it == m_pendingBudget.end())
        return false;

    m_pendingBudget.erase(it);
    return true;
}

void WorkshopService::RunPendingBudget() noexcept
{
    const auto now = Clock::now();
    for (auto it = m_pendingBudget.begin(); it != m_pendingBudget.end();)
    {
        auto* pItem = Cast<TESObjectREFR>(TESForm::GetById(it->ItemFormId));
        auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(it->WorkshopFormId));
        if (!pItem || !pWorkshop || pItem->IsDisabled())
        {
            it = m_pendingBudget.erase(it);
            continue;
        }

        // Its cost is read from its 3D
        if (!pItem->GetNiNode())
        {
            if (now > it->GiveUpAt)
            {
                spdlog::warn("Budget of workshop {:X}: {:X} still has no 3D after 30 s, not counted", pWorkshop->formID, pItem->formID);
                it = m_pendingBudget.erase(it);
            }
            else
                ++it;
            continue;
        }

        ApplyBudget(pItem, pWorkshop, true);
        it = m_pendingBudget.erase(it);
    }
}

void WorkshopService::ApplyBudget(TESObjectREFR* apItem, TESObjectREFR* apWorkshop, bool aAdd) noexcept
{
    auto* pBuildWorkshop = m_inBuildMode ? Cast<TESObjectREFR>(TESForm::GetById(m_buildWorkshopId)) : nullptr;

    const float triangles = GetBudgetValue(apWorkshop, ActorValueInfo::kWorkshopCurrentTriangles);
    const float draws = GetBudgetValue(apWorkshop, ActorValueInfo::kWorkshopCurrentDraws);
    ChangeBudget(apItem, apWorkshop, aAdd, pBuildWorkshop);

    spdlog::info("Budget of workshop {:X}: {} {:X}, triangles {:.0f} -> {:.0f} (of {:.0f}), draws {:.0f} -> {:.0f} (of {:.0f})", apWorkshop->formID, aAdd ? "counted" : "took off",
                 apItem->formID, triangles, GetBudgetValue(apWorkshop, ActorValueInfo::kWorkshopCurrentTriangles), GetBudgetValue(apWorkshop, ActorValueInfo::kWorkshopMaxTriangles), draws,
                 GetBudgetValue(apWorkshop, ActorValueInfo::kWorkshopCurrentDraws), GetBudgetValue(apWorkshop, ActorValueInfo::kWorkshopMaxDraws));
}

TESObjectREFR* WorkshopService::FindChangedRef(const NotifyWorkshopChange& acMessage) const noexcept
{
    if (acMessage.ItemId)
    {
        const auto it = m_itemRefs.find(acMessage.ItemId);
        return it != m_itemRefs.end() ? GetMappedRef(acMessage.ItemId) : nullptr;
    }

    uint32_t formId = acMessage.RefId ? m_world.GetModSystem().GetGameId(acMessage.RefId) : 0;
    // A dead body that was spawned in the game: this client's copy of that character
    if (!formId && acMessage.ActorServerId)
        formId = FindCharacterFormId(acMessage.ActorServerId);
    return formId ? Cast<TESObjectREFR>(TESForm::GetById(formId)) : nullptr;
}

bool WorkshopService::IsLoadedHere(const NotifyWorkshopChange& acMessage) const noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;

    auto& modSystem = m_world.GetModSystem();

    // Interior: the player is in that cell
    if (acMessage.WorldSpaceId == GameId{})
    {
        const TESObjectCELL* pCell = pPlayer->GetParentCellEx();
        return pCell && pCell->formID == modSystem.GetGameId(acMessage.CellId);
    }

    const TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
    if (!pWorldSpace || pWorldSpace->formID != modSystem.GetGameId(acMessage.WorldSpaceId))
        return false;

    const float dx = acMessage.Position[0] - pPlayer->position.x;
    const float dy = acMessage.Position[1] - pPlayer->position.y;
    return dx * dx + dy * dy <= kLoadedRadius * kLoadedRadius;
}

void WorkshopService::RunPendingChanges() noexcept
{
    if (m_pendingChanges.empty())
        return;

    const auto now = Clock::now();
    if (now - m_lastPendingRun < 2s)
        return;
    m_lastPendingRun = now;

    for (auto it = m_pendingChanges.begin(); it != m_pendingChanges.end();)
    {
        if (ApplyChange(it->second))
            it = m_pendingChanges.erase(it);
        else
            ++it;
    }
}

void WorkshopService::RecordChange(const LocalChange& acChange) noexcept
{
    if (!acChange.Undoable)
        return;

    UndoEntry entry{};
    entry.Type = acChange.Type;
    entry.ItemFormId = acChange.ItemFormId;

    if (acChange.Type == RequestWorkshopChange::kPlaced)
        PushUndo(acChange.WorkshopFormId, std::move(entry));
    else if (acChange.Type == RequestWorkshopChange::kMoved && acChange.ItemFormId == m_outlinedId)
    {
        // An object is outlined before it's picked up, so where it was is known
        entry.Before = m_outlinedBefore;
        PushUndo(acChange.WorkshopFormId, std::move(entry));

        if (const auto* pItem = Cast<TESObjectREFR>(TESForm::GetById(acChange.ItemFormId)))
            m_outlinedBefore = GetTransform(pItem);
    }
    // Scraps are recorded by the build tools' hook on the game's scrap, which knows what scrapping gave
}

void WorkshopService::PushUndo(uint32_t aWorkshopFormId, UndoEntry aEntry) noexcept
{
    std::scoped_lock _{m_undoLock};
    auto& entries = m_undo[aWorkshopFormId];
    entries.push_back(std::move(aEntry));
    if (entries.size() > kMaxUndo)
        entries.erase(entries.begin());
}

void WorkshopService::TrackOutlined() noexcept
{
    TESObjectREFR* pOutlined = m_inBuildMode ? GetOutlinedObject() : nullptr;
    const uint32_t id = pOutlined ? pOutlined->formID : 0;
    if (id == m_outlinedId)
        return;

    m_outlinedId = id;
    if (pOutlined)
        m_outlinedBefore = GetTransform(pOutlined);
}

void WorkshopService::RecordToolChange(const TESObjectREFR* apItem, const ObjectTransform& acBefore) noexcept
{
    if (!apItem || !m_buildWorkshopId)
        return;

    if (apItem->formID == m_outlinedId)
        m_outlinedBefore = GetTransform(apItem);

    std::scoped_lock _{m_undoLock};
    auto& entries = m_undo[m_buildWorkshopId];
    // More changes to the same object in a row are one change, undone back to where it was before the first
    if (!entries.empty() && entries.back().ToolChange && entries.back().ItemFormId == apItem->formID)
        return;

    UndoEntry entry{};
    entry.Type = RequestWorkshopChange::kMoved;
    entry.ItemFormId = apItem->formID;
    entry.ToolChange = true;
    entry.Before = acBefore;
    entries.push_back(std::move(entry));
    if (entries.size() > kMaxUndo)
        entries.erase(entries.begin());
}

void WorkshopService::RecordScrap(ScrappedObject aObject) noexcept
{
    if (!m_buildWorkshopId || m_undoing)
        return;

    UndoEntry entry{};
    entry.Type = RequestWorkshopChange::kRemoved;
    entry.Scrapped = std::move(aObject);
    PushUndo(m_buildWorkshopId, std::move(entry));
}

std::string WorkshopService::UndoLastChange() noexcept
{
    auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(m_buildWorkshopId));
    if (!m_inBuildMode || !pWorkshop)
        return "Undo works in build mode";
    // Undoing builds, moves or scraps: the right to build here (rights can be taken away since)
    if (!MayBuildHere())
        return "Only the settlement's leader and the players they allow can build here";

    UndoEntry entry;
    size_t left = 0;
    {
        std::scoped_lock _{m_undoLock};
        const auto it = m_undo.find(m_buildWorkshopId);
        if (it == m_undo.end() || it->second.empty())
            return "Nothing to undo at this settlement";
        auto& entries = it.value();
        entry = std::move(entries.back());
        entries.pop_back();
        left = entries.size();
    }

    m_undoing = true;
    std::string result;
    switch (entry.Type)
    {
    case RequestWorkshopChange::kPlaced: result = UndoBuilt(entry); break;
    case RequestWorkshopChange::kMoved: result = UndoMoved(entry); break;
    case RequestWorkshopChange::kRemoved: result = UndoScrapped(entry, pWorkshop); break;
    default: break;
    }
    m_undoing = false;

    spdlog::info("Undo at workshop {:X}: {}", m_buildWorkshopId, result);
    return left ? fmt::format("{} ({} more to undo)", result, left) : result;
}

std::string WorkshopService::UndoBuilt(const UndoEntry& acEntry) noexcept
{
    TESObjectREFR* pItem = Cast<TESObjectREFR>(TESForm::GetById(acEntry.ItemFormId));
    if (!pItem || pItem->IsDisabled())
        return "What was built is gone already";

    const std::string name = NameOf(pItem->baseForm);
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return "Can't undo now";

    // Scrapped the game's way (Workshop::ScrapReference, CommonLibF4 id 636327): its materials go back to the
    // workbench and the other players see it go. Its context is where the player looks from.
    struct
    {
        float LookPosition[3];
        float LookDirection[3];
        float Heading;
        TESObjectCELL* pCell;
        TESWorldSpace* pWorldSpace;
    } context{};
    static_assert(sizeof(context) == 0x30);
    const float heading = pPlayer->rotation.z;
    context.LookPosition[0] = pPlayer->position.x;
    context.LookPosition[1] = pPlayer->position.y;
    context.LookPosition[2] = pPlayer->position.z + kEyeHeight;
    context.LookDirection[0] = std::sin(heading);
    context.LookDirection[1] = std::cos(heading);
    context.Heading = heading;
    context.pCell = pPlayer->GetParentCellEx();
    context.pWorldSpace = pPlayer->GetWorldSpace();

    using TScrapReference = void(void*, TESObjectREFR**, void*);
    POINTER_FALLOUT4(TScrapReference, s_scrapReference, 636328);
    s_scrapReference.Get()(&context, &pItem, nullptr);
    return fmt::format("Undid building {}, its materials are back", name);
}

std::string WorkshopService::UndoMoved(const UndoEntry& acEntry) noexcept
{
    auto* pItem = Cast<TESObjectREFR>(TESForm::GetById(acEntry.ItemFormId));
    if (!pItem || pItem->IsDisabled())
        return "What was moved is gone";

    pItem->SetTransform(acEntry.Before.Position, acEntry.Before.Rotation, acEntry.Before.Scale);
    if (pItem->formID == m_outlinedId)
        m_outlinedBefore = acEntry.Before;

    {
        std::scoped_lock _{m_localChangesLock};
        m_localChanges.push_back({RequestWorkshopChange::kMoved, m_buildWorkshopId, pItem->formID, false});
    }
    return fmt::format("Undid moving {}", NameOf(pItem->baseForm));
}

std::string WorkshopService::UndoScrapped(const UndoEntry& acEntry, TESObjectREFR* apWorkshop) noexcept
{
    const ScrappedObject& scrapped = acEntry.Scrapped;
    TESForm* pBase = TESForm::GetById(scrapped.BaseId);
    if (!pBase)
        return "Can't bring back what was scrapped, its object isn't known";
    const std::string name = NameOf(pBase);

    // What scrapping it gave goes back: all of it, or it isn't built again
    Vector<std::pair<TESBoundObject*, uint32_t>> components;
    for (const auto& [formId, count] : scrapped.Components)
    {
        auto* pComponent = Cast<TESBoundObject>(TESForm::GetById(formId));
        if (!pComponent)
            continue;

        uint32_t inWorkbench = 0;
        for (const auto& stack : apWorkshop->GetStacks(pComponent))
            inWorkbench += stack.Count;
        if (inWorkbench < count)
            return fmt::format("Can't bring back {}: it gave {} {}, the workbench has {} left", name, count, NameOf(pComponent), inWorkbench);
        components.emplace_back(pComponent, count);
    }

    const ObjectTransform& at = scrapped.Transform;
    // The world's own object (a plugin's reference, scrapped with scrap anything): itself back, for everyone, rather than
    // a new one built at this workbench (seen 2026-10-27: a rock cliff undone at a C.A.M.P. became one of its objects
    // and went with it when the C.A.M.P. was picked up)
    TESObjectREFR* pItem = nullptr;
    uint8_t change = RequestWorkshopChange::kPlaced;
    if (scrapped.FormId && scrapped.FormId < 0xFF000000)
        pItem = Cast<TESObjectREFR>(TESForm::GetById(scrapped.FormId));
    if (pItem)
    {
        RestoreRef(pItem, at.Position, at.Rotation, at.Scale);
        change = RequestWorkshopChange::kRestored;
    }
    else
    {
        auto* pCell = scrapped.CellId ? Cast<TESObjectCELL>(TESForm::GetById(scrapped.CellId)) : nullptr;
        auto* pWorldSpace = scrapped.WorldSpaceId ? Cast<TESWorldSpace>(TESForm::GetById(scrapped.WorldSpaceId)) : nullptr;
        const uint32_t handle = ModManager::Get()->SpawnObject(pBase, at.Position, at.Rotation, pWorldSpace ? nullptr : pCell, pWorldSpace);
        pItem = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
        if (!pItem)
            return fmt::format("Couldn't build {} again", name);

        if (std::abs(at.Scale - 1.f) > 0.001f)
            pItem->SetTransform(at.Position, at.Rotation, at.Scale);
        if (auto* pKeyword = Cast<BGSKeyword>(TESForm::GetById(kWorkshopItemKeyword)))
            pItem->SetLinkedRef(apWorkshop, pKeyword);
    }

    for (const auto& [pComponent, count] : components)
    {
        TESObjectREFR::RemoveItemData data{};
        data.pObject = pComponent;
        data.iNumber = static_cast<int32_t>(count);
        data.eReason = TESObjectREFR::IRR_NONE;
        apWorkshop->RemoveItem(&data);
    }

    {
        std::scoped_lock _{m_localChangesLock};
        m_localChanges.push_back({change, apWorkshop->formID, pItem->formID, false});
    }
    return fmt::format("Undid scrapping {}{}", name, components.empty() ? "" : ", its materials went back into it");
}

void WorkshopService::MapItem(uint32_t aItemId, uint32_t aFormId) noexcept
{
    // An object can only be one item
    if (const auto it = m_refItems.find(aFormId); it != m_refItems.end() && it->second != aItemId)
        UnmapItem(it->second);

    m_itemRefs[aItemId] = aFormId;
    m_refItems[aFormId] = aItemId;
    if (const auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(aFormId)); pRef && pRef->baseForm)
        m_itemBases[aItemId] = pRef->baseForm->formID;
    m_itemMapDirty = true;
}

void WorkshopService::UnmapItem(uint32_t aItemId) noexcept
{
    const auto it = m_itemRefs.find(aItemId);
    if (it == m_itemRefs.end())
        return;

    m_refItems.erase(it->second);
    m_itemRefs.erase(it);
    m_itemBases.erase(aItemId);
    m_itemMapDirty = true;
}

TESObjectREFR* WorkshopService::GetMappedRef(uint32_t aItemId) const noexcept
{
    const auto it = m_itemRefs.find(aItemId);
    if (it == m_itemRefs.end())
        return nullptr;

    auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(it->second));
    if (!pRef || !pRef->baseForm)
        return nullptr;

    // Remembered from an earlier session: the id may belong to another object now (the game reuses the ids of objects
    // it didn't save)
    const auto baseIt = m_itemBases.find(aItemId);
    if (baseIt != m_itemBases.end() && baseIt->second != pRef->baseForm->formID)
        return nullptr;
    return pRef;
}

bool WorkshopService::IsRememberedElsewhere(const NotifyWorkshopChange& acMessage) const noexcept
{
    return acMessage.ItemId && m_itemRefs.find(acMessage.ItemId) != m_itemRefs.end() && !GetMappedRef(acMessage.ItemId) && !IsLoadedHere(acMessage);
}

void WorkshopService::LoadItemMap() noexcept
{
    if (m_itemMapLoaded || !m_transport.IsConnected())
        return;
    m_itemMapLoaded = true;

    // The world's save folder (co-op is played in a world's own)
    m_itemMapFile = SaveFolder::GetGameUserFolder() / SaveFolder::GetLocalSavePath() / "settlement-items.txt";

    std::ifstream file(m_itemMapFile);
    if (!file)
        return;

    size_t count = 0;
    std::string text;
    while (std::getline(file, text))
    {
        if (text.empty() || text[0] == '#')
            continue;

        std::istringstream line(text);
        uint32_t itemId = 0, formId = 0, baseId = 0;
        line >> itemId >> std::hex >> formId >> baseId;
        if (line.fail() || !itemId || !formId || m_itemRefs.find(itemId) != m_itemRefs.end())
            continue;

        m_itemRefs[itemId] = formId;
        m_refItems[formId] = itemId;
        m_itemBases[itemId] = baseId;
        ++count;
    }

    spdlog::info("Remembered {} settlement objects of this world ({})", count, m_itemMapFile.string());
}

void WorkshopService::SaveItemMap() noexcept
{
    if (!m_itemMapLoaded || m_itemMapFile.empty())
        return;

    m_itemMapDirty = false;
    m_itemMapSavedAt = Clock::now();

    auto temporary = m_itemMapFile;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file)
        {
            spdlog::warn("Couldn't write {}", temporary.string());
            return;
        }

        file << "# Reclamation Day: which object in this world's saves is which settlement item of its server\n";
        file << "# item form_id base_id\n";
        for (const auto& [itemId, formId] : m_itemRefs)
        {
            const auto baseIt = m_itemBases.find(itemId);
            file << fmt::format("{} {:08X} {:08X}\n", itemId, formId, baseIt != m_itemBases.end() ? baseIt->second : 0);
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, m_itemMapFile, ec);
    if (ec)
        spdlog::warn("Couldn't replace {}: {}", m_itemMapFile.string(), ec.message());
}

void WorkshopService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // What this game has of the world is in its save, which item each object is stays known for the next session
    SaveItemMap();
    m_itemMapLoaded = false;
    m_itemRefs.clear();
    m_refItems.clear();
    m_itemBases.clear();
    m_pendingChanges.clear();
    m_deferredChanges.clear();
    m_camps.clear();
    m_enterCampUntil.reset();
    m_campCardWanted = false;
    CloseCampCard();
    m_pendingCampMove.reset();
    m_localCampStored = false;
    ResetClaims();
    ResetSettlementDay();
    std::scoped_lock _{m_localChangesLock};
    m_scrappedBodies.clear();
}

#endif

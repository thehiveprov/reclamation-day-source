#if TP_FALLOUT4

// C.A.M.P.s, the client's side (see WorkshopService.h, Camp and the server's WorkshopService)

#include <Services/WorkshopService.h>

#include <World.h>
#include <PlayerCharacter.h>
#include <TESObjectREFR.h>
#include <Games/TES.h>
#include <Forms/TESWorldSpace.h>
#include <Interface/UI.h>
#include <Messages/RequestCamp.h>
#include <Services/OverlayService.h>
#include <Services/PapyrusService.h>
#include <Services/TransportService.h>
#include <WorldRays.h>
#include <Forms/TESObjectCELL.h>

#include <MiniJson.h>

namespace
{
using Clock = std::chrono::steady_clock;
using ObjectReference = TESObjectREFR;

// RD_Camps.esp (Tools/BuildCampsPlugin.js): local ids
constexpr char kCampsPlugin[] = "RD_Camps.esp";
constexpr uint32_t kModuleLocalId = 0x800; // RD_CampModule, the workbench
constexpr uint32_t kDeviceLocalId = 0x801; // RD_CampDevice, the Aid item
// Where the module goes: this far in front of the player
constexpr float kSpotDistance = 300.f;
// Not this close to the map marker of a place with a settlement: one whose workbench has the marker in its build area
// or stands this close to it
constexpr float kMarkerDistance = 3000.f;
constexpr float kSettlementMarkerDistance = 2000.f;
// Moving is free (since 2026-10-27; it cost 5 caps plus one per 1000 units, at most 50)
// PlayerCharacter (CommonLibF4, 1.10.163): currentMapMarkers, a BSTArray<ObjectRefHandle> (size at +0x10), as
// PipboyMapService reads it
constexpr uintptr_t kMapMarkersOffset = 0x738;

void ShowHudMessage(const std::string& acText) noexcept
{
    // SendHUDMessage::ShowHUDMessage (CommonLibF4 id 1163005): message, sound, throttle, warning
    using TShowHUDMessage = void(const char*, const char*, bool, bool);
    POINTER_FALLOUT4(TShowHUDMessage, s_showHUDMessage, 1163006);
    s_showHUDMessage.Get()(acText.c_str(), "", false, false);
}

// --- What doesn't fit after a move (into the workbench as materials, rather than kept as objects)

// The ground's collision layer (L_TERRAIN)
constexpr uint32_t kLayerTerrain = 13;
// Form types: constructible object (COBJ), the game's form arrays (TESDataHandler, by form type from 0x68)
constexpr uint8_t kRecipeFormType = 0x8F;
struct FormArray
{
    TESForm** pData;
    uint32_t Capacity;
    uint32_t Pad;
    uint32_t Size;
    uint32_t Pad2;
};
static_assert(sizeof(FormArray) == 0x18);
// BGSConstructibleObject (CommonLibF4): requiredItems, a BSTArray<BSTTuple<TESForm*, uint32 count>>*, at 0x50 and
// createdItem at 0x60; BGSComponent: its scrap item (the c_..._scrap misc item workbenches keep) at 0x98
constexpr uintptr_t kRecipeItemsOffset = 0x50;
constexpr uintptr_t kRecipeCreatedOffset = 0x60;
constexpr uintptr_t kComponentScrapItemOffset = 0x98;
struct RecipeItem
{
    TESForm* pItem;
    uint32_t Count;
    uint32_t Pad;
};

// What each workshop object is built from, by base form id: the scrap items (for a component) or items and how many,
// read once from every recipe (the first one making it)
const TiltedPhoques::Map<uint32_t, Vector<std::pair<uint32_t, uint32_t>>>& GetBuildMaterials() noexcept
{
    static const auto s_materials = []
    {
        TiltedPhoques::Map<uint32_t, Vector<std::pair<uint32_t, uint32_t>>> materials;
        const auto* pManager = reinterpret_cast<const uint8_t*>(ModManager::Get());
        if (!pManager)
            return materials;
        const auto& recipes = *reinterpret_cast<const FormArray*>(pManager + 0x68 + kRecipeFormType * sizeof(FormArray));
        for (uint32_t i = 0; recipes.pData && i < recipes.Size; ++i)
        {
            const auto* pRecipe = reinterpret_cast<const uint8_t*>(recipes.pData[i]);
            if (!pRecipe || static_cast<uint8_t>(recipes.pData[i]->formType) != kRecipeFormType)
                continue;
            const auto* pCreated = *reinterpret_cast<TESForm* const*>(pRecipe + kRecipeCreatedOffset);
            const auto* pItems = *reinterpret_cast<const FormArray* const*>(pRecipe + kRecipeItemsOffset);
            if (!pCreated || !pItems || materials.find(pCreated->formID) != materials.end())
                continue;

            Vector<std::pair<uint32_t, uint32_t>> given;
            const auto* pEntries = reinterpret_cast<const RecipeItem*>(pItems->pData);
            for (uint32_t j = 0; pEntries && j < pItems->Size; ++j)
            {
                const TESForm* pItem = pEntries[j].pItem;
                if (!pItem || !pEntries[j].Count)
                    continue;
                if (pItem->formType == FormType::Component)
                    pItem = *reinterpret_cast<TESForm* const*>(reinterpret_cast<const uint8_t*>(pItem) + kComponentScrapItemOffset);
                if (pItem && pItem->formType == FormType::Misc)
                    given.emplace_back(pItem->formID, pEntries[j].Count);
            }
            if (!given.empty())
                materials[pCreated->formID] = std::move(given);
        }
        spdlog::info("C.A.M.P.s: {} recipes read, {} objects with materials", recipes.Size, materials.size());
        return materials;
    }();
    return s_materials;
}

// The top of a placed object (its bounds on its spot; tilted, the farthest corner from its origin)
float TopOf(TESObjectREFR* apRef) noexcept
{
    const NiPoint3 low = apRef->GetBoundMin();
    const NiPoint3 high = apRef->GetBoundMax();
    const float scale = apRef->GetScale();
    if (std::abs(apRef->rotation.x) < 0.01f && std::abs(apRef->rotation.y) < 0.01f)
        return apRef->position.z + high.z * scale;
    const float x = std::max(std::abs(low.x), std::abs(high.x)), y = std::max(std::abs(low.y), std::abs(high.y)), z = std::max(std::abs(low.z), std::abs(high.z));
    return apRef->position.z + std::sqrt(x * x + y * y + z * z) * scale;
}

// The ground under a spot: the first terrain a ray down from above meets, through whatever is built on it; none when
// the ground there isn't loaded
std::optional<float> GroundUnder(TESObjectCELL* apCell, uint32_t aFilter, float aX, float aY, float aFromZ) noexcept
{
    NiPoint3 from;
    from.x = aX;
    from.y = aY;
    from.z = aFromZ;
    NiPoint3 to = from;
    to.z = aFromZ - 30000.f;
    for (int i = 0; i < 24; ++i)
    {
        const WorldRays::RayHit hit = WorldRays::PickOnce(apCell, aFilter, from, to);
        if (!hit.Hit)
            return std::nullopt;
        if (hit.Layer == kLayerTerrain)
            return hit.Point.z;
        from.z = hit.Point.z - 1.f;
        if (from.z <= to.z)
            return std::nullopt;
    }
    return std::nullopt;
}

uint32_t FindPluginForm(uint32_t aLocalId) noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(kCampsPlugin);
    return pMod && pMod->IsLoaded() ? pMod->GetFormId(aLocalId) : 0;
}

bool HasPapyrus(const char* apName) noexcept
{
    if (World::Get().ctx().at<PapyrusService>().Get("ObjectReference", apName))
        return true;
    spdlog::warn("C.A.M.P.s: ObjectReference.{} not found", apName);
    return false;
}

int32_t GetItemCount(TESObjectREFR* apRef, TESForm* apItem) noexcept
{
    if (!HasPapyrus("GetItemCount"))
        return 0;
    PAPYRUS_FUNCTION(int32_t, ObjectReference, GetItemCount, TESForm*);
    return s_pGetItemCount(apRef, apItem);
}

// GameScript::FormOrInventoryObj (0x10: the form, then a unique id for an inventory item, 0 for a form): the item of
// AddItem and RemoveItem as the game binds them (NativeFunctionN<TESObjectREFR, bool, FormOrInventoryObj, ...>, checked in
// the exe). Bigger than 8 bytes, so the native gets a pointer to it, never the form itself.
struct FormOrInventoryObj
{
    TESForm* pForm;
    uint16_t UniqueId;
};
static_assert(sizeof(FormOrInventoryObj) == 0x10);

void AddItem(TESObjectREFR* apRef, TESForm* apItem, int32_t aCount, bool aSilent) noexcept
{
    if (!HasPapyrus("AddItem") || !apItem)
        return;
    PAPYRUS_FUNCTION(bool, ObjectReference, AddItem, const FormOrInventoryObj*, uint32_t, bool);
    const FormOrInventoryObj item{apItem, 0};
    s_pAddItem(apRef, &item, static_cast<uint32_t>(aCount), aSilent);
}

void StartWorkshop(TESObjectREFR* apWorkshop) noexcept
{
    if (!HasPapyrus("StartWorkshop"))
        return;
    PAPYRUS_FUNCTION(void, ObjectReference, StartWorkshop, bool);
    s_pStartWorkshop(apWorkshop, true);
}

// Workshop::FindNearestValidWorkshop (CommonLibF4 id 905705) and IsLocationWithinBuildableArea (990965, a C.A.M.P.'s
// sphere through the build tools' hook)
TESObjectREFR* FindNearestWorkshop(const TESObjectREFR* apRef) noexcept
{
    using TFindNearest = TESObjectREFR*(const TESObjectREFR*);
    POINTER_FALLOUT4(TFindNearest, s_findNearest, 905706);
    return s_findNearest.Get()(apRef);
}

bool IsInBuildableArea(const TESObjectREFR* apWorkshop, const NiPoint3& acPosition) noexcept
{
    using TBuildableArea = bool(const TESObjectREFR*, const NiPoint3*);
    POINTER_FALLOUT4(TBuildableArea, s_isInBuildableArea, 990966);
    return s_isInBuildableArea.Get()(apWorkshop, &acPosition);
}

float Distance2D(const std::array<float, 3>& acFrom, const NiPoint3& acTo) noexcept
{
    const float dx = acTo.x - acFrom[0];
    const float dy = acTo.y - acFrom[1];
    return std::sqrt(dx * dx + dy * dy);
}

void ShowOverlayCursor(bool aShow) noexcept
{
    auto& overlay = World::Get().ctx().at<OverlayService>();
    if (overlay.GetActive() == aShow)
        return;
    overlay.SetActive(aShow);
    if (aShow)
        while (ShowCursor(TRUE) <= 0)
            ;
    else
        while (ShowCursor(FALSE) >= 0)
            ;
}

bool IsMenuOpen(const char* apName) noexcept
{
    UI* pUi = UI::Get();
    return pUi && pUi->GetMenuOpen(BSFixedString(apName));
}

void ClosePipboy() noexcept
{
    // UIMessageQueue::AddMessage (CommonLibF4 ids 82123, 1182019): 3 hides a menu
    POINTER_FALLOUT4(void*, s_queue, 82124);
    using TAddMessage = void(void*, const BSFixedString&, int32_t);
    POINTER_FALLOUT4(TAddMessage, s_addMessage, 1182020);
    if (void* pQueue = *reinterpret_cast<void**>(s_queue.Get()))
        s_addMessage.Get()(pQueue, BSFixedString("PipboyMenu"), 3);
}
} // namespace

namespace
{
// Kept once found, looked for at most once a second until then: the hooks ask for every object, every frame
uint32_t CachedPluginForm(std::atomic<uint32_t>& aId, std::atomic<int64_t>& aTriedAt, uint32_t aLocalId) noexcept
{
    if (const uint32_t id = aId)
        return id;

    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (now - aTriedAt < 1000)
        return 0;
    aTriedAt = now;
    aId = FindPluginForm(aLocalId);
    return aId;
}
} // namespace

uint32_t WorkshopService::GetCampModuleId() noexcept
{
    static std::atomic<uint32_t> s_id{0};
    static std::atomic<int64_t> s_triedAt{0};
    return CachedPluginForm(s_id, s_triedAt, kModuleLocalId);
}

uint32_t WorkshopService::GetCampDeviceId() noexcept
{
    static std::atomic<uint32_t> s_id{0};
    static std::atomic<int64_t> s_triedAt{0};
    return CachedPluginForm(s_id, s_triedAt, kDeviceLocalId);
}

bool WorkshopService::GetSharedWorkshopId(uint32_t aWorkshopFormId, GameId& aId) const noexcept
{
    if (!aWorkshopFormId)
        return false;

    // A C.A.M.P.'s module, as built here
    if (const auto item = m_refItems.find(aWorkshopFormId); item != m_refItems.end())
    {
        for (const Camp& camp : m_camps)
        {
            if (camp.ModuleItemId == item->second)
            {
                aId = camp.GetWorkshopId();
                return true;
            }
        }
    }

    // Placed while playing: no id the others know
    if ((aWorkshopFormId & 0xFF000000) == 0xFF000000)
        return false;
    return m_world.GetModSystem().GetServerModId(aWorkshopFormId, aId);
}

TESObjectREFR* WorkshopService::GetWorkshopRef(const GameId& acId) const noexcept
{
    if (Camp::IsCamp(acId))
    {
        const auto camp = std::find_if(m_camps.begin(), m_camps.end(), [&acId](const Camp& acCamp) { return acCamp.Id == acId.BaseId; });
        return camp != m_camps.end() ? GetMappedRef(camp->ModuleItemId) : nullptr;
    }
    return Cast<TESObjectREFR>(TESForm::GetById(m_world.GetModSystem().GetGameId(acId)));
}

const Camp* WorkshopService::FindLocalCamp() const noexcept
{
    if (m_localName.empty())
        return nullptr;
    const auto camp = std::find_if(m_camps.begin(), m_camps.end(), [this](const Camp& acCamp) { return acCamp.OwnerName == m_localName; });
    return camp != m_camps.end() ? &*camp : nullptr;
}

void WorkshopService::OnNotifyCamps(const NotifyCamps& acMessage) noexcept
{
    m_camps = acMessage.Camps;
    RefreshRights();
    if (m_inBuildMode)
        CheckBuildModeClaim(false);

    // Picked up (from the card or the Settlements tab): said once; the card has nothing left to ask about
    const Camp* pCamp = FindLocalCamp();
    const bool stored = pCamp && pCamp->Stored;
    const bool wasStored = m_localCampStored;
    if (stored && !m_localCampStored)
        ShowHudMessage("Your C.A.M.P. is packed up. Use the Makeshift CAMP device to set it down again.");
    m_localCampStored = stored;
    if (!pCamp || stored)
    {
        m_campCardWanted = false;
        CloseCampCard();
    }

    // Moved, or set down from being stored: what's built again at the new spot is checked as it's built here
    // (QueueFitCheck), for a while
    std::optional<std::array<float, 3>> spot;
    if (pCamp && !stored)
        spot = pCamp->Position;
    if (spot && (m_localCampSpot ? *m_localCampSpot != *spot : wasStored))
    {
        m_fitCampId = pCamp->Id;
        m_fitUntil = Clock::now() + std::chrono::seconds(90);
        m_fitStored = 0;
        spdlog::info("C.A.M.P.s: yours is at ({:.0f}, {:.0f}, {:.0f}) now, checking what's built again there", (*spot)[0], (*spot)[1], (*spot)[2]);
    }
    m_localCampSpot = spot;
}

void WorkshopService::RunCamps() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const auto now = Clock::now();

    // Everyone carries the device (Fallout 76: the C.A.M.P. is always in the Pip-Boy)
    if (now - m_campDeviceCheckedAt > std::chrono::seconds(5))
    {
        m_campDeviceCheckedAt = now;
        if (TESForm* pDevice = TESForm::GetById(GetCampDeviceId()); pDevice && GetItemCount(pPlayer, pDevice) <= 0)
        {
            AddItem(pPlayer, pDevice, 1, true);
            spdlog::info("C.A.M.P.s: gave the player the C.A.M.P. device ({:X})", pDevice->formID);
        }
    }

    // The card, once the Pip-Boy the device was used from is down; closed by the overlay (Escape) is "not now"
    if (m_campCardWanted && !IsMenuOpen("PipboyMenu") && !IsMenuOpen("LoadingMenu"))
        ShowCampCard();
    if (m_campCardOpen && !World::Get().ctx().at<OverlayService>().GetActive())
    {
        CloseCampCard();
        AnswerCampMove(false);
    }

    // Set down just now: build mode at it once its module stands here
    if (m_enterCampUntil)
    {
        if (now > *m_enterCampUntil)
        {
            spdlog::warn("C.A.M.P.s: the module of the C.A.M.P. set down didn't come, not opening build mode");
            m_enterCampUntil.reset();
        }
        else if (const Camp* pCamp = FindLocalCamp())
        {
            TESObjectREFR* pModule = GetMappedRef(pCamp->ModuleItemId);
            if (pModule && pModule->GetNiNode() && !m_inBuildMode)
            {
                m_enterCampUntil.reset();
                spdlog::info("C.A.M.P.s: opening build mode at the new C.A.M.P. ({:X})", pModule->formID);
                StartWorkshop(pModule);
            }
        }
    }

    RunFitChecks();
}

std::string WorkshopService::WhyNoCampHere(const NiPoint3& acSpot) const noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->GetWorldSpace())
        return "C.A.M.P.s go outside.";

    // Not in a settlement, nor in someone else's C.A.M.P. (the server keeps C.A.M.P.s apart; ours moves)
    if (TESObjectREFR* pWorkshop = FindNearestWorkshop(pPlayer); pWorkshop && IsInBuildableArea(pWorkshop, acSpot))
    {
        const Camp* pMine = FindLocalCamp();
        const bool mine = pMine && GetMappedRef(pMine->ModuleItemId) == pWorkshop;
        if (!mine)
            return pWorkshop->baseForm && pWorkshop->baseForm->formID == GetCampModuleId() ? "Too close to another C.A.M.P." : "C.A.M.P.s go outside settlements.";
    }

    // Not near someone else's C.A.M.P. (the server refuses it too: said before anything is paid)
    GameId worldSpaceId{};
    if (m_world.GetModSystem().GetServerModId(pPlayer->GetWorldSpace()->formID, worldSpaceId))
    {
        for (const Camp& camp : m_camps)
        {
            if (camp.OwnerName != m_localName && !camp.Stored && camp.WorldSpaceId == worldSpaceId && Distance2D(camp.Position, acSpot) < 2.f * Camp::kRadius)
                return "Too close to another C.A.M.P.";
        }
    }

    // Not at a marked location that has a settlement (named locations without one are fine, though Fallout 76 keeps
    // them off every marked location): a map marker
    // near the spot whose nearest workbench, a settlement's, has the marker in its build area or stands by it
    const uint8_t* pMarkers = reinterpret_cast<const uint8_t*>(pPlayer) + kMapMarkersOffset;
    const auto* pHandles = *reinterpret_cast<const uint32_t* const*>(pMarkers);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pMarkers + 0x10);
    for (uint32_t i = 0; pHandles && i < count && i < 4096; ++i)
    {
        TESObjectREFR* pMarker = TESObjectREFR::GetByHandle(pHandles[i]);
        if (!pMarker || pMarker->GetWorldSpace() != pPlayer->GetWorldSpace())
            continue;
        const float dx = pMarker->position.x - acSpot.x;
        const float dy = pMarker->position.y - acSpot.y;
        if (dx * dx + dy * dy >= kMarkerDistance * kMarkerDistance)
            continue;

        TESObjectREFR* pWorkshop = FindNearestWorkshop(pMarker);
        if (!pWorkshop || (pWorkshop->baseForm && pWorkshop->baseForm->formID == GetCampModuleId()))
            continue;
        const float wx = pWorkshop->position.x - pMarker->position.x;
        const float wy = pWorkshop->position.y - pMarker->position.y;
        if (IsInBuildableArea(pWorkshop, pMarker->position) || wx * wx + wy * wy < kSettlementMarkerDistance * kSettlementMarkerDistance)
            return "That place has a settlement.";
    }

    return {};
}

void WorkshopService::OnCampDeviceUsed() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    if (!m_transport.IsConnected())
    {
        ShowHudMessage("Join a server to set up your C.A.M.P.");
        return;
    }

    // In front of the player, facing them
    const float heading = pPlayer->rotation.z;
    NiPoint3 spot = pPlayer->position;
    spot.x += std::sin(heading) * kSpotDistance;
    spot.y += std::cos(heading) * kSpotDistance;

    RequestCamp request;
    request.Position = {spot.x, spot.y, spot.z};
    request.Heading = heading;
    std::string why = WhyNoCampHere(spot);
    if (why.empty() && !m_world.GetModSystem().GetServerModId(pPlayer->GetWorldSpace()->formID, request.WorldSpaceId))
        why = "C.A.M.P.s can't go in this worldspace.";

    // The module's id this session, for a move too: the server numbers plugins anew each time it starts, so the one a
    // C.A.M.P. was saved with may be another plugin's by now (2026-10-27: a C.A.M.P. from 10-22, moved after plugins
    // were added, came back as nothing)
    if (!m_world.GetModSystem().GetServerModId(GetCampModuleId(), request.ModuleBaseId))
    {
        spdlog::warn("C.A.M.P.s: RD_Camps.esp's module isn't known to the server");
        return;
    }

    // None yet, or packed up: set down here (a stored one is built again from its layout, free)
    const Camp* pCamp = FindLocalCamp();
    if (!pCamp || pCamp->Stored)
    {
        if (!why.empty())
        {
            spdlog::info("C.A.M.P.s: can't set one down at ({:.0f}, {:.0f}): {}", spot.x, spot.y, why);
            ShowHudMessage(why);
            return;
        }

        request.Action = RequestCamp::kPlace;
        m_transport.Send(request);
        m_enterCampUntil = Clock::now() + std::chrono::seconds(30);
        spdlog::info("C.A.M.P.s: setting ours down at ({:.0f}, {:.0f}, {:.0f}){}", spot.x, spot.y, spot.z, pCamp ? ", from its layout" : "");
        return;
    }

    // One standing: the card asks (move it here, pick it up, not now), once the Pip-Boy is down
    request.Action = RequestCamp::kMove;
    m_pendingCampMove = request;
    m_campMoveRefusal = why;
    m_campCardWanted = true;
    if (IsMenuOpen("PipboyMenu"))
        ClosePipboy();
}

void WorkshopService::ShowCampCard() noexcept
{
    m_campCardWanted = false;
    if (!m_pendingCampMove || !PlayerCharacter::Get() || World::Get().ctx().at<OverlayService>().GetActive())
        return;

    // Moving is free: the card's cost and caps stay 0
    World::Get().ctx().at<OverlayService>().SendEvent("campCard", fmt::format(R"({{"cost":0,"caps":0,"refusal":{}}})", MiniJson::Quote(m_campMoveRefusal)));
    ShowOverlayCursor(true);
    m_campCardOpen = true;
    spdlog::info("C.A.M.P.s: the card asks (move here{})", m_campMoveRefusal.empty() ? "" : ", can't move here: " + m_campMoveRefusal);
}

void WorkshopService::CloseCampCard() noexcept
{
    if (!m_campCardOpen)
        return;
    m_campCardOpen = false;
    World::Get().ctx().at<OverlayService>().SendEvent("campCard", "null");
    ShowOverlayCursor(false);
}

void WorkshopService::PickUpCamp() noexcept
{
    const Camp* pCamp = FindLocalCamp();
    if (!pCamp || pCamp->Stored || !m_transport.IsConnected())
        return;

    RequestCamp request;
    request.Action = RequestCamp::kPickUp;
    request.CampId = pCamp->Id;
    m_transport.Send(request);
    spdlog::info("C.A.M.P.s: picking ours ({}) up", pCamp->Id);
}

void WorkshopService::AnswerCampMove(bool aMove) noexcept
{
    const auto pending = std::exchange(m_pendingCampMove, std::nullopt);
    if (!aMove || !pending || !PlayerCharacter::Get() || !m_transport.IsConnected())
        return;
    if (!m_campMoveRefusal.empty())
    {
        ShowHudMessage(m_campMoveRefusal);
        return;
    }

    m_transport.Send(*pending);
    spdlog::info("C.A.M.P.s: moving ours to ({:.0f}, {:.0f}, {:.0f})", pending->Position[0], pending->Position[1], pending->Position[2]);
}

void WorkshopService::QueueFitCheck(const NotifyWorkshopChange& acMessage, const TESObjectREFR* apRef) noexcept
{
    if (!m_fitCampId || !apRef || Clock::now() > m_fitUntil)
        return;
    const Camp* pCamp = FindLocalCamp();
    if (!pCamp || pCamp->Id != m_fitCampId || acMessage.WorkshopId != pCamp->GetWorkshopId() || acMessage.ItemId == pCamp->ModuleItemId)
        return;
    // (a moment later: its spot's ground loaded with it)
    m_fitChecks.emplace_back(acMessage.ItemId, Clock::now() + std::chrono::seconds(2));
}

void WorkshopService::RunFitChecks() noexcept
{
    const auto now = Clock::now();
    if (m_fitChecks.empty())
    {
        if (m_fitCampId && now > m_fitUntil)
            m_fitCampId = 0;
        return;
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const Camp* pCamp = FindLocalCamp();
    TESObjectREFR* pWorkshop = pCamp ? GetWorkshopRef(pCamp->GetWorkshopId()) : nullptr;
    if (!pPlayer || !pCamp || pCamp->Id != m_fitCampId)
    {
        m_fitChecks.clear();
        return;
    }

    const uint32_t filter = WorldRays::GetLineOfSightFilter(pPlayer);
    for (auto it = m_fitChecks.begin(); it != m_fitChecks.end();)
    {
        if (now < it->second)
        {
            ++it;
            continue;
        }
        TESObjectREFR* pRef = GetMappedRef(it->first);
        TESObjectCELL* pCell = pRef ? pRef->GetParentCellEx() : nullptr;
        if (!pRef || !pCell || !pRef->baseForm)
        {
            it = m_fitChecks.erase(it);
            continue;
        }

        const float top = TopOf(pRef);
        const std::optional<float> ground = GroundUnder(pCell, filter, pRef->position.x, pRef->position.y, top + 3000.f);
        if (!ground || !pWorkshop)
        {
            // its ground (or the module) not here yet: again in a moment, until the checks are over
            if (now > m_fitUntil)
                it = m_fitChecks.erase(it);
            else
            {
                it->second = now + std::chrono::seconds(1);
                ++it;
            }
            continue;
        }

        // Entirely underground at the new spot: into the workbench as what it's built from
        if (top < *ground - 2.f && StoreAsMaterials(pRef, pWorkshop))
            ++m_fitStored;
        it = m_fitChecks.erase(it);
    }

    if (m_fitChecks.empty() && m_fitStored)
    {
        ShowHudMessage(fmt::format("{} object{} that ended up underground went into your C.A.M.P.'s workbench as materials", m_fitStored, m_fitStored == 1 ? "" : "s"));
        m_fitStored = 0;
    }
}

bool WorkshopService::StoreAsMaterials(TESObjectREFR* apRef, TESObjectREFR* apWorkshop) noexcept
{
    // What it's built from; an item set out (a decoration) goes back as itself; anything else stays where it is
    Vector<std::pair<uint32_t, uint32_t>> given;
    const auto& materials = GetBuildMaterials();
    if (const auto it = materials.find(apRef->baseForm->formID); it != materials.end())
        given = it->second;
    else
    {
        switch (apRef->baseForm->formType)
        {
        case FormType::Misc:
        case FormType::Weapon:
        case FormType::Armor:
        case FormType::Alchemy:
        case FormType::Book:
        case FormType::Ammo:
        case FormType::Key:
        case FormType::Note: given.emplace_back(apRef->baseForm->formID, 1); break;
        default: break;
        }
    }
    if (given.empty())
    {
        spdlog::info("C.A.M.P.s: {:X} ({:X}) is underground at the new spot but isn't built from anything, left there", apRef->formID, apRef->baseForm->formID);
        return false;
    }

    std::string list;
    for (const auto& [itemId, count] : given)
    {
        auto* pItem = Cast<TESBoundObject>(TESForm::GetById(itemId));
        if (!pItem)
            continue;
        TESObjectREFR::ExtraDataListHandle handle{nullptr};
        apWorkshop->AddObjectToContainer(pItem, &handle, static_cast<int32_t>(count), nullptr, 0);
        list += fmt::format("{}{} {:X}", list.empty() ? "" : ", ", count, itemId);
    }

    // Gone for everyone, as a scrap is (shared from the next update), then here
    QueueLocalChange(RequestWorkshopChange::kRemoved, apWorkshop, apRef);
    DisconnectWires(apRef, apWorkshop);
    if (!ForgetPendingBudget(apRef->formID))
        ApplyBudget(apRef, apWorkshop, false);
    apRef->Disable();
    apRef->Delete();
    spdlog::info("C.A.M.P.s: {:X} ({:X}) ended up underground at the new spot, into the workbench as {}", apRef->formID, apRef->baseForm->formID, list);
    return true;
}

#endif

#if TP_FALLOUT4

#include <Services/BuildToolsService.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Services/OverlayService.h>
#include <Services/WorkshopService.h>
#include <Services/ScenerySalvage.h>
#include <MiniJson.h>

#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Games/TES.h>
#include <Forms/TESBoundObject.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

#include <fstream>
#include <PerfProbe.h>

namespace
{
BuildToolsService* s_pBuildTools = nullptr;

constexpr float kPi = 3.14159265f;
constexpr float kDegrees = kPi / 180.f;
constexpr std::array<float, 7> kRotationSteps{0.1f, 0.5f, 1.f, 5.f, 15.f, 45.f, 90.f}; // degrees
constexpr std::array<float, 7> kMoveSteps{0.1f, 0.5f, 1.f, 5.f, 10.f, 50.f, 100.f};   // units
constexpr std::array<int, 5> kScaleSteps{1, 5, 10, 25, 50};                           // percent
constexpr float kMinScale = 0.01f;
constexpr float kMaxScale = 10.f;
constexpr auto kRepeatDelay = 350ms;
constexpr auto kRepeatInterval = 50ms;

// TESForm flags of references, and the form type of doors (DOOR)
constexpr uint32_t kPersistentFlag = 0x400;
constexpr uint8_t kDoorType = 0x20;

// The game's placement checks (1.10.163, found in the game's code; client ids are the game's + 1):
// - 238047 checks the object being placed (on placing, and each frame): it sets the placement status (id 140777,
//   1 = fine) and returns status == 1. Status 3 = the frame's "blocked" flag (id 822283) is set (overlapping, not
//   supported, ...), 6 = too far away, 2 and 8 = other conditions.
// - 978467 checks an object being moved the same way.
// - 1565251 outlines the object in the color of a state: 1 fine, 3 can't place.
// - 1386903 Workshop::IsWorkshopItem: whether a reference belongs to a settlement (linked to its workbench with
//   WorkshopItemKeyword). Build mode moves and scraps only those.
// - 1371490 decides what build mode outlines: each frame (1411424) a ray from the crosshair goes through what it hits
//   (not markers, trigger boxes or what has the keyword 856489) and asks it of each: settlement objects, unless
//   furniture someone uses; else (dead, for people and creatures) junk with a scrap recipe and the like in the build
//   area. Its first argument isn't used.
using TCheckPlacement = bool(TESObjectREFR*);
TCheckPlacement* RealCheckNewPlacement = nullptr;
TCheckPlacement* RealCheckMovedPlacement = nullptr;
using TOutlinePlacement = void(uint32_t);
TOutlinePlacement* RealOutlinePlacement = nullptr;
using TIsWorkshopItem = bool(const TESObjectREFR*);
TIsWorkshopItem* RealIsWorkshopItem = nullptr;
using TCanSelect = bool(void*, TESObjectREFR*);
TCanSelect* RealCanSelect = nullptr;
// Workshop::ScrapReference (CommonLibF4 id 636327): context, the reference to scrap (a smart pointer: in memory the
// reference pointer), the components it gives
using TScrapReference = void(void*, TESObjectREFR**, void*);
TScrapReference* RealScrapReference = nullptr;

// Workshop::IsLocationWithinBuildableArea (CommonLibF4 id 990965), read from the 1.10.163 exe: inside the workbench's
// build area volume (its linked primitive), else within the radius of its location's center marker, else within 5000
// units of the workbench (.data 0x36DBF60). A C.A.M.P.'s module (RD_Camps.esp) has none of those: its sphere is
// Camp::kRadius.
using TBuildableArea = bool(const TESObjectREFR*, const NiPoint3*);
TBuildableArea* RealBuildableArea = nullptr;

bool HookBuildableArea(const TESObjectREFR* apWorkshop, const NiPoint3* apLocation)
{
    const uint32_t moduleId = WorkshopService::GetCampModuleId();
    if (apWorkshop && apLocation && moduleId && apWorkshop->baseForm && apWorkshop->baseForm->formID == moduleId)
    {
        const float dx = apLocation->x - apWorkshop->position.x;
        const float dy = apLocation->y - apWorkshop->position.y;
        const float dz = apLocation->z - apWorkshop->position.z;
        return dx * dx + dy * dy + dz * dz <= Camp::kRadius * Camp::kRadius;
    }
    return RealBuildableArea(apWorkshop, apLocation);
}

bool AllowPlacement(TESObjectREFR* apRef, bool aAllowed) noexcept
{
    POINTER_FALLOUT4(uint8_t, s_placementStatus, 140778);
    POINTER_FALLOUT4(uint8_t, s_placementBlocked, 822284);

    // Settlement claims: without the right to build here, nothing is placed or moved (the outline stays red)
    if (apRef && !WorkshopService::MayBuildHere())
    {
        *s_placementStatus.Get() = 3;
        *s_placementBlocked.Get() = 1;
        World::Get().ctx().at<WorkshopService>().ShowRefusal(false);
        return false;
    }

    if (aAllowed || !apRef || !s_pBuildTools || !s_pBuildTools->IsPlaceAnywhere())
        return aAllowed;

    *s_placementStatus.Get() = 1;
    *s_placementBlocked.Get() = 0;
    return true;
}

bool HookCheckNewPlacement(TESObjectREFR* apRef)
{
    return AllowPlacement(apRef, RealCheckNewPlacement(apRef));
}

bool HookCheckMovedPlacement(TESObjectREFR* apRef)
{
    return AllowPlacement(apRef, RealCheckMovedPlacement(apRef));
}

void HookOutlinePlacement(uint32_t aState)
{
    if (aState == 3 && s_pBuildTools && s_pBuildTools->IsPlaceAnywhere() && WorkshopService::MayBuildHere())
        aState = 1;
    RealOutlinePlacement(aState);
}

// The quest aliases a reference is in (extra data type)
constexpr uint8_t kExtraAliasInstanceArray = 0x88;

bool HasExtraData(const TESObjectREFR* apRef, uint8_t aType) noexcept
{
    // ExtraDataList::GetByType (id 877089) on the reference's extra data (0x100)
    using TGetByType = const void*(void*, uint8_t);
    POINTER_FALLOUT4(TGetByType, s_getByType, 877090);
    void* pList = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(apRef) + 0x100);
    return pList && s_getByType.Get()(pList, aType);
}

// Form types the pick of objects without collision takes (STAT, TREE, FLOR), the extra data of trigger boxes and the
// like (ExtraPrimitive), and how large (half its bounds' diagonal) such an object can be: effects are often far larger
constexpr uint8_t kStaticType = 0x24;
constexpr uint8_t kTreeType = 0x28;
constexpr uint8_t kFloraType = 0x29;
constexpr uint8_t kExtraPrimitive = 0x67;
constexpr float kMaxUncollidableExtent = 1500.f;

// TESBoundObject::IsMarker (vfunc 0x50)
bool IsMarker(const TESForm* apBase) noexcept
{
    using TIsMarker = bool(const TESForm*);
    void* const* pVtable = *reinterpret_cast<void* const* const*>(apBase);
    return reinterpret_cast<TIsMarker*>(pVtable[0x50])(apBase);
}

// A static's mesh, lowercase (TESObjectSTAT's TESModel at 0x68, TESModel::GetModel is its vfunc 7)
std::string GetStaticModel(const TESForm* apStatic) noexcept
{
    const auto* pModel = reinterpret_cast<const uint8_t*>(apStatic) + 0x68;
    using TGetModel = const char*(const void*);
    void* const* pVtable = *reinterpret_cast<void* const* const*>(pModel);
    const char* pPath = reinterpret_cast<TGetModel*>(pVtable[7])(pModel);
    std::string path = pPath ? pPath : "";
    std::transform(path.begin(), path.end(), path.begin(), [](char aChar) { return static_cast<char>(std::tolower(static_cast<unsigned char>(aChar))); });
    return path;
}

// Effects and markers among statics: meshes under Effects (dust, light beams, fog, ...), of effects or markers
bool IsEffectModel(const TESForm* apStatic) noexcept
{
    const std::string path = GetStaticModel(apStatic);
    return path.empty() || path.rfind("effects\\", 0) == 0 || path.find("\\effects\\") != std::string::npos || path.find("marker") != std::string::npos ||
           path.rfind("fx", 0) == 0 || path.find("\\fx") != std::string::npos;
}

// Scrap anything changes what the game says about the outlined object only: the game also asks about every object
// around (entering build mode it asks whether each of thousands can be scrapped), and some of what asks acts on the
// answer for all of them (the ScrapAll console command scraps what can be)
bool IsOutlined(const TESObjectREFR* apRef) noexcept
{
    return apRef && apRef == WorkshopService::GetOutlinedObject();
}

bool HookIsWorkshopItem(const TESObjectREFR* apRef)
{
    if (RealIsWorkshopItem(apRef))
        return true;
    return s_pBuildTools && s_pBuildTools->IsScrapAnything() && IsOutlined(apRef) && s_pBuildTools->CanTreatAsWorkshopItem(apRef);
}

bool HookCanSelect(void* apContext, TESObjectREFR* apRef)
{
    if (RealCanSelect(apContext, apRef))
        return true;
    return s_pBuildTools && apRef && s_pBuildTools->IsScrapAnything() && s_pBuildTools->CanTreatAsWorkshopItem(apRef);
}

// Workshop 431436: whether the scrap button scraps a reference (see HookCanScrap)
using TCanScrap = bool(TESObjectREFR*, bool);
TCanScrap* RealCanScrap = nullptr;

// TESObjectREFR::GetDisplayFullName (1212056): build mode shows it for the outlined object; nothing for most scenery
// (Scrap Everything names it in its plugin), so scrap anything names it after its mesh (ScenerySalvage)
using TDisplayName = const char*(TESObjectREFR*);
TDisplayName* RealDisplayName = nullptr;

bool IsOutlined(const TESObjectREFR* apRef) noexcept;

const char* HookDisplayName(TESObjectREFR* apRef)
{
    const char* pName = RealDisplayName(apRef);
    if ((pName && *pName) || !s_pBuildTools || !apRef || !s_pBuildTools->IsScrapAnything() || !IsOutlined(apRef))
        return pName;
    const char* pLabel = ScenerySalvage::GetLabel(apRef->baseForm);
    return pLabel ? pLabel : pName;
}

// What scrapping gave (Workshop::ScrapReference's third argument, may be null): BSTArray<BSTTuple<TESBoundObject*,
// uint32_t>>
struct ScrapRewards
{
    struct Entry
    {
        TESBoundObject* pObject;
        uint32_t Count;
    };
    Entry* pEntries;
    uint32_t Capacity;
    uint32_t Pad;
    uint32_t Size;
};

void HookScrapReference(void* apContext, TESObjectREFR** appRef, void* apRewards)
{
    if (!s_pBuildTools || !appRef || !*appRef)
    {
        RealScrapReference(apContext, appRef, apRewards);
        return;
    }

    TESObjectREFR* pRef = *appRef;
    spdlog::info("Build tools: scrapping {:X}", pRef->formID);
    s_pBuildTools->BeforeScrap(pRef);

    // For undo: what it was and where, before it's gone (not dead bodies: their character is gone for everyone)
    std::optional<WorkshopService::ScrappedObject> scrapped;
    if (pRef->baseForm && pRef->formType != FormType::Character)
    {
        scrapped.emplace();
        scrapped->FormId = pRef->formID;
        scrapped->BaseId = pRef->baseForm->formID;
        scrapped->Transform = WorkshopService::GetTransform(pRef);
        if (TESWorldSpace* pWorldSpace = pRef->GetWorldSpace())
            scrapped->WorldSpaceId = pWorldSpace->formID;
        else if (TESObjectCELL* pCell = pRef->GetParentCellEx())
            scrapped->CellId = pCell->formID;
    }

    // Scrap anything: what the game has no scrap recipe for gives what it's made of (ScenerySalvage)
    Vector<std::pair<uint32_t, uint32_t>> salvage;
    std::string name;
    if (scrapped && s_pBuildTools->IsScrapAnything() && !RealCanScrap(pRef, false))
    {
        salvage = ScenerySalvage::GetSalvage(pRef);
        const char* pName = RealDisplayName(pRef);
        const char* pLabel = pName && *pName ? pName : ScenerySalvage::GetLabel(pRef->baseForm);
        name = pLabel ? pLabel : "";
    }

    RealScrapReference(apContext, appRef, apRewards);

    if (scrapped)
    {
        if (const auto* pRewards = static_cast<const ScrapRewards*>(apRewards); pRewards && pRewards->pEntries)
        {
            for (uint32_t i = 0; i < pRewards->Size; ++i)
            {
                if (pRewards->pEntries[i].pObject && pRewards->pEntries[i].Count)
                    scrapped->Components.emplace_back(pRewards->pEntries[i].pObject->formID, pRewards->pEntries[i].Count);
            }
        }
        if (scrapped->Components.empty() && !salvage.empty())
        {
            s_pBuildTools->GiveSalvage(salvage, name);
            scrapped->Components = salvage;
        }
        World::Get().ctx().at<WorkshopService>().RecordScrap(std::move(*scrapped));
    }
}

// Workshop 431436: whether the scrap button scraps a reference (the bool: also what is stored rather than scrapped).
// Says no to anything without a scrap recipe: scrap anything says yes to what it lets build mode outline.

// The scrap button in build mode (1292439): scraps the outlined object (431436, then a confirmation, then
// ScrapReference) unless the flag 816517 is set (an edit going on: set by 375724 from the per-frame workshop update,
// cleared by placing and by UpdateActiveEdit)
using TScrapButton = bool(void*);
TScrapButton* RealScrapButton = nullptr;

bool HookScrapButton(void* apContext)
{
    TESObjectREFR* pOutlined = WorkshopService::GetOutlinedObject();
    POINTER_FALLOUT4(uint8_t, s_editFlag, 816518);
    // Settlement claims: the game won't scrap or store it (HookCanScrap, HookCanStore), say why
    if (pOutlined && !WorkshopService::MayScrapHere())
        World::Get().ctx().at<WorkshopService>().ShowRefusal(true);
    if (pOutlined && s_pBuildTools)
    {
        const char* pRefusal = s_pBuildTools->WhyNotWorkshopItem(pOutlined);
        spdlog::info("Build tools: scrap pressed on {:X} (base {:X}): edit flag {}, the game would scrap it {}, scrap anything {}", pOutlined->formID,
                     pOutlined->baseForm ? pOutlined->baseForm->formID : 0, *s_editFlag.Get(), RealCanScrap(pOutlined, false) ? "yes" : "no",
                     pRefusal ? fmt::format("no ({})", pRefusal) : std::string("yes"));
    }
    else
        spdlog::info("Build tools: scrap pressed with nothing outlined, edit flag {}", *s_editFlag.Get());
    return RealScrapButton(apContext);
}

bool HookCanScrap(TESObjectREFR* apRef, bool aStore)
{
    // Settlement claims: without the right to scrap here, nothing can be (in build mode only, see MayScrapHere)
    if (!WorkshopService::MayScrapHere())
        return false;
    if (RealCanScrap(apRef, aStore))
        return true;
    return s_pBuildTools && s_pBuildTools->IsScrapAnything() && IsOutlined(apRef) && s_pBuildTools->CanTreatAsWorkshopItem(apRef);
}

// Workshop 829383: whether the store button puts a reference back in the workbench. Read from the 1.10.163 exe: storing
// doesn't go through 431436 (the scrap button 1292439, CanSelect 1371490 and the scrap/store functor 286947 ask this
// one, then store with 666921), so the claims refuse it here too.
using TCanStore = bool(TESObjectREFR*);
TCanStore* RealCanStore = nullptr;

bool HookCanStore(TESObjectREFR* apRef)
{
    if (!WorkshopService::MayScrapHere())
        return false;
    return RealCanStore(apRef);
}

// Settlements load their scenery as separate objects, what Scrap Everything's plugin does to their cells (166 of the
// 169 it changes). The game merges most of the world's scenery into combined meshes, cell by cell (precombines),
// which build mode can neither outline nor take apart. A cell read from its plugin (TESObjectCELL::Load) has:
// - its merged objects and their meshes (XCRI): extra data ExtraCombinedRefs, type 0xC5;
// - the objects whose collision is in its merged physics mesh and that precomputed visibility hides (XPRI):
//   ExtraPreVisRefs, type 0x15.
// Without both, it loads each object with its own mesh and collision, which precomputed visibility doesn't hide, like
// Scrap Everything's cells (they have neither). The game itself takes both off some cells as it reads its plugins
// (ExtraDataList::RemoveByType, id 76237, in 1333622): done the same way to settlements' cells as the game reads them,
// before they're in the world. (Taking the merged objects off alone left them without collision: it
// was still expected from the merged physics mesh.) It costs some frame rate in settlements.
constexpr std::array<uint8_t, 2> kExtraPrecombines{0xC5, 0x15};
// Fallout4.esm: keyword of the locations of settlements
constexpr uint32_t kLocTypeWorkshopSettlement = 0x83C9A;
// The game's BSTArray of pointers
struct GameFormArray
{
    TESForm** pData;
    uint32_t Capacity;
    uint32_t Pad;
    uint32_t Size;
    uint32_t Pad2;
};
static_assert(sizeof(GameFormArray) == 0x18);

using TCellLoad = bool(TESObjectCELL*, void*);
TCellLoad* RealCellLoad = nullptr;

// The cells the game read from its plugins: while it loads its data, looked at once it has (the settlements are known
// then); later, the cells of the world outside it reads as they're needed are looked at right away
std::mutex s_cellsReadLock;
Vector<TESObjectCELL*> s_cellsRead;
struct SettlementForms;
std::atomic<const SettlementForms*> s_pSettlementForms{nullptr};
// How many form types there are (TESDataHandler's form arrays, from 0x68 to 0xF50)
constexpr size_t kFormTypeCount = (0xF50 - 0x68) / 0x18;

// Form types of BGSEncounterZone (ECZN) and BGSLocation (LCTN)
constexpr uint8_t kEncounterZoneFormType = 0x6A;
constexpr uint8_t kLocationFormType = 0x6B;
// A cell's extra data giving its encounter zone and its location (their form at +0x18)
constexpr uint8_t kExtraEncounterZone = 0x70;
constexpr uint8_t kExtraLocation = 0x89;

// What a form field holds before and after the game sets it up: the form's id (until then), or the form
const TESForm* ResolveFormValue(uint64_t aValue) noexcept
{
    if (!aValue)
        return nullptr;
    if (aValue <= 0xFFFFFFFFull)
        return TESForm::GetById(static_cast<uint32_t>(aValue));
    return reinterpret_cast<const TESForm*>(aValue);
}

// The game's list of every form of a type (TESDataHandler's form arrays, by form type, from 0x68)
Set<const TESForm*> GetAllForms(uint8_t aFormType) noexcept
{
    Set<const TESForm*> forms;
    const auto* pManager = reinterpret_cast<const uint8_t*>(ModManager::Get());
    if (!pManager)
        return forms;

    const auto& array = *reinterpret_cast<const GameFormArray*>(pManager + 0x68 + aFormType * sizeof(GameFormArray));
    for (uint32_t i = 0; array.pData && i < array.Size; ++i)
    {
        if (const TESForm* pForm = array.pData[i]; pForm && static_cast<uint8_t>(pForm->formType) == aFormType)
            forms.insert(pForm);
    }
    return forms;
}

// Settlements: their locations (and those inside them) and the encounter zones of those. A cell is matched against
// them by what it refers to, never by reading it: until the game loads a cell for play, what it refers to are form
// ids, not forms yet.
struct SettlementForms
{
    Set<const TESForm*> Locations;
    Set<const TESForm*> Zones;
};

SettlementForms FindSettlementForms() noexcept
{
    SettlementForms settlements;
    const TESForm* pSettlementKeyword = TESForm::GetById(kLocTypeWorkshopSettlement);
    if (!pSettlementKeyword)
        return settlements;

    const Set<const TESForm*> all = GetAllForms(kLocationFormType);

    // BGSLocation: its keywords (BGSKeywordForm at 0x30: keywords 0x40, count 0x48), its parent location at 0x50
    auto hasSettlementKeyword = [pSettlementKeyword](const TESForm* apLocation)
    {
        const auto* pBytes = reinterpret_cast<const uint8_t*>(apLocation);
        const auto* const* ppKeywords = *reinterpret_cast<const TESForm* const* const*>(pBytes + 0x40);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(pBytes + 0x48);
        for (uint32_t i = 0; ppKeywords && i < count; ++i)
        {
            if (ppKeywords[i] == pSettlementKeyword)
                return true;
        }
        return false;
    };

    for (const TESForm* pLocation : all)
    {
        const TESForm* pCurrent = pLocation;
        for (int depth = 0; depth < 8 && pCurrent && all.contains(pCurrent); ++depth)
        {
            if (hasSettlementKeyword(pCurrent))
            {
                settlements.Locations.insert(pLocation);
                break;
            }
            pCurrent = *reinterpret_cast<const TESForm* const*>(reinterpret_cast<const uint8_t*>(pCurrent) + 0x50);
        }
    }

    // BGSEncounterZone: its location at 0x28
    for (const TESForm* pZone : GetAllForms(kEncounterZoneFormType))
    {
        if (settlements.Locations.contains(ResolveFormValue(*reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(pZone) + 0x28))))
            settlements.Zones.insert(pZone);
    }

    spdlog::info("Settlements: {} of the game's {} locations are settlements or in one, {} encounter zones are theirs", settlements.Locations.size(), all.size(),
                 settlements.Zones.size());
    return settlements;
}

// What a cell's extra data refers to: a form, or the id of one until the game loads the cell for play
const TESForm* GetCellExtraForm(void* apList, uint8_t aType) noexcept
{
    // ExtraDataList::GetByType (id 877089)
    using TGetByType = const uint8_t*(void*, uint8_t);
    POINTER_FALLOUT4(TGetByType, s_getByType, 877090);
    const uint8_t* pData = s_getByType.Get()(apList, aType);
    return pData ? ResolveFormValue(*reinterpret_cast<const uint64_t*>(pData + 0x18)) : nullptr;
}

// For measuring what the separate objects cost: with the environment variable RD_KEEP_PRECOMBINES set, settlements keep
// their precombined meshes (their scenery can't be scrapped then)
const bool s_keepPrecombines = std::getenv("RD_KEEP_PRECOMBINES") != nullptr;

// Takes a cell's precombined data off if it is (part of) a settlement, true if it had some
bool UncombineSettlementCell(TESObjectCELL* apCell, const SettlementForms& acSettlements) noexcept
{
    if (s_keepPrecombines)
    {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            spdlog::warn("Settlements keep their precombined meshes (RD_KEEP_PRECOMBINES): scenery can't be scrapped");
        return false;
    }

    void* pList = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(apCell) + 0x48);
    if (!pList)
        return false;

    if (!acSettlements.Zones.contains(GetCellExtraForm(pList, kExtraEncounterZone)) && !acSettlements.Locations.contains(GetCellExtraForm(pList, kExtraLocation)))
        return false;

    // ExtraDataList::RemoveByType (id 76237): true if it had one
    using TRemoveByType = bool(void*, uint8_t);
    POINTER_FALLOUT4(TRemoveByType, s_removeByType, 76238);
    bool removed = false;
    for (const uint8_t type : kExtraPrecombines)
        removed = s_removeByType.Get()(pList, type) || removed;
    return removed;
}
// TESObjectCELL::Load (TESForm vfunc 0x9, id 165674): reads the cell from its plugin, every cell while the game loads
// its data, the cells of the world outside again as they're needed
bool HookCellLoad(TESObjectCELL* apCell, void* apFile)
{
    const bool loaded = RealCellLoad(apCell, apFile);
    if (!loaded || !apCell)
        return loaded;

    {
        std::scoped_lock _{s_cellsReadLock};
        s_cellsRead.push_back(apCell);
    }

    if (const SettlementForms* pSettlements = s_pSettlementForms.load(); pSettlements && UncombineSettlementCell(apCell, *pSettlements))
        spdlog::info("Settlement cell {:X}: its objects load one by one (no precombined data)", apCell->formID);
    return loaded;
}
// Objects without collision (hanging vines, some plants and debris): build mode outlines what a ray from the crosshair
// hits, against collision, so it goes through them. Each frame 1411424 casts that ray ({origin, direction} in game
// units, as far as the float 437786) and hands what it picked, or nothing, with the hit point to 334823, which outlines
// it. When an object of the settlement without collision is in front of that along the ray, 334823 gets that object.
using TPick = void(void*, const float*, bool);
TPick* RealPick = nullptr;
using TSelect = void(void*, uint32_t*, float*);
TSelect* RealSelect = nullptr;

// The ray of the pick running now (the game's main thread)
bool s_pickRunning = false;
NiPoint3 s_pickOrigin;
NiPoint3 s_pickDirection;
uint32_t s_lastUncollidableTarget = 0;

void HookPick(void* apResult, const float* apRay, bool aStopAtFirst)
{
    if (apRay)
    {
        s_pickOrigin = NiPoint3(glm::vec3(apRay[0], apRay[1], apRay[2]));
        s_pickDirection = NiPoint3(glm::vec3(apRay[3], apRay[4], apRay[5]));
        s_pickRunning = true;
    }
    RealPick(apResult, apRay, aStopAtFirst);
    s_pickRunning = false;
}

void HookSelect(void* apUnused, uint32_t* apHandle, float* apHitPoint)
{
    if (!s_pickRunning || !s_pBuildTools || !s_pBuildTools->IsScrapAnything() || !apHandle || !apHitPoint)
    {
        RealSelect(apUnused, apHandle, apHitPoint);
        return;
    }

    // As far as what the game picked, or as far as the pick reaches
    POINTER_FALLOUT4(float, s_pickDistance, 437787);
    float maxDistance = *s_pickDistance.Get();
    const bool picked = *apHandle && *apHandle != *TESObjectREFR::GetNullHandle() && TESObjectREFR::GetByHandle(*apHandle);
    if (picked)
    {
        const float dx = apHitPoint[0] - s_pickOrigin.x;
        const float dy = apHitPoint[1] - s_pickOrigin.y;
        const float dz = apHitPoint[2] - s_pickOrigin.z;
        maxDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float distance = 0.f;
    TESObjectREFR* pTarget = s_pBuildTools->FindUncollidableTarget(s_pickOrigin, s_pickDirection, maxDistance, distance);
    if (!pTarget)
    {
        s_lastUncollidableTarget = 0;
        RealSelect(apUnused, apHandle, apHitPoint);
        return;
    }

    if (pTarget->formID != s_lastUncollidableTarget)
    {
        s_lastUncollidableTarget = pTarget->formID;
        spdlog::info("Build tools: crosshair on {:X} (base {:X}, type {:X}, {}), which has no collision, {:.0f} away", pTarget->formID, pTarget->baseForm->formID,
                     static_cast<uint8_t>(pTarget->baseForm->formType), static_cast<uint8_t>(pTarget->baseForm->formType) == kStaticType ? GetStaticModel(pTarget->baseForm) : std::string(), distance);
    }

    uint32_t handle = pTarget->GetHandle().handle.iBits;
    float hitPoint[3] = {s_pickOrigin.x + s_pickDirection.x * distance, s_pickOrigin.y + s_pickDirection.y * distance, s_pickOrigin.z + s_pickDirection.z * distance};
    RealSelect(apUnused, &handle, hitPoint);
}

// Whether a 3D object or any part of it has collision: NiAVObject::collisionObject (0x100); a node's parts are its
// children (NiNode at 0x120: data 0x128, capacity 0x130, used 0x132; NiObject::IsNode is vfunc 3)
bool HasCollision(const uint8_t* apObject, int aDepth = 0) noexcept
{
    if (!apObject || aDepth > 16)
        return false;
    if (*reinterpret_cast<void* const*>(apObject + 0x100))
        return true;

    using TIsNode = const uint8_t*(const uint8_t*);
    void* const* pVtable = *reinterpret_cast<void* const* const*>(apObject);
    const uint8_t* pNode = reinterpret_cast<TIsNode*>(pVtable[3])(apObject);
    if (!pNode)
        return false;

    const auto* const* ppChildren = *reinterpret_cast<const uint8_t* const* const*>(pNode + 0x128);
    const uint16_t capacity = *reinterpret_cast<const uint16_t*>(pNode + 0x130);
    const uint16_t used = std::min(*reinterpret_cast<const uint16_t*>(pNode + 0x132), capacity);
    for (uint16_t i = 0; ppChildren && i < used; ++i)
    {
        if (HasCollision(ppChildren[i], aDepth + 1))
            return true;
    }
    return false;
}

// Where along a ray (world units) it enters a reference's bounds: its base's object bounds (OBND), placed like its 3D
std::optional<float> RayEntersBounds(const TESObjectREFR* apRef, const uint8_t* apNode, const NiPoint3& acOrigin, const NiPoint3& acDirection, float aMaxDistance) noexcept
{
    // TESBoundObject::boundData (0x20): min x, y, z, max x, y, z
    const auto* pBounds = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(apRef->baseForm) + 0x20);
    // NiAVObject::world (0x70): rotation (3 rows of 4 floats), translation, scale. World = rotation * local * scale +
    // translation, so local = transposed rotation * (world - translation) / scale.
    const float* pWorld = reinterpret_cast<const float*>(apNode + 0x70);
    const float* pTranslation = pWorld + 12;
    const float scale = pWorld[15];
    if (scale <= 0.f)
        return std::nullopt;

    const float relative[3] = {acOrigin.x - pTranslation[0], acOrigin.y - pTranslation[1], acOrigin.z - pTranslation[2]};
    const float direction[3] = {acDirection.x, acDirection.y, acDirection.z};
    float tMin = 0.f;
    float tMax = aMaxDistance;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float origin = (pWorld[axis] * relative[0] + pWorld[4 + axis] * relative[1] + pWorld[8 + axis] * relative[2]) / scale;
        const float step = (pWorld[axis] * direction[0] + pWorld[4 + axis] * direction[1] + pWorld[8 + axis] * direction[2]) / scale;
        const float low = pBounds[axis];
        const float high = pBounds[3 + axis];
        if (std::abs(step) < 1e-6f)
        {
            if (origin < low || origin > high)
                return std::nullopt;
            continue;
        }

        float t1 = (low - origin) / step;
        float t2 = (high - origin) / step;
        if (t1 > t2)
            std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax)
            return std::nullopt;
    }
    return tMin;
}

TESObjectREFR* GetOutlinedObject() noexcept
{
    return WorkshopService::GetOutlinedObject();
}

bool IsWorkshopItem(const TESObjectREFR* apRef) noexcept
{
    POINTER_FALLOUT4(TIsWorkshopItem, s_isWorkshopItem, 1386904);
    return s_isWorkshopItem.Get()(apRef);
}

bool IsInBuildableArea(const TESObjectREFR* apWorkshop, const NiPoint3& acPosition) noexcept
{
    // Workshop::IsLocationWithinBuildableArea (CommonLibF4 id 990965)
    using TIsLocationWithinBuildableArea = bool(const TESObjectREFR*, const NiPoint3*);
    POINTER_FALLOUT4(TIsLocationWithinBuildableArea, s_isLocationWithinBuildableArea, 990966);
    return s_isLocationWithinBuildableArea.Get()(apWorkshop, &acPosition);
}

void ShowMessage(const std::string& acText) noexcept
{
    // SendHUDMessage::ShowHUDMessage (CommonLibF4 id 1163005): message, sound, throttle, warning
    using TShowHUDMessage = void(const char*, const char*, bool, bool);
    POINTER_FALLOUT4(TShowHUDMessage, s_showHUDMessage, 1163006);
    s_showHUDMessage.Get()(acText.c_str(), "", false, false);
}

const char* AxisName(int aAxis) noexcept
{
    return aAxis == 0 ? "X" : aAxis == 1 ? "Y" : "Z";
}

// Keys the game or our overlay already use, never given to the build tools
bool IsReservedKey(int aVirtualKey) noexcept
{
    if (aVirtualKey <= 0 || aVirtualKey > 0xFE)
        return true;
    // Letters (game controls, build mode), number row (favorites)
    if ((aVirtualKey >= 'A' && aVirtualKey <= 'Z') || (aVirtualKey >= '0' && aVirtualKey <= '9'))
        return true;

    switch (aVirtualKey)
    {
    case VK_LBUTTON:
    case VK_RBUTTON:
    case VK_CANCEL:
    case VK_MBUTTON:
    case VK_XBUTTON1:
    case VK_XBUTTON2:
    case VK_TAB:
    case VK_RETURN:
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_MENU:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LCONTROL:
    case VK_RCONTROL: // overlay
    case VK_LMENU:
    case VK_RMENU:
    case VK_CAPITAL:
    case VK_ESCAPE:
    case VK_SPACE:
    case VK_LEFT: // build menu
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_NUMLOCK:
    case VK_F2: // overlay
    case VK_F3: // debug info
    case VK_F5: // quick save
    case VK_F6: // test builds: connect, Discord overlay
    case VK_F7:
    case VK_F8:
    case VK_F9: // quick load
    case VK_F10: // Windows' menu key
    case VK_F12: // Steam screenshot
    case VK_OEM_3: // console
        return true;
    default: return false;
    }
}

std::filesystem::path ConfigPath() noexcept
{
    return TiltedPhoques::GetPath() / "config" / "BuildTools.ini";
}

std::string Trim(std::string_view aText) noexcept
{
    const auto begin = aText.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos)
        return {};
    const auto end = aText.find_last_not_of(" \t\r");
    return std::string(aText.substr(begin, end - begin + 1));
}
} // namespace

const std::array<BuildToolsService::ActionInfo, static_cast<size_t>(BuildToolsService::Action::Count)> BuildToolsService::s_actions{{
    {Action::TogglePlaceAnywhere, "placeAnywhere", VK_F1, false},
    {Action::ToggleScrapAnything, "scrapAnything", VK_F4, false},
    {Action::ToggleObjectSnap, "objectSnap", VK_F11, false},
    {Action::ToggleGridSnap, "gridSnap", VK_END, false},
    {Action::ChangeRotationAxis, "rotationAxis", VK_INSERT, false},
    {Action::RotateLeft, "rotateLeft", VK_OEM_COMMA, true},
    {Action::RotateRight, "rotateRight", VK_OEM_PERIOD, true},
    {Action::RotateStepDown, "rotateStepDown", VK_SUBTRACT, false},
    {Action::RotateStepUp, "rotateStepUp", VK_ADD, false},
    {Action::Level, "level", VK_NUMPAD0, false},
    {Action::MoveUp, "moveUp", VK_NUMPAD8, true},
    {Action::MoveDown, "moveDown", VK_NUMPAD2, true},
    {Action::MoveLeft, "moveLeft", VK_NUMPAD4, true},
    {Action::MoveRight, "moveRight", VK_NUMPAD6, true},
    {Action::MoveCloser, "moveCloser", VK_NUMPAD7, true},
    {Action::MoveAway, "moveAway", VK_NUMPAD9, true},
    {Action::MoveStepDown, "moveStepDown", VK_DIVIDE, false},
    {Action::MoveStepUp, "moveStepUp", VK_MULTIPLY, false},
    {Action::ToggleWorldAxes, "worldAxes", VK_HOME, false},
    {Action::ScaleDown, "scaleDown", VK_NUMPAD1, true},
    {Action::ScaleUp, "scaleUp", VK_NUMPAD3, true},
    {Action::ScaleStepDown, "scaleStepDown", VK_NEXT, false},
    {Action::ScaleStepUp, "scaleStepUp", VK_PRIOR, false},
    {Action::ScaleReset, "scaleReset", VK_DECIMAL, false},
    {Action::Redraw, "redraw", VK_NUMPAD5, false},
    {Action::Undo, "undo", VK_BACK, false},
    {Action::Remember, "remember", VK_OEM_PLUS, false},  // =
    {Action::PastePositionX, "pastePositionX", VK_OEM_4, false}, // [
    {Action::PastePositionY, "pastePositionY", VK_OEM_6, false}, // ]
    {Action::PastePositionZ, "pastePositionZ", VK_OEM_5, false}, // backslash
    {Action::PasteAngles, "pasteAngles", VK_OEM_1, false},       // ;
    {Action::PasteScale, "pasteScale", VK_OEM_7, false},         // '
    {Action::PasteAll, "pasteAll", VK_OEM_2, false},             // /
}};

static TiltedPhoques::Initializer s_buildToolsHooks(
    []()
    {
        POINTER_FALLOUT4(TCheckPlacement, s_checkNewPlacement, 238048);
        POINTER_FALLOUT4(TCheckPlacement, s_checkMovedPlacement, 978468);
        POINTER_FALLOUT4(TOutlinePlacement, s_outlinePlacement, 1565252);
        POINTER_FALLOUT4(TIsWorkshopItem, s_isWorkshopItem, 1386904);
        POINTER_FALLOUT4(TScrapReference, s_scrapReference, 636328);
        POINTER_FALLOUT4(TCanSelect, s_canSelect, 1371491);
        POINTER_FALLOUT4(TCanScrap, s_canScrap, 431437);
        POINTER_FALLOUT4(TCanStore, s_canStore, 829384);
        POINTER_FALLOUT4(TBuildableArea, s_buildableArea, 990966);
        POINTER_FALLOUT4(TDisplayName, s_displayName, 1212057);
        POINTER_FALLOUT4(TScrapButton, s_scrapButton, 1292440);
        POINTER_FALLOUT4(TCellLoad, s_cellLoad, 165675);
        POINTER_FALLOUT4(TPick, s_pick, 1411425);
        POINTER_FALLOUT4(TSelect, s_select, 334824);

        RealCanScrap = s_canScrap.Get();
        RealCanStore = s_canStore.Get();
        RealBuildableArea = s_buildableArea.Get();
        RealDisplayName = s_displayName.Get();
        RealScrapButton = s_scrapButton.Get();
        RealCellLoad = s_cellLoad.Get();
        RealPick = s_pick.Get();
        RealSelect = s_select.Get();
        TP_HOOK(&RealCanScrap, HookCanScrap);
        TP_HOOK(&RealCanStore, HookCanStore);
        TP_HOOK(&RealBuildableArea, HookBuildableArea);
        TP_HOOK(&RealDisplayName, HookDisplayName);
        TP_HOOK(&RealScrapButton, HookScrapButton);
        TP_HOOK(&RealCellLoad, HookCellLoad);
        TP_HOOK(&RealPick, HookPick);
        TP_HOOK(&RealSelect, HookSelect);

        RealCheckNewPlacement = s_checkNewPlacement.Get();
        RealCheckMovedPlacement = s_checkMovedPlacement.Get();
        RealOutlinePlacement = s_outlinePlacement.Get();
        RealIsWorkshopItem = s_isWorkshopItem.Get();
        RealScrapReference = s_scrapReference.Get();
        RealCanSelect = s_canSelect.Get();

        TP_HOOK(&RealCheckNewPlacement, HookCheckNewPlacement);
        TP_HOOK(&RealCheckMovedPlacement, HookCheckMovedPlacement);
        TP_HOOK(&RealOutlinePlacement, HookOutlinePlacement);
        TP_HOOK(&RealIsWorkshopItem, HookIsWorkshopItem);
        TP_HOOK(&RealScrapReference, HookScrapReference);
        TP_HOOK(&RealCanSelect, HookCanSelect);
    });

// The game's data is loaded, as far as can be told (the main menu shows while it loads, and the game's own flag is off
// before loading starts): every active plugin has its load index (Mod::standardId, 0xFF until the load order is
// compiled) and no form has been added for a second. How many forms it has then, 0 before.
size_t CountLoadedForms() noexcept
{
    static size_t s_lastCount = 0;
    static std::chrono::steady_clock::time_point s_since{};

    const auto* pManager = reinterpret_cast<const uint8_t*>(ModManager::Get());
    if (!pManager)
        return 0;

    // TESDataHandler: loadingFiles (0x1010); compiledFileCollection: its files and small files (BSTArray<TESFile*>
    // at 0xFC0 and 0xFD8)
    if (pManager[0x1010])
        return 0;
    for (const size_t offset : {0xFC0, 0xFD8})
    {
        const auto& files = *reinterpret_cast<const GameFormArray*>(pManager + offset);
        if (offset == 0xFC0 && !files.Size)
            return 0;
        for (uint32_t i = 0; files.pData && i < files.Size; ++i)
        {
            const auto* pFile = reinterpret_cast<const Mod*>(files.pData[i]);
            if (!pFile || !pFile->IsLoaded())
                return 0;
        }
    }

    size_t count = 0;
    for (size_t type = 0; type < kFormTypeCount; ++type)
        count += reinterpret_cast<const GameFormArray*>(pManager + 0x68)[type].Size;

    const auto now = std::chrono::steady_clock::now();
    if (count != s_lastCount)
    {
        s_lastCount = count;
        s_since = now;
        return 0;
    }
    return now - s_since >= 1s ? count : 0;
}

void UncombineSettlementCells(bool aInWorld) noexcept
{
    // Each list of settlements found stays: the hook may be looking at one from a thread reading cells
    static Vector<std::unique_ptr<SettlementForms>> s_versions;
    static size_t s_builtForForms = 0;
    static size_t s_cellsChecked = 0;
    static bool s_reachedWorld = false;

    // Found (again) while at the main menu once the game's data stops changing, and in the world if not found yet
    s_reachedWorld = s_reachedWorld || aInWorld;
    const size_t forms = s_reachedWorld ? 0 : CountLoadedForms();
    if ((forms && forms != s_builtForForms) || (s_reachedWorld && s_versions.empty()))
    {
        auto pSettlements = std::make_unique<SettlementForms>(FindSettlementForms());
        if (!pSettlements->Locations.empty())
        {
            s_pSettlementForms = pSettlements.get();
            s_versions.push_back(std::move(pSettlements));
            s_builtForForms = forms;
            s_cellsChecked = 0;
        }
    }
    if (s_versions.empty())
        return;

    // The cells read before, or since the settlements were found again (the hook looks at those read after). Not once
    // in the world: those may be in it already.
    Vector<TESObjectCELL*> cells;
    {
        std::scoped_lock _{s_cellsReadLock};
        if (s_reachedWorld)
            s_cellsChecked = s_cellsRead.size();
        if (s_cellsChecked >= s_cellsRead.size())
            return;
        cells.assign(s_cellsRead.begin() + s_cellsChecked, s_cellsRead.end());
        s_cellsChecked = s_cellsRead.size();
    }

    size_t uncombined = 0;
    for (TESObjectCELL* pCell : cells)
    {
        if (UncombineSettlementCell(pCell, *s_versions.back()))
            ++uncombined;
    }
    if (uncombined || cells.size() > 1)
        spdlog::info("Settlements: {} of {} cells read load their objects one by one (no precombined data)", uncombined, cells.size());
}
BuildToolsService::BuildToolsService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_workshop(aWorld.ctx().at<WorkshopService>())
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&BuildToolsService::OnUpdate>(this);

    ResetKeys();
    LoadConfig();
    ReadMcmSettings(false);

    s_pBuildTools = this;
}

BuildToolsService::~BuildToolsService() noexcept
{
    s_pBuildTools = nullptr;
}

const char* BuildToolsService::WhyNotWorkshopItem(const TESObjectREFR* apRef) const noexcept
{
    if (!apRef || !apRef->baseForm)
        return "no base object";
    if (!m_workshop.IsInBuildMode())
        return "not in build mode";

    // Not doors (they connect places), what a quest uses right now (in one of its aliases: extra data 0x88 on the
    // reference's extra data at 0x100), named characters (persistent) or what can't be seen (markers). Other
    // persistent objects are much of a settlement's scenery: those can go.
    if (static_cast<uint8_t>(apRef->baseForm->formType) == kDoorType)
        return "a door";
    if ((apRef->flags & kPersistentFlag) && apRef->formType == FormType::Character)
        return "a named character";
    if (HasExtraData(apRef, kExtraAliasInstanceArray))
        return "used by a quest";
    if (apRef->IsDisabled())
        return "disabled";
    if (!const_cast<TESObjectREFR*>(apRef)->GetNiNode())
        return "no 3D of its own";

    // People and creatures only once they're dead bodies, and never players (they get back up)
    if (apRef->formType == FormType::Character)
    {
        auto* pActor = Cast<Actor>(const_cast<TESObjectREFR*>(apRef));
        if (!pActor || pActor == PlayerCharacter::Get() || pActor->GetExtension()->IsPlayer())
            return "a player";
        if (!pActor->IsDead())
            return "alive";
    }

    const auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(m_workshop.GetBuildWorkshopId()));
    if (!pWorkshop)
        return "workbench not found";
    if (pWorkshop == apRef)
        return "the workbench";

    return IsInBuildableArea(pWorkshop, apRef->position) ? nullptr : "outside the build area";
}

TESObjectREFR* BuildToolsService::FindUncollidableTarget(const NiPoint3& acOrigin, const NiPoint3& acDirection, float aMaxDistance, float& aDistance) const noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pPlayerCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!m_workshop.IsInBuildMode() || !pPlayerCell)
        return nullptr;

    // The view hasn't changed since the last frame: the same answer (checked again now and then, objects load)
    const auto now = Clock::now();
    UncollidablePick& last = m_lastUncollidablePick;
    auto same = [](const NiPoint3& acA, const NiPoint3& acB, float aTolerance)
    { return std::abs(acA.x - acB.x) <= aTolerance && std::abs(acA.y - acB.y) <= aTolerance && std::abs(acA.z - acB.z) <= aTolerance; };
    if (now - last.At < 250ms && same(acOrigin, last.Origin, 0.5f) && same(acDirection, last.Direction, 0.0005f) && std::abs(aMaxDistance - last.MaxDistance) < 1.f)
    {
        auto* pLast = last.TargetId ? Cast<TESObjectREFR>(TESForm::GetById(last.TargetId)) : nullptr;
        if (!last.TargetId || (pLast && !pLast->IsDisabled()))
        {
            aDistance = last.Distance;
            return pLast;
        }
    }

    // The player's cell, and outside the cells around it
    Vector<TESObjectCELL*> cells{pPlayerCell};
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
    {
        cells.clear();
        constexpr float kCellSize = 4096.f;
        const int32_t x = static_cast<int32_t>(std::floor(pPlayer->position.x / kCellSize));
        const int32_t y = static_cast<int32_t>(std::floor(pPlayer->position.y / kCellSize));
        for (int32_t dx = -1; dx <= 1; ++dx)
        {
            for (int32_t dy = -1; dy <= 1; ++dy)
            {
                if (TESObjectCELL* pCell = ModManager::Get()->GetCellFromCoordinates(x + dx, y + dy, pWorldSpace, false))
                    cells.push_back(pCell);
            }
        }
    }

    if (m_uncollidables.size() > 64)
        m_uncollidables.clear();

    TESObjectREFR* pBest = nullptr;
    float best = aMaxDistance;
    for (TESObjectCELL* pCell : cells)
    {
        // Its objects without collision, found again when its objects change or now and then (their 3D loads later)
        CellUncollidables& scan = m_uncollidables[pCell->formID];
        if (scan.pObjects != pCell->objects.data || scan.Count != pCell->objects.length || now - scan.At > 2s)
            ScanUncollidables(pCell, scan, now);

        for (const auto& [formId, extent] : scan.Candidates)
        {
            auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
            if (!pRef || pRef->IsDisabled())
                continue;

            // Quickly: is the ray anywhere near it (a sphere around its bounds)
            const float radius = extent * pRef->GetScale();
            const float vx = pRef->position.x - acOrigin.x;
            const float vy = pRef->position.y - acOrigin.y;
            const float vz = pRef->position.z - acOrigin.z;
            const float along = vx * acDirection.x + vy * acDirection.y + vz * acDirection.z;
            if (along < -radius || along > best + radius || vx * vx + vy * vy + vz * vz - along * along > radius * radius)
                continue;

            // The ray goes through its bounds before anything else found
            const auto* pNode = reinterpret_cast<const uint8_t*>(pRef->GetNiNode());
            if (!pNode)
                continue;
            const auto entry = RayEntersBounds(pRef, pNode, acOrigin, acDirection, best);
            if (!entry || *entry >= best || WhyNotWorkshopItem(pRef))
                continue;

            best = *entry;
            pBest = pRef;
        }
    }

    last = {now, acOrigin, acDirection, aMaxDistance, pBest ? pBest->formID : 0, best};
    aDistance = best;
    return pBest;
}

void BuildToolsService::ScanUncollidables(TESObjectCELL* apCell, CellUncollidables& aScan, Clock::time_point aNow) const noexcept
{
    aScan.pObjects = apCell->objects.data;
    aScan.Count = apCell->objects.length;
    aScan.At = aNow;
    aScan.Candidates.clear();
    if (!apCell->objects.data)
        return;

    if (m_collisionCache.size() > 50000)
        m_collisionCache.clear();

    for (TESObjectREFR* pRef : apCell->objects)
    {
        if (!pRef || !pRef->baseForm || pRef == PlayerCharacter::Get() || pRef->IsDisabled())
            continue;

        // Visible scenery only: statics, trees and plants, not markers, trigger boxes or effects (dust, light beams,
        // fog: no collision, often large), which would take the crosshair from what's behind them
        const TESForm* pBase = pRef->baseForm;
        const uint8_t baseType = static_cast<uint8_t>(pBase->formType);
        if (baseType != kStaticType && baseType != kTreeType && baseType != kFloraType)
            continue;
        if (IsMarker(pBase) || HasExtraData(pRef, kExtraPrimitive) || (baseType == kStaticType && IsEffectModel(pBase)))
            continue;

        // The size of its bounds (its base's object bounds: min x, y, z, max x, y, z at 0x20)
        const auto* pBounds = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(pBase) + 0x20);
        float extent = 0.f;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float size = static_cast<float>(std::max(std::abs(pBounds[axis]), std::abs(pBounds[3 + axis])));
            extent += size * size;
        }
        extent = std::sqrt(extent);
        if (extent <= 0.f || extent * pRef->GetScale() > kMaxUncollidableExtent)
            continue;

        // Shown (NiAVObject flags at 0x108, bit 0: hidden), without collision anywhere in its 3D (the game's pick sees
        // the others)
        const auto* pNode = reinterpret_cast<const uint8_t*>(pRef->GetNiNode());
        if (!pNode || (*reinterpret_cast<const uint64_t*>(pNode + 0x108) & 1))
            continue;
        auto cached = m_collisionCache.find(pRef->formID);
        if (cached == m_collisionCache.end() || cached->second.first != pNode)
        {
            m_collisionCache[pRef->formID] = {pNode, HasCollision(pNode)};
            cached = m_collisionCache.find(pRef->formID);
        }
        if (!cached->second.second)
            aScan.Candidates.emplace_back(pRef->formID, extent);
    }
}
void BuildToolsService::GiveSalvage(const Vector<std::pair<uint32_t, uint32_t>>& acSalvage, const std::string& acName) noexcept
{
    auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(m_workshop.GetBuildWorkshopId()));
    if (!pWorkshop)
        return;

    std::string given;
    for (const auto& [itemId, count] : acSalvage)
    {
        auto* pItem = Cast<TESBoundObject>(TESForm::GetById(itemId));
        if (!pItem)
            continue;

        TESObjectREFR::ExtraDataListHandle handle{nullptr};
        pWorkshop->AddObjectToContainer(pItem, &handle, static_cast<int32_t>(count), nullptr, 0);
        const char* pItemName = pItem->GetName();
        given += fmt::format("{}{} {}", given.empty() ? "" : ", ", count, pItemName && *pItemName ? pItemName : "scrap");
    }

    ShowMessage(fmt::format("{}: {}", acName.empty() ? std::string("Scrapped") : "Scrapped " + acName, given));
    spdlog::info("Build tools: scrap anything gave {} for {}", given, acName);
}

void BuildToolsService::BeforeScrap(TESObjectREFR* apRef) noexcept
{
    if (apRef->formType != FormType::Character || !m_workshop.IsInBuildMode())
        return;

    auto* pActor = Cast<Actor>(apRef);
    if (!pActor)
        return;

    // A turret (the game scraps those itself): its character goes away for everyone
    if (!pActor->IsDead())
    {
        m_workshop.NoteScrappedCharacter(apRef);
        return;
    }

    // Its loot isn't lost: it goes to the settlement's workbench. Then its character goes away for everyone.
    if (auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(m_workshop.GetBuildWorkshopId())))
        apRef->MoveAllItemsTo(pWorkshop);
    m_workshop.NoteScrappedCharacter(apRef);

    ShowMessage("Body scrapped, its items are in the workbench");
    spdlog::info("Build tools: scrapping the body {:X}, its items went to workshop {:X}", apRef->formID, m_workshop.GetBuildWorkshopId());
}

bool BuildToolsService::KeysActive() const noexcept
{
    if (!m_workshop.IsInBuildMode() || m_world.ctx().at<OverlayService>().GetActive())
        return false;

    // Only while the game has the keyboard
    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    return processId == GetCurrentProcessId();
}

void BuildToolsService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfProbe::Section _perf("BuildTools");
    // The MCM page, now and then; the snapping switches kept in the game while building (its INI is read after we
    // start)
    if (const auto now = Clock::now(); now - m_mcmReadAt > std::chrono::seconds(2))
    {
        m_mcmReadAt = now;
        ReadMcmSettings(true);
    }
    if (m_workshop.IsInBuildMode())
        ApplySnapping();

    // The edit flag that stops the scrap button (see HookScrapButton), logged as it changes
    {
        POINTER_FALLOUT4(uint8_t, s_editFlag, 816518);
        static uint8_t s_lastEditFlag = 0xFF;
        if (const uint8_t flag = *s_editFlag.Get(); flag != s_lastEditFlag)
        {
            s_lastEditFlag = flag;
            const TESObjectREFR* pOutlined = GetOutlinedObject();
            spdlog::info("Build tools: edit flag {} (outlined {:X}, build mode {})", flag, pOutlined ? pOutlined->formID : 0, m_workshop.IsInBuildMode());
        }
    }

    // In case the main loop hasn't (in the world, the game's data is loaded)
    UncombineSettlementCells(true);

    // The overlay learns the keys once the game runs (and again whenever they change)
    if (!m_sentState)
    {
        m_sentState = true;
        SendState();
    }

    const bool active = KeysActive();
    const auto now = Clock::now();

    for (size_t i = 0; i < m_keys.size(); ++i)
    {
        KeyState& key = m_keys[i];
        const bool down = active && key.VirtualKey && (GetAsyncKeyState(key.VirtualKey) & 0x8000) != 0;
        if (!down)
        {
            key.Down = false;
            continue;
        }

        if (!key.Down)
        {
            key.Down = true;
            key.Next = now + kRepeatDelay;
            Run(s_actions[i].What);
        }
        else if (s_actions[i].Repeats && now >= key.Next)
        {
            key.Next = now + kRepeatInterval;
            Run(s_actions[i].What);
        }
    }
}

void BuildToolsService::Run(Action aAction) noexcept
{
    const float rotation = kRotationSteps[m_rotationStep] * kDegrees;
    const float move = kMoveSteps[m_moveStep];
    const float scale = static_cast<float>(kScaleSteps[m_scaleStep]) / 100.f;

    // Moving relative to the player: its facing (Z angle, forward is +Y at 0) gives "away" and "right"
    auto offset = [this](float aRight, float aAway, float aUp)
    {
        return [this, aRight, aAway, aUp](Transform& aTransform)
        {
            if (m_worldAxes)
            {
                aTransform.Position.x += aRight;
                aTransform.Position.y += aAway;
            }
            else if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
            {
                const float heading = pPlayer->rotation.z;
                aTransform.Position.x += std::sin(heading) * aAway + std::cos(heading) * aRight;
                aTransform.Position.y += std::cos(heading) * aAway - std::sin(heading) * aRight;
            }
            aTransform.Position.z += aUp;
        };
    };
    auto rotate = [this](float aAngle)
    {
        return [this, aAngle](Transform& aTransform)
        {
            float& angle = m_rotationAxis == 0 ? aTransform.Rotation.x : m_rotationAxis == 1 ? aTransform.Rotation.y : aTransform.Rotation.z;
            angle = std::remainder(angle + aAngle, 2.f * kPi);
        };
    };
    auto paste = [this](const std::function<void(Transform&, const Transform&)>& acCopy)
    {
        if (!m_remembered)
        {
            ShowMessage("Nothing remembered yet");
            return;
        }
        const Transform remembered = *m_remembered;
        ChangeOutlined([&](Transform& aTransform) { acCopy(aTransform, remembered); });
    };

    switch (aAction)
    {
    case Action::TogglePlaceAnywhere:
        m_placeAnywhere = !m_placeAnywhere;
        ShowMessage(m_placeAnywhere ? "Place anywhere on" : "Place anywhere off");
        SaveConfig();
        SendState();
        break;
    case Action::ToggleScrapAnything:
        m_scrapAnything = !m_scrapAnything;
        ShowMessage(m_scrapAnything ? "Scrap anything on" : "Scrap anything off");
        SaveConfig();
        SendState();
        break;
    case Action::ToggleObjectSnap:
        m_objectSnap = !m_objectSnap;
        ApplySnapping();
        ShowMessage(m_objectSnap ? "Snapping to objects on" : "Snapping to objects off");
        SaveConfig();
        SendState();
        break;
    case Action::ToggleGridSnap:
        m_gridSnap = !m_gridSnap;
        ApplySnapping();
        ShowMessage(m_gridSnap ? "Grid snapping on (32 units, 45 degrees)" : "Grid snapping off");
        SaveConfig();
        SendState();
        break;
    case Action::ChangeRotationAxis:
        m_rotationAxis = (m_rotationAxis + 1) % 3;
        ShowMessage(fmt::format("Rotation axis {}", AxisName(m_rotationAxis)));
        break;
    case Action::RotateLeft: ChangeOutlined(rotate(-rotation)); break;
    case Action::RotateRight: ChangeOutlined(rotate(rotation)); break;
    case Action::RotateStepDown:
    case Action::RotateStepUp:
        m_rotationStep = aAction == Action::RotateStepUp ? std::min(m_rotationStep + 1, kRotationSteps.size() - 1) : (m_rotationStep ? m_rotationStep - 1 : 0);
        ShowMessage(fmt::format("Rotation step {:g} degrees", kRotationSteps[m_rotationStep]));
        break;
    case Action::Level:
        ChangeOutlined(
            [](Transform& aTransform)
            {
                aTransform.Rotation.x = 0.f;
                aTransform.Rotation.y = 0.f;
            });
        break;
    case Action::MoveUp: ChangeOutlined(offset(0.f, 0.f, move)); break;
    case Action::MoveDown: ChangeOutlined(offset(0.f, 0.f, -move)); break;
    case Action::MoveLeft: ChangeOutlined(offset(-move, 0.f, 0.f)); break;
    case Action::MoveRight: ChangeOutlined(offset(move, 0.f, 0.f)); break;
    case Action::MoveCloser: ChangeOutlined(offset(0.f, -move, 0.f)); break;
    case Action::MoveAway: ChangeOutlined(offset(0.f, move, 0.f)); break;
    case Action::MoveStepDown:
    case Action::MoveStepUp:
        m_moveStep = aAction == Action::MoveStepUp ? std::min(m_moveStep + 1, kMoveSteps.size() - 1) : (m_moveStep ? m_moveStep - 1 : 0);
        ShowMessage(fmt::format("Move step {:g}", kMoveSteps[m_moveStep]));
        break;
    case Action::ToggleWorldAxes:
        m_worldAxes = !m_worldAxes;
        ShowMessage(m_worldAxes ? "Moving along the world's axes" : "Moving relative to you");
        break;
    case Action::ScaleDown:
    case Action::ScaleUp:
    {
        const float change = aAction == Action::ScaleUp ? scale : -scale;
        std::optional<float> newScale;
        ChangeOutlined(
            [&](Transform& aTransform)
            {
                aTransform.Scale = std::clamp(aTransform.Scale + change, kMinScale, kMaxScale);
                newScale = aTransform.Scale;
            });
        if (newScale)
            ShowMessage(fmt::format("Scale {:.0f}%", *newScale * 100.f));
        break;
    }
    case Action::ScaleStepDown:
    case Action::ScaleStepUp:
        m_scaleStep = aAction == Action::ScaleStepUp ? std::min(m_scaleStep + 1, kScaleSteps.size() - 1) : (m_scaleStep ? m_scaleStep - 1 : 0);
        ShowMessage(fmt::format("Scale step {}%", kScaleSteps[m_scaleStep]));
        break;
    case Action::ScaleReset: ChangeOutlined([](Transform& aTransform) { aTransform.Scale = 1.f; }); break;
    case Action::Redraw:
        if (TESObjectREFR* pRef = GetOutlinedObject())
        {
            // Rebuilds the object's 3D where it is now, without the jitter moving it can leave
            pRef->Disable();
            pRef->Enable();
        }
        break;
    case Action::Undo: ShowMessage(m_workshop.UndoLastChange()); break;
    case Action::Remember:
        if (TESObjectREFR* pRef = GetOutlinedObject())
        {
            m_remembered = Transform{pRef->position, pRef->rotation, pRef->GetScale()};
            ShowMessage("Object position, angles and scale remembered");
        }
        break;
    case Action::PastePositionX: paste([](Transform& aTo, const Transform& acFrom) { aTo.Position.x = acFrom.Position.x; }); break;
    case Action::PastePositionY: paste([](Transform& aTo, const Transform& acFrom) { aTo.Position.y = acFrom.Position.y; }); break;
    case Action::PastePositionZ: paste([](Transform& aTo, const Transform& acFrom) { aTo.Position.z = acFrom.Position.z; }); break;
    case Action::PasteAngles: paste([](Transform& aTo, const Transform& acFrom) { aTo.Rotation = acFrom.Rotation; }); break;
    case Action::PasteScale: paste([](Transform& aTo, const Transform& acFrom) { aTo.Scale = acFrom.Scale; }); break;
    case Action::PasteAll: paste([](Transform& aTo, const Transform& acFrom) { aTo = acFrom; }); break;
    default: break;
    }
}

void BuildToolsService::ChangeOutlined(const std::function<void(Transform&)>& acChange) noexcept
{
    TESObjectREFR* pRef = GetOutlinedObject();
    if (!pRef)
        return;

    // Settlement claims: rotating, moving, scaling and pasting need the right to build here
    if (!WorkshopService::MayBuildHere())
    {
        m_workshop.ShowRefusal(false);
        return;
    }

    const Transform before{pRef->position, pRef->rotation, pRef->GetScale()};
    Transform after = before;
    acChange(after);

    pRef->SetTransform(after.Position, after.Rotation, after.Scale);

    // Objects that belong to the settlement are shared and can be undone; a new object still being placed is shared
    // (and undone) once it's placed
    if (IsWorkshopItem(pRef))
    {
        m_workshop.RecordToolChange(pRef, {before.Position, before.Rotation, before.Scale});
        m_workshop.ShareMove(pRef);
    }
}

void BuildToolsService::ResetKeys() noexcept
{
    for (size_t i = 0; i < m_keys.size(); ++i)
        m_keys[i] = KeyState{s_actions[i].DefaultKey};
}

void BuildToolsService::LoadConfig() noexcept
{
    std::ifstream file(ConfigPath());
    if (!file)
    {
        SaveConfig();
        return;
    }

    std::string section;
    std::string line;
    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string name = Trim(std::string_view(line).substr(0, equals));
        const std::string value = Trim(std::string_view(line).substr(equals + 1));

        if (section == "main")
        {
            const bool on = value == "on" || value == "true" || value == "1";
            if (name == "placeAnywhere")
                m_placeAnywhere = on;
            else if (name == "scrapAnything")
                m_scrapAnything = on;
            else if (name == "objectSnap")
                m_objectSnap = on;
            else if (name == "gridSnap")
                m_gridSnap = on;
        }
        else if (section == "keys")
        {
            const auto action = std::find_if(s_actions.begin(), s_actions.end(), [&](const ActionInfo& acInfo) { return name == acInfo.Name; });
            if (action == s_actions.end())
                continue;

            const int key = static_cast<int>(std::strtol(value.c_str(), nullptr, 0));
            const auto index = static_cast<size_t>(action - s_actions.begin());
            if (IsReservedKey(key))
            {
                spdlog::warn("Build tools: {} can't use key {:#x} (the game or the overlay uses it), keeping {:#x}", name, key, m_keys[index].VirtualKey);
                continue;
            }
            m_keys[index].VirtualKey = key;
        }
    }

    spdlog::info("Build tools: place anywhere {}, scrap anything {}, snapping to objects {}, grid snapping {}, keys from {}", m_placeAnywhere ? "on" : "off",
                 m_scrapAnything ? "on" : "off", m_objectSnap ? "on" : "off", m_gridSnap ? "on" : "off", ConfigPath().string());
}

void BuildToolsService::SaveConfig() const noexcept
{
    std::error_code error;
    std::filesystem::create_directories(ConfigPath().parent_path(), error);

    std::ofstream file(ConfigPath(), std::ios::trunc);
    if (!file)
    {
        spdlog::warn("Build tools: couldn't write {}", ConfigPath().string());
        return;
    }

    file << "; Reclamation Day build tools. Change them in the overlay (Settings, Build tools) or here.\n"
            "; Keys are Windows virtual key codes: https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes\n"
            "; Keys the game or the overlay use (letters, number row, arrows, Tab, Space, Shift, Ctrl, Alt, F2, F3, F5 to F10,\n"
            "; F12, console) are refused.\n\n";
    file << "[main]\n";
    file << "placeAnywhere = " << (m_placeAnywhere ? "on" : "off") << "\n";
    file << "scrapAnything = " << (m_scrapAnything ? "on" : "off") << "\n";
    file << "objectSnap = " << (m_objectSnap ? "on" : "off") << "\n";
    file << "gridSnap = " << (m_gridSnap ? "on" : "off") << "\n\n";
    file << "[keys]\n";
    for (size_t i = 0; i < s_actions.size(); ++i)
        file << s_actions[i].Name << " = " << fmt::format("{:#04x}", m_keys[i].VirtualKey) << "\n";
}

// Snapping, read from the 1.10.163 exe (2026-10-27). The workshop's per-frame placement update (CommonLibF4 id 679561)
// skips its snap points block (finding the snap points of the objects near the one held and snapping to them) when its
// jne at +0xD2E is taken (0F 85 0C 17 00 00, to +0x2440: the grid snapping check), as it is while an edit goes on
// (the flag 816517): made an unconditional jmp (90 E9) while snapping to objects is off. The grid snapping there (32
// units, 45 degrees: fSnapHorizontalUnits, fSnapVerticalUnits, fSnapRotationDegrees) follows the INI setting
// bWorkshopEnableGridBasedSnapping:Workshop (the Setting 1292777, its bool at +8), off in the game's own INI.
void BuildToolsService::ApplySnapping() const noexcept
{
    POINTER_FALLOUT4(uint8_t, s_placementUpdate, 679562);
    POINTER_FALLOUT4(uint8_t, s_gridSnapSetting, 1292778);

    uint8_t* pBranch = s_placementUpdate.Get() + 0xD2E;
    const uint16_t branch = *reinterpret_cast<const uint16_t*>(pBranch);
    if (branch == 0x850F || branch == 0xE990)
    {
        if (const uint16_t wanted = m_objectSnap ? 0x850F : 0xE990; branch != wanted)
            TiltedPhoques::Put<uint16_t>(pBranch, wanted);
    }
    else
    {
        static bool s_warned = false;
        if (!std::exchange(s_warned, true))
            spdlog::warn("Build tools: the game's placement code isn't as expected ({:#06x}), snapping to objects stays as the game has it", branch);
    }

    *(s_gridSnapSetting.Get() + 8) = m_gridSnap ? 1 : 0;
}

// The switches on our MCM page (Data/MCM/Config/ReclamationDay/config.json, Build Tools), as MCM writes them to
// Data\MCM\Settings\ReclamationDay.ini (settings like these belong on the MCM page). One
// changed there since last looked at is taken; one MCM hasn't written yet leaves config\BuildTools.ini's. The keys don't
// write MCM's file (MCM keeps its own copy and writes the whole file back): a key's switch holds until the page
// changes it or the game starts again.
void BuildToolsService::ReadMcmSettings(bool aAnnounce) noexcept
{
    const auto path = (std::filesystem::current_path() / L"Data" / L"MCM" / L"Settings" / L"ReclamationDay.ini").wstring();
    const std::array<std::pair<const wchar_t*, bool*>, 4> switches{
        {{L"bPlaceAnywhere", &m_placeAnywhere}, {L"bScrapAnything", &m_scrapAnything}, {L"bObjectSnap", &m_objectSnap}, {L"bGridSnap", &m_gridSnap}}};

    bool changed = false;
    for (size_t i = 0; i < switches.size(); ++i)
    {
        wchar_t text[16]{};
        GetPrivateProfileStringW(L"BuildTools", switches[i].first, L"", text, static_cast<DWORD>(std::size(text)), path.c_str());
        const int value = text[0] ? (std::wcstol(text, nullptr, 10) != 0 ? 1 : 0) : -1;
        if (value == m_mcmSeen[i])
            continue;
        m_mcmSeen[i] = value;
        if (value >= 0 && *switches[i].second != (value == 1))
        {
            *switches[i].second = value == 1;
            changed = true;
        }
    }
    if (!changed)
        return;

    spdlog::info("Build tools (MCM): place anywhere {}, scrap anything {}, snapping to objects {}, grid snapping {}", m_placeAnywhere ? "on" : "off", m_scrapAnything ? "on" : "off",
                 m_objectSnap ? "on" : "off", m_gridSnap ? "on" : "off");
    if (aAnnounce)
    {
        SaveConfig();
        SendState();
    }
}

void BuildToolsService::SendState(const std::string& acError) const noexcept
{
    // The snapping switches stay out of the overlay (2026-10-27): their keys and settings are in
    // config\BuildTools.ini only
    std::string keys;
    for (size_t i = 0; i < s_actions.size(); ++i)
    {
        if (s_actions[i].What == Action::ToggleObjectSnap || s_actions[i].What == Action::ToggleGridSnap)
            continue;
        keys += fmt::format(R"({}{{"action":{},"key":{},"defaultKey":{}}})", keys.empty() ? "" : ",", MiniJson::Quote(s_actions[i].Name), m_keys[i].VirtualKey, s_actions[i].DefaultKey);
    }

    const std::string json = fmt::format(R"({{"placeAnywhere":{},"scrapAnything":{},"keys":[{}],"error":{}}})", m_placeAnywhere ? "true" : "false", m_scrapAnything ? "true" : "false", keys,
                                         MiniJson::Quote(acError));
    m_world.ctx().at<OverlayService>().SendEvent("buildToolsState", json);
}

std::string BuildToolsService::CheckKey(Action aAction, int aVirtualKey) const noexcept
{
    if (IsReservedKey(aVirtualKey))
        return "reserved";

    for (size_t i = 0; i < s_actions.size(); ++i)
    {
        if (s_actions[i].What != aAction && m_keys[i].VirtualKey == aVirtualKey)
            return fmt::format("used:{}", s_actions[i].Name);
    }
    return {};
}

void BuildToolsService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    const auto colon = acPayload.find(':');
    const std::string name = acPayload.substr(0, colon);
    const std::string value = colon == std::string::npos ? std::string{} : acPayload.substr(colon + 1);

    if (acRequest == "bind")
    {
        const auto action = std::find_if(s_actions.begin(), s_actions.end(), [&](const ActionInfo& acInfo) { return name == acInfo.Name; });
        if (action == s_actions.end())
            return SendState("unknown");

        const int key = static_cast<int>(std::strtol(value.c_str(), nullptr, 0));
        if (std::string error = CheckKey(action->What, key); !error.empty())
            return SendState(error);

        m_keys[static_cast<size_t>(action - s_actions.begin())] = KeyState{key};
        spdlog::info("Build tools: {} on key {:#x}", name, key);
    }
    else if (acRequest == "set")
    {
        if (name == "placeAnywhere")
            m_placeAnywhere = value == "1";
        else if (name == "scrapAnything")
            m_scrapAnything = value == "1";
    }
    else if (acRequest == "reset")
    {
        ResetKeys();
        spdlog::info("Build tools: keys back to the defaults");
    }
    else if (acRequest != "get")
        return;

    if (acRequest != "get")
        SaveConfig();
    SendState();
}

#endif

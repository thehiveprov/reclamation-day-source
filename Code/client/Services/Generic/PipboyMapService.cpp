#include <Services/PipboyMapService.h>

#if TP_FALLOUT4

#include <Events/UpdateEvent.h>
#include <Games/Fallout4/BikeLink.h>
#include <World.h>
#include <Components.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <ExtraData/ExtraDataList.h>
#include <Games/Fallout4/PipboyGlancePose.h>
#include <Games/Fallout4/PovSwitch.h>
#include <Interface/UI.h>
#include <Interface/IMenu.h>
#include <Interface/Scaleform.h>
#include <SaveFolder.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/ParkourService.h>
#include <Services/DualWieldService.h>

#include <atomic>
#include <intrin.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <PerfProbe.h>

namespace
{
PipboyMapService* s_pService = nullptr;

// Our movie, under Interface (the game loads "Interface/<name>.swf")
constexpr const char* kMovieName = "ReclamationDay/PipboyMap";

// GenericMenu (CommonLibF4 ids, +1 here): its constructor (id 300484) loads the movie named by a static string (id
// 1087412, which the game sets itself before each of its own uses) and takes its flags from a static (id 302153; -1, the
// default, gives 0x9301: pauses the game, needs updates, topmost rendered, renders off screen, custom rendering), which
// it puts back to -1. Ours doesn't pause.
constexpr uint32_t kGenericMenuFlags = 0x9300;
// GameMenuBase::shaderFXObjects (a BSTArray of BSGFxShaderFXTarget*): the parts of a custom rendered menu the game
// draws (nothing else of the movie is seen). The menu's destructor frees the array, not what's in it.
constexpr uintptr_t kShaderTargetsOffset = 0x70;

// A press shorter than this keeps the map up until the next one; a longer one shows it while held
constexpr float kHoldTime = 0.35f;
// The arm's blend out (PipboyGlancePose) and a little: the screen stays lit while it goes down
constexpr float kLowerTime = 0.45f;
// How long after the screen's renderer is turned on its menu is opened, and after a switch from third person the
// renderer is
constexpr float kMenuDelay = 0.15f;
constexpr float kViewSwitchDelay = 0.25f;
// How long the drawing has to come off the screen before the menu closes anyway
constexpr float kCloseTimeout = 0.5f;
// How often the marks that don't move (locations, quest targets, exits) are gathered again
constexpr float kMarksRefresh = 0.25f;

// One scale that shows what's around and where the player is going: world units from the middle of the screen to its
// nearer edge. Outside about three cells each way (a few blocks of Boston), inside about a room or two each way.
constexpr float kWorldRadius = 11000.f;
constexpr float kLocalRadius = 2000.f;
// Locations a little off the screen still come in as it turns
constexpr float kGatherRadiusScale = 1.6f;

// The Pip-Boy screen's visible part of our 1024 x 768 stage (GetVisibleFrameRect) is kept clear of its edges by this
// (stage units), where the glass curves away
constexpr float kScreenMargin = 24.f;
// The marks' size (1: drawn on a 1024 x 768 stage as is), and how much brighter the map image is drawn (the Pip-Boy's
// screen dims it a lot)
constexpr double kMarkScale = 1.6;
constexpr double kImageBrightness = 2.;
// The local map is rendered again once the player is this far from where it last was (not more often than this)
constexpr float kLocalMapRefreshDistance = 800.f;
constexpr float kLocalMapRefreshTime = 1.f;

// PlayerCharacter (CommonLibF4, 1.10.163): currentMapMarkers (a BSTArray<ObjectRefHandle>, its size at +0x10) at 0x738,
// the quest target lock (a BSSpinLock) at 0x630, questTargets (a BSTHashMap<TESQuest*, QuestTargetArray*>) at 0x7F0,
// playerMapMarker (the custom marker's handle) at 0xBC4, the current location (a BGSLocation) at 0xCC8
constexpr uintptr_t kMapMarkersOffset = 0x738;
constexpr uintptr_t kQuestTargetLockOffset = 0x630;
constexpr uintptr_t kQuestTargetsOffset = 0x7F0;
constexpr uintptr_t kCustomMarkerOffset = 0xBC4;

// PlayerCamera::cameraStates (CommonLibF4 CameraStates) at 0xE0
constexpr uint32_t kFirstPersonState = 0;
constexpr uint32_t kThirdPersonState = 8;

// PipboyManager's calls without arguments (CommonLibF4 ids, +1 here)
constexpr uint32_t kEnablePipboyShader = 157922;
constexpr uint32_t kDisablePipboyShader = 662520;

// Menus that lower the arm (the menu mode count covers most others)
constexpr const char* kBlockingMenus[] = {
    "PipboyMenu",       "DialogueMenu",    "VATSMenu",      "WorkshopMenu",   "LooksMenu",          "ContainerMenu",
    "BarterMenu",       "ExamineMenu",     "TerminalMenu",  "LockpickingMenu", "BookMenu",          "PauseMenu",
    "Console",          "FavoritesMenu",   "LoadingMenu",   "MainMenu",        "SleepWaitMenu",     "MessageBoxMenu",
    "CookingMenu",      "PowerArmorModMenu", "LevelUpMenu", "SPECIALMenu",     "PipboyHolotapeMenu", "CreditsMenu",
};

const BSFixedString& FixedString(const char* acpText) noexcept
{
    static std::unordered_map<std::string, BSFixedString> s_strings;
    auto it = s_strings.find(acpText);
    if (it == s_strings.end())
        it = s_strings.emplace(acpText, BSFixedString(acpText)).first;
    return it->second;
}

uint8_t* PlayerCamera() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    return *s_playerCamera.Get();
}

bool IsFirstPerson() noexcept
{
    const uint8_t* pCamera = PlayerCamera();
    if (!pCamera)
        return false;
    const uint8_t* pState = *reinterpret_cast<const uint8_t* const*>(pCamera + 0x28);
    return pState && *reinterpret_cast<const uint32_t*>(pState + 0x20) == kFirstPersonState;
}

// PlayerCamera::SetState with one of its states
void SetCameraState(uint32_t aState) noexcept
{
    uint8_t* pCamera = PlayerCamera();
    if (!pCamera)
        return;
    using TSetState = void(uint8_t*, uint8_t*);
    POINTER_FALLOUT4(TSetState, s_setState, 858848);
    if (uint8_t* pState = *reinterpret_cast<uint8_t**>(pCamera + 0xE0 + aState * sizeof(void*)))
        s_setState.Get()(pCamera, pState);
}

// UIMessageQueue::AddMessage (CommonLibF4 ids 82123, 1182019): 1 shows a menu, 3 hides it
void SendMenuMessage(const char* acpMenu, int32_t aType) noexcept
{
    POINTER_FALLOUT4(void*, s_queue, 82124);
    using TAddMessage = void(void*, const BSFixedString&, int32_t);
    POINTER_FALLOUT4(TAddMessage, s_addMessage, 1182020);
    if (void* pQueue = *reinterpret_cast<void**>(s_queue.Get()))
        s_addMessage.Get()(pQueue, FixedString(acpMenu), aType);
}

bool IsMenuOpen(const char* acpMenu) noexcept
{
    UI* pUI = UI::Get();
    return pUI && pUI->GetMenuOpen(FixedString(acpMenu));
}

// UI::menuMode (CommonLibF4, 0x1E0): how many open menus take the game's controls
uint32_t MenuModeCount() noexcept
{
    UI* pUI = UI::Get();
    return pUI ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pUI) + 0x1E0) : 0;
}

// The GenericMenu shows our movie next time it's made, without pausing the game
void PrepareGenericMenu() noexcept
{
    POINTER_FALLOUT4(BSFixedString, s_movieName, 1087413);
    POINTER_FALLOUT4(int32_t, s_flags, 302154);
    s_movieName.Get()->Set(kMovieName);
    *s_flags.Get() = kGenericMenuFlags;
}

void PipboyManagerCall(uint32_t aId) noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_pipboyManager, 553235);
    uint8_t* pManager = *s_pipboyManager.Get();
    if (!pManager)
        return;
    using TCall = void(uint8_t*);
    const VersionDbPtr<TCall> function(aId);
    function.Get()(pManager);
}

// The Pip-Boy's background blur, the image space modifier PipboyMenuImod: the game's Pip-Boy turns it on as the arm is
// up (InitPipboy) and off only in its closing animation (OnPipboyCloseAnim), so ours stops it itself - no blur while
// glancing, none left behind (ImageSpaceModifierInstanceForm::Stop by editor ID, CommonLibF4 id 549773)
void StopPipboyBlur() noexcept
{
    using TStop = void(const BSFixedString&);
    POINTER_FALLOUT4(TStop, s_stop, 549774);
    s_stop.Get()(FixedString("PipboyMenuImod"));
}

// ---------------------------------------------------------------------------------------------------------------
// What's on the map

// The world map the Pip-Boy shows here, as the game works it out for its map page (PipboyMapData, CommonLibF4 ids):
// the worldspace (392016: the player's, up to its top parent), its map data (378446: usable width and height, then the
// north west and south east cells, int16, at 0x08) and its map texture (724327, under Textures)
struct WorldMap
{
    void* pWorld{nullptr};
    const char* pTexture{nullptr};
    int16_t nwX{0}, nwY{0}, seX{0}, seY{0};
};

WorldMap GetWorldMap() noexcept
{
    using TGetWorld = void*();
    using TGetMapData = const uint8_t*(void*);
    using TGetTexture = const char*(void*);
    POINTER_FALLOUT4(TGetWorld, s_getWorld, 392017);
    POINTER_FALLOUT4(TGetMapData, s_getMapData, 378447);
    POINTER_FALLOUT4(TGetTexture, s_getTexture, 724328);
    WorldMap map;
    map.pWorld = s_getWorld.Get()();
    if (!map.pWorld)
        return map;
    map.pTexture = s_getTexture.Get()(map.pWorld);
    if (const uint8_t* pData = s_getMapData.Get()(map.pWorld))
    {
        map.nwX = *reinterpret_cast<const int16_t*>(pData + 8);
        map.nwY = *reinterpret_cast<const int16_t*>(pData + 10);
        map.seX = *reinterpret_cast<const int16_t*>(pData + 12);
        map.seY = *reinterpret_cast<const int16_t*>(pData + 14);
    }
    return map;
}

// The world map texture's border the corners leave out, as fractions of its width and height (left, top, right,
// bottom): the corner cells land that far in from the texture's edges. The Pip-Boy's map page sets them per texture
// (Pipboy_MapPage.swf, WorldMapHolder.onPipboyChangeEvent; used by MapHolder.normalizedToMapSpace). Interiors' local
// maps have none.
void GetWorldMapGutters(const char* apTexture, float (&aGutters)[4]) noexcept
{
    const auto is = [apTexture](const char* acpName) { return apTexture && _stricmp(apTexture, acpName) == 0; };
    const auto set = [&aGutters](float aLeft, float aTop, float aRight, float aBottom)
    {
        aGutters[0] = aLeft;
        aGutters[1] = aTop;
        aGutters[2] = aRight;
        aGutters[3] = aBottom;
    };
    if (is("DLC03\\Interface\\Map\\MapFarHarbor.dds"))
        set(0.00920067f, 0.02515624f, 0.03374173f, 0.00958931f);
    else if (is("DLC04\\Interface\\Map\\NukaWorldMap.DDS"))
        set(-0.045f, 0.0005f, -0.095f, -0.155f);
    else
        set(0.02685546875f, 0.02685546875f, 0.0283203125f, 0.0283203125f);
}

// The top worldspace of one (TESWorldSpace's parent, CommonLibF4 id 644684): Diamond City and the like share the
// Commonwealth's map and places
TESWorldSpace* TopWorld(TESWorldSpace* apWorld) noexcept
{
    using TGetParent = TESWorldSpace*(TESWorldSpace*);
    POINTER_FALLOUT4(TGetParent, s_getParent, 644685);
    for (int i = 0; apWorld && i < 8; ++i)
    {
        TESWorldSpace* pParent = s_getParent.Get()(apWorld);
        if (!pParent)
            break;
        apWorld = pParent;
    }
    return apWorld;
}

bool IsInterior(const TESObjectCELL* apCell) noexcept
{
    return apCell && (apCell->cellFlags[0] & 1);
}

// Where the player is: their cell inside, their top worldspace outside
struct Space
{
    TESObjectCELL* pCell{nullptr};
    TESWorldSpace* pWorld{nullptr};
    bool interior{false};

    [[nodiscard]] bool Has(TESObjectREFR* apRef) const noexcept
    {
        TESObjectCELL* pCell = apRef ? apRef->GetParentCellEx() : nullptr;
        if (!pCell)
            return false;
        if (interior)
            return pCell == this->pCell;
        return !IsInterior(pCell) && TopWorld(pCell->worldspace) == pWorld;
    }
};

// BSScaleformExternalTexture (CommonLibF4): a game texture Scaleform movies load as img://<the texture's name>
struct ExternalTexture
{
    void* pTexture;
    uint32_t renderTarget;
    BSFixedString::Data* pPath;
};
static_assert(sizeof(ExternalTexture) == 0x18);

// NiObject's reference count at 0x08; its DeleteThis is slot 1
void ReleaseNiObject(void* apObject) noexcept
{
    auto* pCount = reinterpret_cast<volatile long*>(static_cast<uint8_t*>(apObject) + 0x08);
    if (_InterlockedDecrement(pCount) == 0)
    {
        using TDeleteThis = void(void*);
        (*reinterpret_cast<TDeleteThis* const* const*>(apObject))[1](apObject);
    }
}

// The world map's texture as one movies can load, as the Pip-Boy's map page does it (CommonLibF4 id 1106300): loaded
// (LoadTextureByPath, id 1375091), named and handed to an external texture (SetTexture, id 119731), which keeps it.
// Kept for the game's run; another worldspace's map gets a new name, so a movie loads it again.
ExternalTexture s_worldTexture{};
std::string s_worldTexturePath;
std::string s_worldTextureName;

const std::string& WorldTextureName(const std::string& acPath) noexcept
{
    if (acPath == s_worldTexturePath)
        return s_worldTextureName;
    s_worldTexturePath = acPath;
    s_worldTextureName.clear();

    using TLoadTexture = void(const char*, bool, void**, bool, bool, bool);
    POINTER_FALLOUT4(TLoadTexture, s_loadTexture, 1375092);
    void* pTexture = nullptr;
    s_loadTexture.Get()(acPath.c_str(), true, &pTexture, false, false, false);
    if (!pTexture)
    {
        spdlog::warn("Realtime Pip-Boy Map: no texture {}", acPath);
        return s_worldTextureName;
    }

    static int s_count = 0;
    s_worldTextureName = fmt::format("ReclamationDayPipboyMap{}", ++s_count);
    // NiObjectNET::name at 0x10
    reinterpret_cast<BSFixedString*>(static_cast<uint8_t*>(pTexture) + 0x10)->Set(s_worldTextureName.c_str());
    using TSetTexture = void(ExternalTexture*, void*);
    POINTER_FALLOUT4(TSetTexture, s_setTexture, 119732);
    s_setTexture.Get()(&s_worldTexture, pTexture);
    ReleaseNiObject(pTexture);
    spdlog::info("Realtime Pip-Boy Map: world map {} as img://{}", acPath, s_worldTextureName);
    return s_worldTextureName;
}

// The game's local map (CommonLibF4 ids): the object that renders it (id 717197), made while the Pip-Boy's map page is
// (PipboyMapMenu's constructor, id 227317: id 1549810; its destructor, id 838724: id 312943). Each frame the game
// renders it and gives its texture to the external texture "LocalMap" (id 1536138; that one at id 1155495).
void* LocalMapObject() noexcept
{
    POINTER_FALLOUT4(void*, s_localMap, 717198);
    return *s_localMap.Get();
}

void MakeLocalMap() noexcept
{
    using TMake = void();
    POINTER_FALLOUT4(TMake, s_make, 1549811);
    s_make.Get()();
}

void FreeLocalMap() noexcept
{
    using TFree = void();
    POINTER_FALLOUT4(TFree, s_free, 312944);
    s_free.Get()();
}

// The game renders the local map in its frame (id 408683) once a countdown of frames on Main (CommonLibF4 id 756304;
// the count at 0x20) runs out, and only while the Pip-Boy menu is open (UI's check by name, id 1065114, called at
// +0x188 of the frame): the map page starts the countdown (id 1113548, with 4 frames, id 1173498) when the local map's
// texture isn't there yet. Ours starts it too, and the hook on the check answers yes for that one call.
std::atomic<bool> s_localMapRenderWanted{false};

void RequestLocalMapRender() noexcept
{
    s_localMapRenderWanted = true;
    POINTER_FALLOUT4(uint8_t*, s_main, 756305);
    POINTER_FALLOUT4(int32_t, s_frames, 1173499);
    if (uint8_t* pMain = *s_main.Get())
        *reinterpret_cast<volatile int32_t*>(pMain + 0x20) = std::max(*s_frames.Get(), 1);
}

// When it was last asked for, and where the player was
struct LocalMapRequest
{
    float since{1e9f};
    float x{0.f};
    float y{0.f};
};
LocalMapRequest s_localMapRequest;

bool IsLocalMapTextureReady() noexcept
{
    POINTER_FALLOUT4(ExternalTexture*, s_localTexture, 1155496);
    const ExternalTexture* pTexture = *s_localTexture.Get();
    return pTexture && pTexture->pTexture;
}

// The local map's north west, north east and south west corners in the world, as the Pip-Boy's map page gets them
// (id 1310715 calls id 1020638, which writes each as a NiPoint3: x, y and z)
bool GetLocalMapCorners(float (&aCorners)[6]) noexcept
{
    if (!LocalMapObject())
        return false;
    using TGetCorners = void(float*, float*, float*);
    POINTER_FALLOUT4(TGetCorners, s_getCorners, 1020639);
    alignas(16) float nw[4]{};
    alignas(16) float ne[4]{};
    alignas(16) float sw[4]{};
    s_getCorners.Get()(nw, ne, sw);
    aCorners[0] = nw[0];
    aCorners[1] = nw[1];
    aCorners[2] = ne[0];
    aCorners[3] = ne[1];
    aCorners[4] = sw[0];
    aCorners[5] = sw[1];
    return (ne[0] != nw[0] || ne[1] != nw[1]) && (sw[0] != nw[0] || sw[1] != nw[1]);
}

enum class MarkKind : uint8_t
{
    kLocation,
    kUndiscovered,
    kExit,
    kQuest,
    kCustom,
    kPartyMember,
    kOtherPlayer,
};

struct Mark
{
    MarkKind kind;
    float x;
    float y;
};

// The player's quest targets, as the console's ShowQuestTargets walks them (CommonLibF4 ids 863322, 370210, 626066),
// under the player's quest target lock (locked by id 1425657, unlocked as the game does it): each quest's
// QuestTargetArray is a BSTArray of objectives, each with a BSTArray of targets at 0x20. A target's place to show is
// the first load door on the way to it (id 1490112: an invalid handle when it's in the same place), else the target
// itself or whoever carries it (id 1237673).
template <class TVisit> void ForEachQuestTarget(PlayerCharacter* apPlayer, const TVisit& acVisit) noexcept
{
    auto* pPlayer = reinterpret_cast<uint8_t*>(apPlayer);
    auto* pLock = pPlayer + kQuestTargetLockOffset;
    using TLock = void(void*, const char*);
    POINTER_FALLOUT4(TLock, s_lock, 1425658);
    s_lock.Get()(pLock, nullptr);

    const uint8_t* pMap = pPlayer + kQuestTargetsOffset;
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(pMap + 0x0C);
    const uint8_t* pEntries = *reinterpret_cast<uint8_t* const*>(pMap + 0x28);
    for (uint32_t i = 0; pEntries && i < capacity; ++i)
    {
        // an entry: the quest, its targets, the next entry (none: unused)
        const uint8_t* pEntry = pEntries + i * 0x18;
        if (!*reinterpret_cast<void* const*>(pEntry + 0x10))
            continue;
        const uint8_t* pObjectives = *reinterpret_cast<uint8_t* const*>(pEntry + 0x08);
        if (!pObjectives)
            continue;
        auto* const* ppObjectives = *reinterpret_cast<uint8_t* const* const*>(pObjectives);
        const uint32_t objectiveCount = *reinterpret_cast<const uint32_t*>(pObjectives + 0x10);
        for (uint32_t j = 0; ppObjectives && j < objectiveCount; ++j)
        {
            const uint8_t* pObjective = ppObjectives[j];
            if (!pObjective)
                continue;
            auto* const* ppTargets = *reinterpret_cast<uint8_t* const* const*>(pObjective + 0x20);
            const uint32_t targetCount = *reinterpret_cast<const uint32_t*>(pObjective + 0x30);
            for (uint32_t k = 0; ppTargets && k < targetCount; ++k)
            {
                if (ppTargets[k])
                    acVisit(ppTargets[k]);
            }
        }
    }

    // BSSpinLock: owning thread, then the count
    auto* pCount = reinterpret_cast<volatile long*>(pLock + 4);
    if (*pCount == 1)
    {
        *reinterpret_cast<volatile uint32_t*>(pLock) = 0;
        _InterlockedCompareExchange(pCount, 0, 1);
    }
    else
    {
        _InterlockedDecrement(pCount);
    }
}

TESObjectREFR* QuestTargetPlace(uint8_t* apTarget) noexcept
{
    using TGetDoor = uint32_t*(uint8_t*, uint32_t*);
    using TGetTarget = uint32_t*(uint8_t*, uint32_t*, bool);
    POINTER_FALLOUT4(TGetDoor, s_getDoor, 1490113);
    POINTER_FALLOUT4(TGetTarget, s_getTarget, 1237674);
    uint32_t handle = 0;
    s_getDoor.Get()(apTarget, &handle);
    if (TESObjectREFR* pDoor = handle ? TESObjectREFR::GetByHandle(handle) : nullptr)
        return pDoor;
    handle = 0;
    s_getTarget.Get()(apTarget, &handle, true);
    return handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
}

// A map marker's MapMarkerData flags (0 when the reference isn't one): 1 shown on the map, 2 discovered (fast travel)
uint8_t MapMarkerFlags(TESObjectREFR* apRef) noexcept
{
    ExtraDataList* pExtra = apRef ? apRef->GetExtraDataList() : nullptr;
    BSExtraData* pMarker = pExtra ? pExtra->GetByType(ExtraDataType::MapMarker) : nullptr;
    const uint8_t* pData = pMarker ? *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(pMarker) + 0x18) : nullptr;
    return pData ? pData[0x10] : 0;
}

// The Pip-Boy colour of Fallout4Prefs.ini, the game's default when it isn't set
uint32_t PipboyColour() noexcept
{
    static const uint32_t s_colour = []
    {
        const std::wstring ini = (SaveFolder::GetGameUserFolder() / "Fallout4Prefs.ini").wstring();
        const auto get = [&](const wchar_t* acpKey, float aDefault)
        {
            wchar_t text[64]{};
            GetPrivateProfileStringW(L"Pipboy", acpKey, L"", text, static_cast<DWORD>(std::size(text)), ini.c_str());
            float value = aDefault;
            if (text[0])
            {
                wchar_t* pEnd = nullptr;
                value = std::wcstof(text, &pEnd);
                if (pEnd == text)
                    value = aDefault;
            }
            return static_cast<uint32_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
        };
        return get(L"fPipboyEffectColorR", 0.08f) << 16 | get(L"fPipboyEffectColorG", 1.f) << 8 | get(L"fPipboyEffectColorB", 0.09f);
    }();
    return s_colour;
}

// What the screen should show, from the game's update; the menu's own advance draws it
struct Wanted
{
    bool show{false};
    // take the drawing off (the menu closes after)
    bool close{false};
    // the map image (img://...), empty: none yet
    std::string url;
    // its north west, north east and south west corners in the world (x, y)
    float corners[6]{};
    // how far in from its left, top, right and bottom edges they are, as fractions of its size
    float gutters[4]{};
    float x{0.f};
    float y{0.f};
    // radians, 0 north, clockwise
    float heading{0.f};
    float radius{kWorldRadius};
    std::vector<Mark> marks;
    std::string place;
};
std::mutex s_wantedLock;
Wanted s_wanted;
// the GenericMenu showing our movie (drawn in its advance), and whether the drawing is off it
std::atomic<IMenu*> s_pOurMenu{nullptr};
std::atomic<bool> s_drawingOff{false};

// ---------------------------------------------------------------------------------------------------------------
// The drawing, only touched in the menu's advance (and once the menu is gone, under the same lock)

struct Screen
{
    IMenu* pMenu{nullptr};
    GFx::Movie* pMovie{nullptr};
    GFx::Value root;
    // everything, one of the menu's shader targets
    GFx::Value screen;
    // the map image, under the mask
    GFx::Value clip;
    GFx::Value mask;
    GFx::Value loader;
    // the marks, drawn again each frame
    GFx::Value marks;
    GFx::Value label;
    GFx::Value north;
    uint8_t* pTarget{nullptr};
    std::string url;
    bool imageReady{false};
    std::string place;
};
std::mutex s_screenLock;
Screen s_screen;

// A display object as a shader target (BSGFxShaderFXTarget), as HUDCompass makes its drawn part (HudMarker.cpp):
// constructor (CommonLibF4 id 599569), foreground effect 2 (876001), background 1 (278402), flag 4 (1235532). 0xB0 bytes.
uint8_t* MakeShaderTarget(const GFx::Value& acObject) noexcept
{
    using TConstructor = void*(void*, const GFx::Value&);
    using TSetEffect = void(void*, int32_t, float);
    using TSetFlag = void(void*);
    POINTER_FALLOUT4(TConstructor, s_constructor, 599570);
    POINTER_FALLOUT4(TSetEffect, s_setForeground, 876002);
    POINTER_FALLOUT4(TSetEffect, s_setBackground, 278403);
    POINTER_FALLOUT4(TSetFlag, s_setFlag4, 1235533);
    constexpr size_t kTargetSize = 0x100;
    auto* pTarget = static_cast<uint8_t*>(_aligned_malloc(kTargetSize, 16));
    if (!pTarget)
        return nullptr;
    std::memset(pTarget, 0, kTargetSize);
    s_constructor.Get()(pTarget, acObject);
    s_setForeground.Get()(pTarget, 2, 1.f);
    s_setBackground.Get()(pTarget, 1, 1.f);
    s_setFlag4.Get()(pTarget);
    return pTarget;
}

// Its destructor (slot 0, without freeing), which also leaves the game's colour events. With aMovieGone, its display
// object's value (at 0x08) is dropped first, untouched.
void DestroyShaderTarget(uint8_t* apTarget, bool aMovieGone) noexcept
{
    if (!apTarget)
        return;
    if (aMovieGone)
        reinterpret_cast<GFx::Value*>(apTarget + 0x08)->Forget();
    using TDestructor = void(void*, uint32_t);
    TDestructor** pVTable = *reinterpret_cast<TDestructor***>(apTarget);
    pVTable[0](apTarget, 0);
    _aligned_free(apTarget);
}

void ClearScreen(bool aMovieGone) noexcept
{
    GFx::Value* values[] = {&s_screen.root, &s_screen.screen, &s_screen.clip, &s_screen.mask, &s_screen.loader, &s_screen.marks, &s_screen.label, &s_screen.north};
    for (GFx::Value* pValue : values)
    {
        if (aMovieGone)
            pValue->Forget();
        else
            *pValue = GFx::Value();
    }
    s_screen.pMenu = nullptr;
    s_screen.pMovie = nullptr;
    s_screen.pTarget = nullptr;
    s_screen.url.clear();
    s_screen.imageReady = false;
    s_screen.place.clear();
}

// The movie is still there: our target out of its menu's list and gone, the drawing out of the movie
void TakeOffScreen() noexcept
{
    if (s_screen.pMenu && s_screen.pTarget)
    {
        auto& targets = *reinterpret_cast<GameArray<uint8_t*>*>(reinterpret_cast<uint8_t*>(s_screen.pMenu) + kShaderTargetsOffset);
        for (uint32_t i = 0; i < targets.length; ++i)
        {
            if (targets[i] == s_screen.pTarget)
            {
                for (uint32_t j = i + 1; j < targets.length; ++j)
                    targets[j - 1] = targets[j];
                --targets.length;
                break;
            }
        }
        DestroyShaderTarget(s_screen.pTarget, false);
    }
    if (s_screen.root.IsObject() && s_screen.screen.IsObject())
        s_screen.root.Invoke("removeChild", nullptr, &s_screen.screen, 1);
    ClearScreen(false);
}

// The menu went without us (a load closes every menu): its movie's values are dropped untouched
void ForgetScreen() noexcept
{
    if (s_screen.pTarget)
        DestroyShaderTarget(s_screen.pTarget, true);
    ClearScreen(true);
}

GFx::Value MakeText(GFx::Movie* apMovie, double aSize) noexcept
{
    GFx::Value text;
    GFx::Value format;
    apMovie->CreateObject(&text, "flash.text.TextField");
    apMovie->CreateObject(&format, "flash.text.TextFormat");
    if (!text.IsObject())
        return text;
    if (format.IsObject())
    {
        // the game's own fonts (Interface/fonts_en.swf, fontconfig.txt)
        format.SetMember("font", GFx::Value("$MAIN_Font_Bold"));
        format.SetMember("size", GFx::Value(aSize));
        format.SetMember("color", GFx::Value(static_cast<double>(PipboyColour())));
        text.SetMember("defaultTextFormat", format);
    }
    text.SetMember("embedFonts", GFx::Value(true));
    text.SetMember("selectable", GFx::Value(false));
    text.SetMember("mouseEnabled", GFx::Value(false));
    text.SetMember("autoSize", GFx::Value("left"));
    text.SetMember("text", GFx::Value(""));
    return text;
}

bool BuildScreen(IMenu* apMenu, GFx::Movie* apMovie) noexcept
{
    GFx::Value root;
    if (!apMovie->GetVariable(&root, "root") || !root.IsObject())
    {
        spdlog::warn("Realtime Pip-Boy Map: no root in our movie");
        return false;
    }
    s_screen.pMenu = apMenu;
    s_screen.pMovie = apMovie;
    s_screen.root = root;

    for (GFx::Value* pSprite : {&s_screen.screen, &s_screen.clip, &s_screen.mask, &s_screen.marks})
        apMovie->CreateObject(pSprite, "flash.display.Sprite");
    apMovie->CreateObject(&s_screen.loader, "flash.display.Loader");
    if (!s_screen.screen.IsObject() || !s_screen.clip.IsObject() || !s_screen.mask.IsObject() || !s_screen.marks.IsObject() || !s_screen.loader.IsObject())
    {
        spdlog::warn("Realtime Pip-Boy Map: couldn't make the drawing");
        ClearScreen(false);
        return false;
    }
    s_screen.label = MakeText(apMovie, 38.);
    s_screen.north = MakeText(apMovie, 32.);

    // the map image brighter
    {
        GFx::Value args[8]{GFx::Value(kImageBrightness), GFx::Value(kImageBrightness), GFx::Value(kImageBrightness), GFx::Value(1.),
                           GFx::Value(0.),               GFx::Value(0.),               GFx::Value(0.),               GFx::Value(0.)};
        GFx::Value colour;
        apMovie->CreateObject(&colour, "flash.geom.ColorTransform", args, 8);
        GFx::Value transform;
        if (colour.IsObject() && s_screen.loader.GetMember("transform", &transform) && transform.IsObject())
            transform.SetMember("colorTransform", colour);
    }
    s_screen.clip.Invoke("addChild", nullptr, &s_screen.loader, 1);
    s_screen.screen.Invoke("addChild", nullptr, &s_screen.clip, 1);
    s_screen.screen.Invoke("addChild", nullptr, &s_screen.mask, 1);
    s_screen.clip.SetMember("mask", s_screen.mask);
    s_screen.screen.Invoke("addChild", nullptr, &s_screen.marks, 1);
    if (s_screen.label.IsObject())
        s_screen.screen.Invoke("addChild", nullptr, &s_screen.label, 1);
    if (s_screen.north.IsObject())
    {
        s_screen.north.SetMember("text", GFx::Value("N"));
        s_screen.screen.Invoke("addChild", nullptr, &s_screen.north, 1);
    }
    root.Invoke("addChild", nullptr, &s_screen.screen, 1);

    s_screen.pTarget = MakeShaderTarget(s_screen.screen);
    if (s_screen.pTarget)
    {
        auto& targets = *reinterpret_cast<GameArray<uint8_t*>*>(reinterpret_cast<uint8_t*>(apMenu) + kShaderTargetsOffset);
        const uint32_t count = targets.length;
        targets.Resize(count + 1);
        targets[count] = s_screen.pTarget;
    }
    spdlog::info("Realtime Pip-Boy Map: drawing made (shader target {})", s_screen.pTarget != nullptr);
    return true;
}

// Calls on a sprite's graphics
struct Pen
{
    GFx::Value graphics;

    explicit Pen(GFx::Value& aSprite) noexcept { aSprite.GetMember("graphics", &graphics); }

    void Call(const char* acpName, std::initializer_list<double> aArgs = {}) noexcept
    {
        GFx::Value args[8];
        size_t count = 0;
        for (double arg : aArgs)
        {
            if (count < std::size(args))
                args[count++] = GFx::Value(arg);
        }
        graphics.Invoke(acpName, nullptr, args, count);
    }
    void Fill(uint32_t aColour, double aAlpha) noexcept { Call("beginFill", {static_cast<double>(aColour), aAlpha}); }
    void Line(double aWidth, uint32_t aColour, double aAlpha) noexcept { Call("lineStyle", {aWidth, static_cast<double>(aColour), aAlpha}); }
    void NoLine() noexcept { graphics.Invoke("lineStyle"); }
    void End() noexcept { graphics.Invoke("endFill"); }
    void Polygon(std::initializer_list<std::pair<double, double>> aPoints) noexcept
    {
        bool first = true;
        for (const auto& [x, y] : aPoints)
        {
            Call(first ? "moveTo" : "lineTo", {x, y});
            first = false;
        }
        if (aPoints.size() > 0)
            Call("lineTo", {aPoints.begin()->first, aPoints.begin()->second});
    }
};

// From the menu's advance: the map as it's wanted
void DrawScreen(GFx::Movie* apMovie, const Wanted& acWanted) noexcept
{
    const uint32_t colour = PipboyColour();

    // the part of the stage on the Pip-Boy's screen
    float visible[4]{0.f, 0.f, 1024.f, 768.f};
    apMovie->GetVisibleFrameRect(visible);
    const double left = visible[0] + kScreenMargin;
    const double top = visible[1] + kScreenMargin;
    const double right = visible[2] - kScreenMargin;
    const double bottom = visible[3] - kScreenMargin;
    const double centreX = (left + right) * 0.5;
    const double centreY = (top + bottom) * 0.5;
    const double half = std::min(right - left, bottom - top) * 0.5;

    // a world offset (x east, y north) to the screen: scaled, y down, turned so the player faces up
    const double scale = half / std::max(acWanted.radius, 1.f);
    const double turn = -acWanted.heading;
    const double cosine = std::cos(turn);
    const double sine = std::sin(turn);
    const auto offset = [&](double aX, double aY, double& aOutX, double& aOutY)
    {
        aOutX = scale * (cosine * aX + sine * aY);
        aOutY = scale * (sine * aX - cosine * aY);
    };
    const auto toScreen = [&](double aX, double aY, double& aOutX, double& aOutY)
    {
        offset(aX - acWanted.x, aY - acWanted.y, aOutX, aOutY);
        aOutX += centreX;
        aOutY += centreY;
    };

    // the screen's backing and the map's mask
    {
        Pen pen(s_screen.screen);
        pen.Call("clear");
        pen.Fill(0x000000, 1.);
        pen.Call("drawRect", {visible[0], visible[1], visible[2] - visible[0], visible[3] - visible[1]});
        pen.End();
        Pen mask(s_screen.mask);
        mask.Call("clear");
        mask.Fill(0xFFFFFF, 1.);
        mask.Call("drawRect", {left, top, right - left, bottom - top});
        mask.End();
    }

    // the map image: loaded when it changes, then placed so its corners land where they are in the world
    if (acWanted.url != s_screen.url)
    {
        s_screen.url = acWanted.url;
        s_screen.imageReady = false;
        if (!s_screen.url.empty())
        {
            GFx::Value url(s_screen.url.c_str());
            GFx::Value request;
            apMovie->CreateObject(&request, "flash.net.URLRequest", &url, 1);
            if (request.IsObject())
            {
                s_screen.loader.Invoke("load", nullptr, &request, 1);
            }
        }
        else
        {
            s_screen.loader.Invoke("unload");
        }
    }
    double imageWidth = 0.;
    double imageHeight = 0.;
    GFx::Value content;
    if (!s_screen.url.empty() && s_screen.loader.GetMember("content", &content) && content.IsObject())
    {
        GFx::Value width;
        GFx::Value height;
        content.GetMember("width", &width);
        content.GetMember("height", &height);
        imageWidth = width.GetNumber();
        imageHeight = height.GetNumber();
        if (!s_screen.imageReady && imageWidth > 0. && imageHeight > 0.)
        {
            s_screen.imageReady = true;
            content.SetMember("smoothing", GFx::Value(true));
            spdlog::info("Realtime Pip-Boy Map: {} is {} x {}", s_screen.url, imageWidth, imageHeight);
        }
    }
    const bool hasImage = s_screen.imageReady && imageWidth > 0. && imageHeight > 0.;
    for (GFx::Value* pImage : {&s_screen.loader})
    {
        if (!pImage->IsObject())
            continue;
        pImage->SetMember("visible", GFx::Value(hasImage));
        if (!hasImage)
            continue;
        const float* pCorners = acWanted.corners;
        const float* pGutters = acWanted.gutters;
        // the world one image pixel spans to the right and down: the corners span the image less its gutters
        const double spanX = imageWidth * (1. - pGutters[0] - pGutters[2]);
        const double spanY = imageHeight * (1. - pGutters[1] - pGutters[3]);
        const double rightX = (pCorners[2] - pCorners[0]) / spanX;
        const double rightY = (pCorners[3] - pCorners[1]) / spanX;
        const double downX = (pCorners[4] - pCorners[0]) / spanY;
        const double downY = (pCorners[5] - pCorners[1]) / spanY;
        double ax, ay, bx, by, tx, ty;
        offset(rightX, rightY, ax, ay);
        offset(downX, downY, bx, by);
        // the image's top left: out from the north west corner by the left and top gutters
        const double leftPixels = imageWidth * pGutters[0];
        const double topPixels = imageHeight * pGutters[1];
        toScreen(pCorners[0] - leftPixels * rightX - topPixels * downX, pCorners[1] - leftPixels * rightY - topPixels * downY, tx, ty);
        GFx::Value args[6]{GFx::Value(ax), GFx::Value(ay), GFx::Value(bx), GFx::Value(by), GFx::Value(tx), GFx::Value(ty)};
        GFx::Value matrix;
        apMovie->CreateObject(&matrix, "flash.geom.Matrix", args, 6);
        GFx::Value transform;
        if (matrix.IsObject() && pImage->GetMember("transform", &transform) && transform.IsObject())
            transform.SetMember("matrix", matrix);
    }

    Pen pen(s_screen.marks);
    pen.Call("clear");

    // without an image yet: a faint range ring, so the screen isn't empty
    if (!hasImage)
    {
        pen.Line(1.5, colour, 0.3);
        pen.Call("drawCircle", {centreX, centreY, half * 0.5});
        pen.Call("drawCircle", {centreX, centreY, half});
        pen.NoLine();
    }

    const auto inside = [&](double aX, double aY, double aMargin) { return aX >= left + aMargin && aX <= right - aMargin && aY >= top + aMargin && aY <= bottom - aMargin; };
    const double k = kMarkScale;

    for (const Mark& mark : acWanted.marks)
    {
        double x = 0.;
        double y = 0.;
        toScreen(mark.x, mark.y, x, y);
        switch (mark.kind)
        {
        case MarkKind::kLocation:
        case MarkKind::kUndiscovered:
        {
            if (!inside(x, y, 6. * k))
                break;
            if (mark.kind == MarkKind::kLocation)
            {
                pen.Fill(colour, 1.);
                pen.Line(1.5 * k, 0x000000, 0.8);
            }
            else
            {
                pen.Line(2. * k, colour, 0.9);
            }
            pen.Polygon({{x, y - 8. * k}, {x + 8. * k, y}, {x, y + 8. * k}, {x - 8. * k, y}});
            if (mark.kind == MarkKind::kLocation)
                pen.End();
            pen.NoLine();
            break;
        }
        case MarkKind::kExit:
        {
            if (!inside(x, y, 8. * k))
                break;
            pen.Line(2.5 * k, colour, 0.9);
            pen.Call("drawRect", {x - 6. * k, y - 9. * k, 12. * k, 18. * k});
            pen.NoLine();
            break;
        }
        case MarkKind::kCustom:
        {
            if (!inside(x, y, 8. * k))
                break;
            pen.Line(3. * k, colour, 1.);
            pen.Call("moveTo", {x - 7. * k, y - 7. * k});
            pen.Call("lineTo", {x + 7. * k, y + 7. * k});
            pen.Call("moveTo", {x + 7. * k, y - 7. * k});
            pen.Call("lineTo", {x - 7. * k, y + 7. * k});
            pen.NoLine();
            break;
        }
        case MarkKind::kPartyMember:
        case MarkKind::kOtherPlayer:
        {
            if (!inside(x, y, 7. * k))
                break;
            if (mark.kind == MarkKind::kPartyMember)
            {
                pen.Fill(colour, 1.);
                pen.Line(1.5 * k, 0x000000, 0.8);
                pen.Call("drawCircle", {x, y, 8. * k});
                pen.End();
            }
            else
            {
                pen.Line(2.5 * k, colour, 0.9);
                pen.Call("drawCircle", {x, y, 7. * k});
                pen.NoLine();
            }
            break;
        }
        case MarkKind::kQuest:
        {
            // off the screen: kept at its edge, pointing the way
            const double edge = 14. * k;
            if (!inside(x, y, edge))
            {
                const double dx = x - centreX;
                const double dy = y - centreY;
                const double reachX = (right - left) * 0.5 - edge;
                const double reachY = (bottom - top) * 0.5 - edge;
                const double t = std::min(std::abs(dx) > 0.001 ? reachX / std::abs(dx) : 1e9, std::abs(dy) > 0.001 ? reachY / std::abs(dy) : 1e9);
                const double length = std::hypot(dx, dy);
                if (length < 0.001)
                    break;
                x = centreX + dx * t;
                y = centreY + dy * t;
                const double ux = dx / length * k;
                const double uy = dy / length * k;
                pen.Fill(colour, 1.);
                pen.Line(1.5 * k, 0x000000, 0.8);
                pen.Polygon({{x + ux * 12., y + uy * 12.}, {x - ux * 6. - uy * 9., y - uy * 6. + ux * 9.}, {x - ux * 6. + uy * 9., y - uy * 6. - ux * 9.}});
                pen.End();
                break;
            }
            // the game's quest marker: a triangle pointing down at the place
            pen.Fill(colour, 1.);
            pen.Line(1.5 * k, 0x000000, 0.8);
            pen.Polygon({{x - 9. * k, y - 14. * k}, {x + 9. * k, y - 14. * k}, {x, y + 1. * k}});
            pen.End();
            break;
        }
        }
    }

    // the player in the middle, facing up
    pen.Fill(colour, 1.);
    pen.Line(1.5 * k, 0x000000, 0.8);
    pen.Polygon({{centreX, centreY - 13. * k}, {centreX + 9. * k, centreY + 10. * k}, {centreX, centreY + 5. * k}, {centreX - 9. * k, centreY + 10. * k}});
    pen.End();
    pen.NoLine();

    // north, on a ring near the edge
    double northX = 0.;
    double northY = 0.;
    offset(0., 1., northX, northY);
    const double northLength = std::hypot(northX, northY);
    if (northLength > 0.)
    {
        northX /= northLength;
        northY /= northLength;
        const double ring = half - 10. * k;
        const double px = centreX + northX * ring;
        const double py = centreY + northY * ring;
        const double nx = northX * k;
        const double ny = northY * k;
        pen.Fill(colour, 0.9);
        pen.Polygon({{px + nx * 8., py + ny * 8.}, {px - nx * 4. - ny * 6., py - ny * 4. + nx * 6.}, {px - nx * 4. + ny * 6., py - ny * 4. - nx * 6.}});
        pen.End();
        if (s_screen.north.IsObject())
        {
            s_screen.north.SetMember("x", GFx::Value(centreX + northX * (ring - 24. * k) - 9.));
            s_screen.north.SetMember("y", GFx::Value(centreY + northY * (ring - 24. * k) - 16.));
        }
    }

    // the screen's frame
    pen.Line(2., colour, 0.6);
    pen.Call("drawRect", {left, top, right - left, bottom - top});
    pen.NoLine();

    // where the player is
    if (s_screen.label.IsObject())
    {
        if (acWanted.place != s_screen.place)
        {
            s_screen.place = acWanted.place;
            s_screen.label.SetMember("text", GFx::Value(s_screen.place.c_str()));
        }
        s_screen.label.SetMember("x", GFx::Value(left + 10.));
        s_screen.label.SetMember("y", GFx::Value(top + 6.));
    }
}

// The GenericMenu's advance (IMenu slot 4, CommonLibF4 id 1165196): where the game captures the movie for drawing
void ApplyScreen(IMenu* apMenu) noexcept
{
    auto* pMovie = static_cast<GFx::Movie*>(apMenu->pMovie);
    if (!pMovie)
        return;

    Wanted wanted;
    {
        std::scoped_lock _(s_wantedLock);
        wanted = s_wanted;
    }

    std::scoped_lock _(s_screenLock);
    if (wanted.close)
    {
        if (s_screen.pMenu)
            TakeOffScreen();
        s_drawingOff = true;
        return;
    }
    if (!wanted.show)
        return;
    if (s_screen.pMenu != apMenu || s_screen.pMovie != pMovie)
    {
        if (s_screen.pMenu)
            ForgetScreen();
        if (!BuildScreen(apMenu, pMovie))
            return;
    }
    DrawScreen(pMovie, wanted);
}

TP_THIS_FUNCTION(TAdvanceMovie, void, IMenu, float aDelta, uint64_t aTime);
TAdvanceMovie* RealGenericAdvanceMovie = nullptr;

void TP_MAKE_THISCALL(HookGenericAdvanceMovie, IMenu, float aDelta, uint64_t aTime)
{
    static PerfProbe::HookStat s_perf("MenuAdvanceMovie");
    PerfProbe::Hook _perf(s_perf);
    if (apThis && apThis == s_pOurMenu.load())
        ApplyScreen(apThis);
    PerfProbe::GameCall _game;
    TiltedPhoques::ThisCall(RealGenericAdvanceMovie, apThis, aDelta, aTime);
}

// ---------------------------------------------------------------------------------------------------------------
// The game's controls (each handler's OnButtonEvent, slot 8 of its vtable; ButtonEvent in CommonLibF4: the user event
// at 0x28, the value at 0x38, how long it's been down at 0x3C)

struct InputHandler;
TP_THIS_FUNCTION(TButtonEvent, void, InputHandler, const void* apEvent);

const BSFixedString::Data* UserEventOf(const void* apEvent) noexcept
{
    return *reinterpret_cast<BSFixedString::Data* const*>(static_cast<const uint8_t*>(apEvent) + 0x28);
}
float ValueOf(const void* apEvent) noexcept
{
    return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(apEvent) + 0x38);
}
float HeldOf(const void* apEvent) noexcept
{
    return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(apEvent) + 0x3C);
}
bool IsUserEvent(const void* apEvent, const char* acpName) noexcept
{
    return UserEventOf(apEvent) == FixedString(acpName).data;
}

// PipboyHandler (CommonLibF4 id 181358): Tab and the quick keys. Quick Map (M) is ours while the map is on; Tab takes
// the arm and the screen for the game's own Pip-Boy.
TButtonEvent* RealPipboyButton = nullptr;
void TP_MAKE_THISCALL(HookPipboyButton, InputHandler, const void* apEvent)
{
    if (s_pService && apEvent && s_pService->IsEnabled())
    {
        if (IsUserEvent(apEvent, "QuickMap"))
        {
            const float value = ValueOf(apEvent);
            const float held = HeldOf(apEvent);
            // a press, or a release; the frames it stays down in between are nothing
            if (value == 0.f || held == 0.f)
                s_pService->OnMapButton(value != 0.f, held);
            return;
        }
        if (s_pService->IsShowing() && IsUserEvent(apEvent, "Pipboy"))
            s_pService->Abort();
    }
    TiltedPhoques::ThisCall(RealPipboyButton, apThis, apEvent);
}

// Attacking or aiming (AttackBlockHandler, id 153517), sprinting (SprintHandler, 1356251), holstering or reloading
// (ReadyWeaponHandler, 1411297), bashing or throwing (MeleeThrowHandler, 150410): with the map up, the press lowers it
// and does nothing else, nor does the rest of it until it's let go
enum class Swallowed
{
    kAttack,
    kSprint,
    kReadyWeapon,
    kMeleeThrow,
    kCount,
};
bool s_swallowing[static_cast<size_t>(Swallowed::kCount)]{};

bool LowersMap(Swallowed aControl, const void* apEvent) noexcept
{
    if (!apEvent)
        return false;
    bool& swallowing = s_swallowing[static_cast<size_t>(aControl)];
    const float value = ValueOf(apEvent);
    if (swallowing)
    {
        if (value == 0.f)
            swallowing = false;
        return true;
    }
    if (value != 0.f && s_pService && s_pService->IsRaised())
    {
        s_pService->Lower();
        swallowing = true;
        return true;
    }
    return false;
}

// With the map down, dual wielding (DualWieldService) has the two buttons next: right mouse for a weapon in the left
// hand, the grapple's press held back while it aims
TButtonEvent* RealAttackButton = nullptr;
void TP_MAKE_THISCALL(HookAttackButton, InputHandler, const void* apEvent)
{
    if (LowersMap(Swallowed::kAttack, apEvent))
        return;
    switch (DualWieldService::OnAttackButton(apEvent))
    {
    case DualWieldService::AttackInput::kSwallow: return;
    case DualWieldService::AttackInput::kReplayPress: TiltedPhoques::ThisCall(RealAttackButton, apThis, DualWieldService::HeldPress()); break;
    case DualWieldService::AttackInput::kReleaseInstead: TiltedPhoques::ThisCall(RealAttackButton, apThis, DualWieldService::ReleaseEvent()); return;
    case DualWieldService::AttackInput::kPass: break;
    }
    TiltedPhoques::ThisCall(RealAttackButton, apThis, apEvent);
}

TButtonEvent* RealSprintButton = nullptr;
void TP_MAKE_THISCALL(HookSprintButton, InputHandler, const void* apEvent)
{
    if (!LowersMap(Swallowed::kSprint, apEvent))
        TiltedPhoques::ThisCall(RealSprintButton, apThis, apEvent);
}

TButtonEvent* RealReadyWeaponButton = nullptr;
void TP_MAKE_THISCALL(HookReadyWeaponButton, InputHandler, const void* apEvent)
{
    if (LowersMap(Swallowed::kReadyWeapon, apEvent))
        return;
    // a reload tap with a weapon in each hand also lowers both for the two-gun reload
    DualWieldService::OnReadyWeaponButton(apEvent);
    // (riding, the button is only the left hand slot's: it never draws the right hand's weapon)
    if (BikeLink::IsRiding())
        return;
    TiltedPhoques::ThisCall(RealReadyWeaponButton, apThis, apEvent);
}

TButtonEvent* RealMeleeThrowButton = nullptr;
void TP_MAKE_THISCALL(HookMeleeThrowButton, InputHandler, const void* apEvent)
{
    if (!LowersMap(Swallowed::kMeleeThrow, apEvent))
        TiltedPhoques::ThisCall(RealMeleeThrowButton, apThis, apEvent);
}

// The view's controls, the third person key (TogglePOVHandler, id 1263350) and the wheel (CameraZoomHandler, id
// 1303092): nothing while the map is up, which is first person only. A press the game won't switch the view for says
// why in the log (PovSwitch).
TButtonEvent* RealTogglePovButton = nullptr;
void TP_MAKE_THISCALL(HookTogglePovButton, InputHandler, const void* apEvent)
{
    const bool mapUp = s_pService && s_pService->IsShowing();
    if (apEvent && ValueOf(apEvent) != 0.f && HeldOf(apEvent) == 0.f)
        PovSwitch::OnPressed(mapUp);
    if (mapUp)
        return;
    TiltedPhoques::ThisCall(RealTogglePovButton, apThis, apEvent);
}

TButtonEvent* RealCameraZoomButton = nullptr;
void TP_MAKE_THISCALL(HookCameraZoomButton, InputHandler, const void* apEvent)
{
    if (s_pService && s_pService->IsShowing())
        return;
    TiltedPhoques::ThisCall(RealCameraZoomButton, apThis, apEvent);
}

// UI's "is this menu open" (CommonLibF4 id 1065114): yes for the frame's local map check, once, when ours asked
using TIsMenuOpen = bool(void*, const BSFixedString&);
TIsMenuOpen* RealIsMenuOpen = nullptr;

bool HookIsMenuOpen(void* apUI, const BSFixedString& acName)
{
    if (s_localMapRenderWanted.load(std::memory_order_relaxed))
    {
        POINTER_FALLOUT4(uint8_t, s_frameRender, 408684);
        if (_ReturnAddress() == s_frameRender.Get() + 0x18D)
        {
            s_localMapRenderWanted = false;
            return true;
        }
    }
    return RealIsMenuOpen(apUI, acName);
}

TiltedPhoques::Initializer s_pipboyMapHooks(
    []()
    {
        POINTER_FALLOUT4(TAdvanceMovie, s_genericAdvanceMovie, 1165197);
        RealGenericAdvanceMovie = s_genericAdvanceMovie.Get();
        TP_HOOK(&RealGenericAdvanceMovie, HookGenericAdvanceMovie);

        POINTER_FALLOUT4(TIsMenuOpen, s_isMenuOpen, 1065115);
        RealIsMenuOpen = s_isMenuOpen.Get();
        TP_HOOK(&RealIsMenuOpen, HookIsMenuOpen);

        const auto hook = [](TButtonEvent*& aReal, uint32_t aId, auto aHook)
        {
            const VersionDbPtr<TButtonEvent> function(aId);
            aReal = function.Get();
            TP_HOOK(&aReal, aHook);
        };
        hook(RealPipboyButton, 181359, HookPipboyButton);
        hook(RealAttackButton, 153518, HookAttackButton);
        hook(RealSprintButton, 1356252, HookSprintButton);
        hook(RealReadyWeaponButton, 1411298, HookReadyWeaponButton);
        hook(RealMeleeThrowButton, 150411, HookMeleeThrowButton);
        hook(RealTogglePovButton, 1263351, HookTogglePovButton);
        hook(RealCameraZoomButton, 1303093, HookCameraZoomButton);
    });

// ---------------------------------------------------------------------------------------------------------------
// The marks that don't move, gathered a few times a second (the game's update)
struct Gathered
{
    float refresh{0.f};
    TESObjectCELL* pCell{nullptr};
    std::vector<Mark> marks;
    // a cell's load doors, found once per cell
    TESObjectCELL* pExitCell{nullptr};
    std::vector<Mark> exits;
};
Gathered s_gathered;

std::filesystem::path ConfigPath() noexcept
{
    return TiltedPhoques::GetPath() / "config" / "PipboyMap.ini";
}
} // namespace

PipboyMapService::PipboyMapService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    s_pService = this;
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&PipboyMapService::OnUpdate>(this);
    LoadConfig();
}

PipboyMapService::~PipboyMapService() noexcept
{
    if (s_pService == this)
        s_pService = nullptr;
}

void PipboyMapService::LoadConfig() noexcept
{
    std::ifstream file(ConfigPath());
    if (!file)
    {
        std::error_code error;
        std::filesystem::create_directories(ConfigPath().parent_path(), error);
        std::ofstream out(ConfigPath(), std::ios::trunc);
        out << "; Realtime Pip-Boy Map: M (the game's Quick Map key) raises the Pip-Boy and shows a live map on it.\n"
               "; Tap M to keep it up until you press M again, or hold M to look while it's held.\n"
               "; enabled=false gives M back to the game's own map.\n"
               "enabled=true\n";
        spdlog::info("Realtime Pip-Boy Map: on, M raises it (settings made in {})", ConfigPath().string());
        return;
    }
    std::string line;
    while (std::getline(file, line))
    {
        const auto equals = line.find('=');
        if (line.empty() || line[0] == ';' || equals == std::string::npos)
            continue;
        const auto trim = [](std::string aText)
        {
            const auto begin = aText.find_first_not_of(" \t\r");
            const auto end = aText.find_last_not_of(" \t\r");
            return begin == std::string::npos ? std::string() : aText.substr(begin, end - begin + 1);
        };
        const std::string name = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (name == "enabled")
            m_enabled = !(value == "false" || value == "off" || value == "0");
    }
    spdlog::info("Realtime Pip-Boy Map: {}", m_enabled ? "on, M raises it" : "off, M opens the game's map");
}

const char* PipboyMapService::Blocked(PlayerCharacter* apPlayer, bool aRaising) const noexcept
{
    if (!apPlayer || apPlayer->IsDead())
        return "dead";
    if (apPlayer->IsInPowerArmor())
        return "in power armor";
    if (!apPlayer->GetParentCellEx())
        return "not in a cell";
    if (MenuModeCount() > 0)
        return "a menu is open";
    for (const char* pMenu : kBlockingMenus)
    {
        if (IsMenuOpen(pMenu))
            return pMenu;
    }
    if (ParkourService::IsMoving(apPlayer))
        return "a parkour move";
    if (!aRaising && m_phaseTime > 0.5f && !IsFirstPerson())
        return "third person";
    return nullptr;
}

void PipboyMapService::OnMapButton(bool aDown, float aHeldSeconds) noexcept
{
    if (aDown)
    {
        if (m_phase == Phase::kIdle)
        {
            Raise(PlayerCharacter::Get());
            m_raisedByPress = m_phase == Phase::kUp;
        }
        else if (m_phase == Phase::kUp)
        {
            Lower();
        }
        return;
    }
    // let go: a hold lowers it, a tap leaves it up
    if (m_raisedByPress && m_phase == Phase::kUp && aHeldSeconds >= kHoldTime)
        Lower();
    m_raisedByPress = false;
}

void PipboyMapService::Raise(PlayerCharacter* apPlayer) noexcept
{
    if (const char* pWhy = Blocked(apPlayer, true))
    {
        spdlog::info("Realtime Pip-Boy Map: not raised ({})", pWhy);
        return;
    }

    // On the motorcycle the camera is its plugin's: it lets go for as long as the glance has the view, and the ride goes on
    BikeLink::SetViewHeld(true);
    m_restoreThirdPerson = !IsFirstPerson();
    if (m_restoreThirdPerson)
        SetCameraState(kFirstPersonState);

    if (!PipboyGlancePose::Start(apPlayer))
    {
        spdlog::warn("Realtime Pip-Boy Map: the arm can't be posed");
        if (m_restoreThirdPerson)
            SetCameraState(kThirdPersonState);
        m_restoreThirdPerson = false;
        BikeLink::SetViewHeld(false);
        return;
    }
    // the weapon out of the hand while the arm is up, as the game's own Pip-Boy does
    apPlayer->CullWeapons(true);
    m_weaponsCulled = true;

    {
        std::scoped_lock _(s_wantedLock);
        s_wanted = Wanted{};
        s_wanted.show = true;
    }
    s_drawingOff = false;
    s_gathered.refresh = 0.f;
    s_localMapRequest = {};

    m_screenOn = false;
    m_screenOnTime = 0.f;
    m_menuAsked = false;
    m_pMenu = nullptr;
    m_phase = Phase::kUp;
    m_phaseTime = 0.f;
    // in first person already, the screen's renderer at once (else once the view has switched, see OnUpdate)
    if (!m_restoreThirdPerson)
        TurnScreenOn();
    UpdateWanted(apPlayer, 0.f);
    spdlog::info("Realtime Pip-Boy Map: raised{}", m_restoreThirdPerson ? " (from third person)" : "");
}

void PipboyMapService::TurnScreenOn() noexcept
{
    PipboyManagerCall(kEnablePipboyShader);
    StopPipboyBlur();
    m_screenOn = true;
    m_screenOnTime = m_phaseTime;
}

void PipboyMapService::Lower() noexcept
{
    if (m_phase != Phase::kUp)
        return;
    PipboyGlancePose::Stop();
    m_phase = Phase::kLowering;
    m_phaseTime = 0.f;
    m_raisedByPress = false;
}

void PipboyMapService::Abort() noexcept
{
    if (m_phase == Phase::kIdle)
        return;
    PipboyGlancePose::StopNow();
    s_pOurMenu = nullptr;
    {
        std::scoped_lock _(s_screenLock);
        if (s_screen.pMenu)
            TakeOffScreen();
    }
    m_restoreThirdPerson = false;
    Finish();
    spdlog::info("Realtime Pip-Boy Map: off for the game's Pip-Boy");
}

void PipboyMapService::Finish() noexcept
{
    s_pOurMenu = nullptr;
    if (m_menuAsked)
        SendMenuMessage("GenericMenu", 3);
    if (m_screenOn)
    {
        PipboyManagerCall(kDisablePipboyShader);
        StopPipboyBlur();
    }
    m_screenOn = false;
    m_menuAsked = false;
    s_localMapRenderWanted = false;
    if (m_madeLocalMap && !IsMenuOpen("PipboyMenu"))
        FreeLocalMap();
    m_madeLocalMap = false;
    if (m_restoreThirdPerson)
    {
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (pPlayer && !pPlayer->IsDead() && IsFirstPerson() && MenuModeCount() == 0)
            SetCameraState(kThirdPersonState);
    }
    m_restoreThirdPerson = false;
    BikeLink::SetViewHeld(false);
    if (m_weaponsCulled)
    {
        if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
            pPlayer->CullWeapons(false);
        m_weaponsCulled = false;
    }
    {
        std::scoped_lock _(s_wantedLock);
        s_wanted = Wanted{};
    }
    m_pMenu = nullptr;
    m_phase = Phase::kIdle;
    m_phaseTime = 0.f;
}

void PipboyMapService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("PipboyMap");
    // a frame's time, at most a tenth of a second (the first frames after a load report the whole load)
    const float delta = std::min(static_cast<float>(acEvent.Delta), 0.1f);
    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    if (m_phase == Phase::kIdle)
        return;
    m_phaseTime += delta;

    // from third person, the screen's renderer once the first person view is in (turned on in the same frame as the
    // switch, the screen stayed blank)
    if (m_phase == Phase::kUp && !m_screenOn && m_phaseTime >= kViewSwitchDelay)
        TurnScreenOn();
    // the screen's menu, once the renderer turned on a frame or two ago has started (a menu made before it stays dark)
    if (m_phase == Phase::kUp && m_screenOn && !m_menuAsked && m_phaseTime >= m_screenOnTime + kMenuDelay)
    {
        m_menuAsked = true;
        PrepareGenericMenu();
        SendMenuMessage("GenericMenu", 1);
    }

    // the GenericMenu, once open; gone without us closing it (a load closes every menu), its drawing is dropped
    if (m_phase != Phase::kClosing || m_pMenu)
    {
        UI* pUI = UI::Get();
        IMenu* pOpen = pUI ? pUI->FindMenuByName("GenericMenu") : nullptr;
        if (!m_pMenu && pOpen && m_phase != Phase::kClosing)
        {
            m_pMenu = pOpen;
            s_pOurMenu = pOpen;
        }
        else if (m_pMenu && pOpen != m_pMenu)
        {
            spdlog::info("Realtime Pip-Boy Map: the screen's menu closed");
            s_pOurMenu = nullptr;
            m_pMenu = nullptr;
            {
                std::scoped_lock _(s_screenLock);
                if (s_screen.pMenu)
                    ForgetScreen();
            }
            s_drawingOff = true;
            if (m_phase == Phase::kUp)
                Lower();
        }
    }

    switch (m_phase)
    {
    case Phase::kUp:
        if (const char* pWhy = Blocked(pPlayer, false))
        {
            spdlog::info("Realtime Pip-Boy Map: lowered ({})", pWhy);
            Lower();
        }
        break;
    case Phase::kLowering:
        if (!PipboyGlancePose::IsActive() || m_phaseTime >= kLowerTime)
        {
            {
                std::scoped_lock _(s_wantedLock);
                s_wanted.close = true;
            }
            if (!m_pMenu)
                s_drawingOff = true;
            m_phase = Phase::kClosing;
            m_phaseTime = 0.f;
        }
        break;
    case Phase::kClosing:
        if (s_drawingOff || m_phaseTime >= kCloseTimeout)
        {
            if (!s_drawingOff)
            {
                // the menu never drew again: its drawing is dropped, the menu closes with it
                s_pOurMenu = nullptr;
                std::scoped_lock _(s_screenLock);
                if (s_screen.pMenu)
                    TakeOffScreen();
            }
            Finish();
        }
        return;
    case Phase::kIdle: return;
    }

    if (pPlayer && (m_phase == Phase::kUp || m_phase == Phase::kLowering))
        UpdateWanted(pPlayer, delta);
}

void PipboyMapService::UpdateWanted(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    Space space;
    space.pCell = apPlayer->GetParentCellEx();
    space.interior = IsInterior(space.pCell);
    space.pWorld = space.interior || !space.pCell ? nullptr : TopWorld(space.pCell->worldspace);

    Wanted wanted;
    wanted.show = true;
    wanted.x = apPlayer->position.x;
    wanted.y = apPlayer->position.y;
    wanted.heading = apPlayer->rotation.z;
    wanted.radius = space.interior ? kLocalRadius : kWorldRadius;

    // the map image: the game's local map inside, the worldspace's map outside
    if (space.interior)
    {
        if (!LocalMapObject() && m_phase == Phase::kUp)
        {
            MakeLocalMap();
            m_madeLocalMap = LocalMapObject() != nullptr;
            spdlog::info("Realtime Pip-Boy Map: local map made {}", m_madeLocalMap);
        }
        // rendered when there's none yet, then again as the player moves
        const bool ready = IsLocalMapTextureReady();
        s_localMapRequest.since += aDelta;
        const bool moved = std::hypot(wanted.x - s_localMapRequest.x, wanted.y - s_localMapRequest.y) > kLocalMapRefreshDistance;
        if (LocalMapObject() && s_localMapRequest.since >= kLocalMapRefreshTime && (!ready || moved))
        {
            RequestLocalMapRender();
            s_localMapRequest = {0.f, wanted.x, wanted.y};
        }
        // its corners asked every frame, as the map page does
        if (GetLocalMapCorners(wanted.corners) && ready)
            wanted.url = "img://LocalMap";
    }
    else
    {
        const WorldMap map = GetWorldMap();
        if (map.pWorld && map.seX != map.nwX && map.seY != map.nwY)
        {
            const std::string path = std::string("Textures\\") + (map.pTexture && *map.pTexture ? map.pTexture : "Interface\\Pip-Boy\\WorldMap_d.dds");
            const std::string& name = WorldTextureName(path);
            if (!name.empty())
            {
                // the cells' corners (4096 units a cell), as the map page places the texture (CommonLibF4 id 1231434)
                constexpr float kCell = 4096.f;
                wanted.url = "img://" + name;
                const float corners[6]{map.nwX * kCell, map.nwY * kCell, map.seX * kCell, map.nwY * kCell, map.nwX * kCell, map.seY * kCell};
                std::copy(std::begin(corners), std::end(corners), wanted.corners);
                GetWorldMapGutters(map.pTexture, wanted.gutters);
            }
        }
    }

    // the marks that don't move
    s_gathered.refresh -= aDelta;
    if (s_gathered.refresh <= 0.f || s_gathered.pCell != space.pCell)
    {
        s_gathered.refresh = kMarksRefresh;
        s_gathered.pCell = space.pCell;
        s_gathered.marks.clear();
        const float reach = wanted.radius * kGatherRadiusScale;
        const auto isClose = [&](const TESObjectREFR* apRef) { return std::abs(apRef->position.x - wanted.x) < reach && std::abs(apRef->position.y - wanted.y) < reach; };

        if (space.interior)
        {
            // the cell's way out: its load doors
            if (s_gathered.pExitCell != space.pCell)
            {
                s_gathered.pExitCell = space.pCell;
                s_gathered.exits.clear();
                for (TESObjectREFR* pRef : space.pCell->objects)
                {
                    ExtraDataList* pExtra = pRef ? pRef->GetExtraDataList() : nullptr;
                    if (pExtra && pExtra->GetByType(ExtraDataType::Teleport))
                        s_gathered.exits.push_back({MarkKind::kExit, pRef->position.x, pRef->position.y});
                }
            }
            s_gathered.marks = s_gathered.exits;
        }
        else
        {
            // the locations the map shows, filled once discovered
            const uint8_t* pMarkers = reinterpret_cast<const uint8_t*>(apPlayer) + kMapMarkersOffset;
            const auto* pHandles = *reinterpret_cast<const uint32_t* const*>(pMarkers);
            const uint32_t count = *reinterpret_cast<const uint32_t*>(pMarkers + 0x10);
            for (uint32_t i = 0; pHandles && i < count; ++i)
            {
                TESObjectREFR* pRef = TESObjectREFR::GetByHandle(pHandles[i]);
                if (!pRef || !isClose(pRef) || !space.Has(pRef))
                    continue;
                const uint8_t flags = MapMarkerFlags(pRef);
                if (flags & 1)
                    s_gathered.marks.push_back({flags & 2 ? MarkKind::kLocation : MarkKind::kUndiscovered, pRef->position.x, pRef->position.y});
            }
        }

        // the player's custom marker
        const uint32_t custom = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(apPlayer) + kCustomMarkerOffset);
        if (TESObjectREFR* pCustom = custom ? TESObjectREFR::GetByHandle(custom) : nullptr; pCustom && space.Has(pCustom))
            s_gathered.marks.push_back({MarkKind::kCustom, pCustom->position.x, pCustom->position.y});

        // the quest targets (shown wherever they are: off the screen they wait at its edge)
        std::vector<Mark> quests;
        ForEachQuestTarget(apPlayer,
                           [&](uint8_t* apTarget)
                           {
                               TESObjectREFR* pPlace = QuestTargetPlace(apTarget);
                               if (pPlace && space.Has(pPlace))
                                   quests.push_back({MarkKind::kQuest, pPlace->position.x, pPlace->position.y});
                           });
        for (const Mark& quest : quests)
        {
            const bool same = std::any_of(s_gathered.marks.begin(), s_gathered.marks.end(),
                                          [&](const Mark& acMark) { return acMark.kind == MarkKind::kQuest && std::abs(acMark.x - quest.x) < 64.f && std::abs(acMark.y - quest.y) < 64.f; });
            if (!same)
                s_gathered.marks.push_back(quest);
        }
    }
    wanted.marks = s_gathered.marks;

    // the other players, where they are this frame: the party's filled
    {
        const auto& party = m_world.GetPartyService().GetPartyMembers();
        auto view = m_world.view<FormIdComponent, PlayerComponent>();
        for (auto entity : view)
        {
            auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
            if (!pActor || pActor == apPlayer || !space.Has(pActor))
                continue;
            const uint32_t playerId = view.get<PlayerComponent>(entity).Id;
            const bool inParty = std::find(party.begin(), party.end(), playerId) != party.end();
            wanted.marks.push_back({inParty ? MarkKind::kPartyMember : MarkKind::kOtherPlayer, pActor->position.x, pActor->position.y});
        }
    }

    // where the player is: the location's name, else the cell's
    if (TESForm* pLocation = apPlayer->locationForm; pLocation && pLocation->GetName() && *pLocation->GetName())
        wanted.place = pLocation->GetName();
    else if (space.pCell && space.pCell->GetName())
        wanted.place = space.pCell->GetName();

    std::scoped_lock _(s_wantedLock);
    s_wanted = std::move(wanted);
}

#endif

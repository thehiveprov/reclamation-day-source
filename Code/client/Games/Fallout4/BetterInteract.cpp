#include <PlayerCharacter.h>
#include <TESObjectREFR.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <ExtraData/ExtraDataList.h>
#include <Games/TES.h>
#include <Games/Memory.h>
#include <Interface/UI.h>
#include <Interface/HudMarker.h>
#include <Misc/BSFixedString.h>
#include <Forms/ActorValueInfo.h>
#include <Services/PapyrusService.h>
#include <SelectionGlow.h>
#include <SelectionOutline.h>
#include <Services/Vats76Service.h>
#include <World.h>
#include <WorldRays.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

// Better Interact, based on Zzyxzz's Smart Selection (Nexus 109243), rebuilt in our own code: when the crosshair finds
// nothing, the reference the player most likely means is picked instead (by angle from the crosshair, distance, kind of
// item, value, in line of sight, within the view's distance and width), and gets an outline (SelectionOutline.cpp), or,
// with the Analytical Eye perk (RD_Perks.esp, given at Intelligence 6), the glow VATS shows its targets with
// (SelectionGlow.cpp). What the crosshair finds itself is never replaced. With Zzyxzz's Smart Selection installed ours
// stays out of the way. The settings keep their [SmartSelection*] INI sections so what players saved keeps working.

namespace
{
// Reclamation Day ids are CommonLibF4's + 1 (VersionDb::LoadFallout4)

// The ViewCaster's pick (every frame) hands its pick to the setter here
constexpr uint32_t kViewCasterPick = 795750;
constexpr uint32_t kViewCasterPickSetterCall = 0x19BC;
constexpr uint32_t kViewCasterSetPick = 725909;
// Makes the pick record from the raw pick (the handle and where the crosshair hit): what stealing it would be, etc.
constexpr uint32_t kBuildPickRecord = 1423598;
// PlayerCamera*: its current state at 0x28, its states from 0xE0 (first person first, third person ninth)
constexpr uint32_t kPlayerCamera = 1171981;
constexpr size_t kCameraCurrentState = 0x28;
constexpr size_t kCameraFirstPersonState = 0xE0;
constexpr size_t kCameraThirdPersonState = 0x120;
// TESObjectREFR: activation blocked (a keyword or its extra data), its activate text ("Take", "Open", ...)
constexpr uint32_t kIsActivationBlocked = 407610;
constexpr uint32_t kGetActivateText = 1077222;
// ExtraDataList's instance data of an item (or null), TESValueForm::GetFormValue
constexpr uint32_t kGetInstanceData = 1280131;
constexpr uint32_t kGetFormValue = 885784;
// The Analytical Eye perk (RD_Perks.esp, Tools/BuildPerksPlugin.js): given once the player's Intelligence (permanent
// value: points, bobbleheads, the book, no chems) reaches this; while they have it, the glow instead of the outline
constexpr const char* kPerksPlugin = "RD_Perks.esp";
constexpr uint32_t kAnalyticalEye = 0x800;
constexpr float kAnalyticalEyeIntelligence = 6.f;
constexpr float kPerkInterval = 1.f;

// Virtual functions by their slot (CommonLibF4's TESObjectREFR / TESForm / TESBoundObject)
constexpr size_t kGetActorHeightOrRefBound = 0x68;
constexpr size_t kGet3D = 0x8C;
constexpr size_t kIsProjectile = 0xB5;
constexpr size_t kGetPlayable = 0x1D;
constexpr size_t kGetDefaultInstanceData = 0x57;
// NiAVObject: parent, world rotation (rows of 4 floats), translation, scale, world bound (centre, radius)
constexpr size_t kNodeParent = 0x28;
constexpr size_t kWorldRotate = 0x70;
constexpr size_t kWorldTranslate = 0xA0;
constexpr size_t kWorldScale = 0xAC;
constexpr size_t kWorldBound = 0xB0;
// TESBoundObject's bounds (int16 min x y z, max x y z); a light's flags (bit 1: can be carried); a moving projectile's
// velocity
constexpr size_t kBoundsMin = 0x20;
constexpr size_t kBoundsMax = 0x26;
constexpr size_t kLightFlags = 0x154;
constexpr size_t kActivatorWater = 0x138;
constexpr size_t kProjectileVelocity = 0x18C;

// TESForm flags
constexpr uint32_t kFormDeleted = 0x20;
constexpr uint32_t kFormInitialized = 0x8;
constexpr uint32_t kFormDisabled = 0x800;
constexpr uint32_t kFormHarvested = 0x2000;
// Disintegrated characters (their ash pile is what's looted)
constexpr auto kExtraAshPileRef = static_cast<ExtraDataType>(0x85);
constexpr auto kExtraTextDisplayData = static_cast<ExtraDataType>(0x99);

// Zzyxzz's Smart Selection's builds: with either installed, ours stay out of the way
constexpr const wchar_t* kSmartSelectionDlls[] = {L"SmartSelectionRD.dll", L"SmartSelection.dll"};
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr float kSettingsInterval = 1.f;
// No picking or outline while one of these is open (VATS and the workshop show their own targets)
constexpr const char* kStandDownMenus[] = {"VATSMenu", "WorkshopMenu", "LoadingMenu", "MainMenu", "PauseMenu", "PipboyMenu", "TerminalMenu",
                                           "BookMenu", "FavoritesMenu", "SleepWaitMenu", "LevelUpMenu", "DialogueMenu", "LooksMenu", "SPECIALMenu",
                                           "HolotapeMenu", "CookingMenu", "RobotModMenu", "PowerArmorModMenu", "ContainerMenu", "BarterMenu",
                                           "ExamineMenu", "LockpickingMenu", "Console"};

// Exterior cells are this big
constexpr float kCellSize = 4096.f;
constexpr float kTiny = 1e-4f;
// How many of the best get the line of sight check (the original: the first two), and its rays across the target
constexpr size_t kLineOfSightChecks = 2;
constexpr float kRaySpread = 0.6f;

// Kinds (the original's), from the base form's type; the groups the switches are for
enum Kind : uint8_t
{
    kMisc = 0,
    kActivator = 1,
    kCreature = 2,
    kNpc = 3,
    kContainer = 4,
    kIngredient = 5,
    kFlora = 6,
    kTree = 7,
    kWeapon = 8,
    kDoor = 9,
    kFurniture = 10,
    kAid = 11,
    kBook = 12,
    kArmor = 13,
    kAmmo = 14,
    kNote = 15,
    kScroll = 16,
    kSoulGem = 17,
    kSpell = 18,
    kKey = 19,
    kTalkingActivator = 20,
    kProjectile = 22,
    kLight = 23,
    kUnsupported = 24,
};
enum Group : uint8_t
{
    kActivatables,
    kItems,
    kNpcs,
    kContainers,
    kGroupCount
};

// Preferred kinds first (cost = rank / 24): keys, notes, scrolls, ammo, ingredients, aid, weapons, armour ... doors
constexpr std::array<float, 25> kKindRank = {18, 13, 16, 17, 19, 6, 14, 15, 8, 21, 20, 7, 12, 9, 5, 2, 3, 4, 10, 1, 22, 23, 0, 11, 0};

Group GroupOf(Kind aKind) noexcept
{
    switch (aKind)
    {
    case kActivator:
    case kFlora:
    case kTree:
    case kDoor:
    case kFurniture:
    case kTalkingActivator: return kActivatables;
    case kCreature:
    case kNpc: return kNpcs;
    case kContainer: return kContainers;
    default: return kItems;
    }
}

template <class TResult, class... TArgs> TResult CallVirtual(const void* apObject, size_t aSlot, TArgs... aArgs) noexcept
{
    using TFunction = TResult(const void*, TArgs...);
    return (*reinterpret_cast<TFunction* const* const*>(apObject))[aSlot](apObject, aArgs...);
}

template <class T> T Read(const void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(apObject) + aOffset);
}

bool Finite(float aValue) noexcept
{
    return std::isfinite(aValue);
}

// A base form's kind; actors by their base (an NPC or a creature)
Kind KindOf(const TESObjectREFR* apRef) noexcept
{
    const TESForm* pBase = apRef->baseForm;
    if (static_cast<uint8_t>(apRef->formType) == 0x41)
        return pBase && static_cast<uint8_t>(pBase->formType) == 0x2D ? kNpc : kCreature;
    if (!pBase)
        return kUnsupported;

    switch (static_cast<uint8_t>(pBase->formType))
    {
    case 0x19: return kSpell;
    case 0x1A: return kScroll;
    case 0x1B: return Read<const void*>(pBase, kActivatorWater) ? kUnsupported : kActivator;
    case 0x1C: return kTalkingActivator;
    case 0x1D: return kArmor;
    case 0x1E: return kBook;
    case 0x1F: return kContainer;
    case 0x20: return kDoor;
    case 0x21: return kIngredient;
    case 0x22: return kLight;
    case 0x23: return kMisc;
    case 0x28: return kTree;
    case 0x29: return kFlora;
    case 0x2A: return kFurniture;
    case 0x2B: return kWeapon;
    case 0x2C: return kAmmo;
    case 0x2F: return kKey;
    case 0x30: return kAid;
    case 0x32: return kNote;
    case 0x33: return kProjectile;
    case 0x36: return kSoulGem;
    default: return kUnsupported;
    }
}

// Items picked up into the inventory (the playable check applies)
bool IsInventoryType(uint8_t aType) noexcept
{
    switch (aType)
    {
    case 0x19:
    case 0x1A:
    case 0x1D:
    case 0x1E:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x2B:
    case 0x2C:
    case 0x2F:
    case 0x30:
    case 0x32:
    case 0x36: return true;
    default: return false;
    }
}

// Forms with bounds (TESBoundObject)
bool IsBoundObjectType(uint8_t aType) noexcept
{
    return aType == 0x08 || aType == 0x09 || aType == 0x12 || aType == 0x13 || (aType >= 0x18 && aType <= 0x38) || aType == 0x55 || aType == 0x5A ||
           aType == 0x61 || aType == 0x81 || aType == 0x85;
}

// --- settings

struct ViewSettings
{
    bool Enabled = true;
    float Range = 150.f;
    float Width = 0.3f;
    float AngleWeight = 5.f;
    float DistanceWeight = 1.f;
    float TypeWeight = 0.1f;
    float ValueWeight = 0.6f;
    int ValueCap = 200;
    float PreviousBonus = 0.1f;
    float NpcBonus = 1.5f;
    bool Outline = true;
    int OutlineColour = 0;
    int OutlineWidth = 2;
    float OutlineOpacity = 0.85f;
};

struct Settings
{
    // Reclamation Day's choices: both views; activators, items and containers (the bike's seat and saddlebags), no
    // characters
    std::array<ViewSettings, 2> Views{};
    std::array<bool, kGroupCount> Select{true, true, false, true};
    std::array<bool, kGroupCount> Outline{true, true, false, true};
};

Settings s_settings{};
bool s_settingsLoaded = false;
std::filesystem::file_time_type s_settingsTime{};
std::chrono::steady_clock::time_point s_settingsChecked{};

struct IniFile
{
    std::wstring path;

    [[nodiscard]] float Float(const wchar_t* acpSection, const wchar_t* acpKey, float aDefault) const noexcept
    {
        wchar_t text[32]{};
        GetPrivateProfileStringW(acpSection, acpKey, L"", text, static_cast<DWORD>(std::size(text)), path.c_str());
        wchar_t* pEnd = nullptr;
        const float value = std::wcstof(text, &pEnd);
        return pEnd != text && std::isfinite(value) ? value : aDefault;
    }
    [[nodiscard]] bool Bool(const wchar_t* acpSection, const wchar_t* acpKey, bool aDefault) const noexcept { return Float(acpSection, acpKey, aDefault ? 1.f : 0.f) != 0.f; }
    [[nodiscard]] int Int(const wchar_t* acpSection, const wchar_t* acpKey, int aDefault) const noexcept
    {
        return static_cast<int>(Float(acpSection, acpKey, static_cast<float>(aDefault)));
    }

    void Read(const wchar_t* acpSection, ViewSettings& aView) const noexcept
    {
        aView.Range = std::clamp(Float(acpSection, L"fSelectionRange", aView.Range), 40.f, 1000.f);
        aView.Width = std::clamp(Float(acpSection, L"fMaxAngleDifference", aView.Width), 0.05f, 2.f);
        aView.AngleWeight = std::clamp(Float(acpSection, L"fAngleWeight", aView.AngleWeight), 0.f, 10.f);
        aView.DistanceWeight = std::clamp(Float(acpSection, L"fDistanceWeight", aView.DistanceWeight), 0.f, 10.f);
        aView.TypeWeight = std::clamp(Float(acpSection, L"fTypeWeight", aView.TypeWeight), 0.f, 10.f);
        aView.ValueWeight = std::clamp(Float(acpSection, L"fItemValueWeight", aView.ValueWeight), 0.f, 10.f);
        aView.ValueCap = std::clamp(Int(acpSection, L"iValueNormalizationCap", aView.ValueCap), 1, 2000);
        aView.PreviousBonus = std::clamp(Float(acpSection, L"fPreviousSelectionBonus", aView.PreviousBonus), -2.f, 5.f);
        aView.NpcBonus = std::clamp(Float(acpSection, L"fNPCSelectionBonus", aView.NpcBonus), -2.f, 5.f);
        aView.Outline = Bool(acpSection, L"bOutlineSelectedTarget", aView.Outline);
        aView.OutlineColour = std::clamp(Int(acpSection, L"iOutlineColor", aView.OutlineColour), 0, 6);
        aView.OutlineWidth = std::clamp(Int(acpSection, L"iOutlineWidth", aView.OutlineWidth), 1, 4);
        aView.OutlineOpacity = std::clamp(Float(acpSection, L"fOutlineOpacity", aView.OutlineOpacity), 0.f, 1.f);
    }
};

void UpdateSettings() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (s_settingsLoaded && now - s_settingsChecked < std::chrono::duration<float>(kSettingsInterval))
        return;
    s_settingsChecked = now;

    // the game's folder, as the script extender is found (ScriptExtender.cpp)
    const auto path = std::filesystem::current_path() / L"Data" / kSettingsFile;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    const bool there = !ec;
    if (s_settingsLoaded && (there ? time : std::filesystem::file_time_type{}) == s_settingsTime)
        return;
    s_settingsTime = there ? time : std::filesystem::file_time_type{};

    Settings settings{};
    if (there)
    {
        const IniFile ini{path.wstring()};
        settings.Views[0].Enabled = ini.Bool(L"SmartSelection", L"bEnableFirstPerson", settings.Views[0].Enabled);
        settings.Views[1].Enabled = ini.Bool(L"SmartSelection", L"bEnableThirdPerson", settings.Views[1].Enabled);
        constexpr std::array<const wchar_t*, kGroupCount> kSelect = {L"bSelectActivatables", L"bSelectItems", L"bSelectNPCs", L"bSelectContainers"};
        constexpr std::array<const wchar_t*, kGroupCount> kOutline = {L"bOutlineActivatables", L"bOutlineItems", L"bOutlineNPCs", L"bOutlineContainers"};
        for (size_t i = 0; i < kGroupCount; ++i)
        {
            settings.Select[i] = ini.Bool(L"SmartSelection", kSelect[i], settings.Select[i]);
            settings.Outline[i] = ini.Bool(L"SmartSelection", kOutline[i], settings.Outline[i]);
        }
        ini.Read(L"SmartSelection1P", settings.Views[0]);
        ini.Read(L"SmartSelection3P", settings.Views[1]);
    }
    s_settings = settings;
    s_settingsLoaded = true;
    spdlog::info("Better Interact: settings {} (first person {}, third person {}, select {}{}{}{}, outline {}{}{}{})", there ? "from MCM" : "default",
                 settings.Views[0].Enabled, settings.Views[1].Enabled, settings.Select[kActivatables] ? "A" : "-", settings.Select[kItems] ? "I" : "-",
                 settings.Select[kNpcs] ? "N" : "-", settings.Select[kContainers] ? "C" : "-", settings.Outline[kActivatables] ? "A" : "-",
                 settings.Outline[kItems] ? "I" : "-", settings.Outline[kNpcs] ? "N" : "-", settings.Outline[kContainers] ? "C" : "-");
}

// --- the game

bool IsMenuOpen(const BSFixedString& acMenu) noexcept
{
    UI* pUI = UI::Get();
    return pUI && pUI->GetMenuOpen(acMenu);
}

bool IsStandingDownForMenus() noexcept
{
    static const auto s_menus = []
    {
        std::vector<std::unique_ptr<BSFixedString>> menus;
        for (const char* pName : kStandDownMenus)
            menus.push_back(std::make_unique<BSFixedString>(pName));
        return menus;
    }();
    return std::any_of(s_menus.begin(), s_menus.end(), [](const auto& acpMenu) { return IsMenuOpen(*acpMenu); });
}

// 0 first person, 1 third person, -1 any other camera (VATS, furniture, transitions...)
int CameraView() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, kPlayerCamera);
    const uint8_t* pCamera = *s_playerCamera.Get();
    if (!pCamera)
        return -1;
    const void* pCurrent = Read<const void*>(pCamera, kCameraCurrentState);
    if (!pCurrent)
        return -1;
    if (pCurrent == Read<const void*>(pCamera, kCameraFirstPersonState))
        return 0;
    if (pCurrent == Read<const void*>(pCamera, kCameraThirdPersonState))
        return 1;
    return -1;
}

const uint8_t* Get3D(const TESObjectREFR* apRef) noexcept
{
    return static_cast<const uint8_t*>(CallVirtual<void*>(apRef, kGet3D));
}

void* InstanceDataOf(const TESObjectREFR* apRef, const TESForm* apBase) noexcept
{
    if (apRef->extraData)
    {
        using TGetInstanceData = void*(ExtraDataList*, const TESForm*, bool);
        POINTER_FALLOUT4(TGetInstanceData, s_getInstanceData, kGetInstanceData);
        if (void* pData = s_getInstanceData.Get()(apRef->extraData, apBase, false))
            return pData;
    }
    return CallVirtual<void*>(apBase, kGetDefaultInstanceData);
}

// Whether the reference has an activate text ("Take", "Open", ...): the game writes it into a BSStringT<char> (text on
// the game's heap, length, capacity)
bool HasActivateText(TESObjectREFR* apRef) noexcept
{
    struct Text
    {
        char* pData = nullptr;
        uint16_t Length = 0;
        uint16_t Capacity = 0;
    };
    using TGetActivateText = bool(TESObjectREFR*, Text&);
    POINTER_FALLOUT4(TGetActivateText, s_getActivateText, kGetActivateText);

    Text text{};
    const bool has = s_getActivateText.Get()(apRef, text) && text.pData && *text.pData;
    if (text.pData)
        Memory::Free(text.pData);
    return has;
}

// The view's tangents of a point (right / ahead, up / ahead); false behind the camera
bool ViewTangents(const glm::vec3& acPoint, float& aX, float& aY) noexcept
{
    float right = 0.f, up = 0.f, ahead = 0.f;
    if (!HudMarker::WorldToView(NiPoint3(acPoint), right, up, ahead) || !(ahead > kTiny))
        return false;
    aX = right / ahead;
    aY = up / ahead;
    return Finite(aX) && Finite(aY);
}

// An angle off the crosshair as the original measures it: 2 sin(atan(tangent) / 2)
float Chord(float aTangent) noexcept
{
    return 2.f * std::sin(std::atan(aTangent) * 0.5f);
}

// A reference's box in the world: from its base's bounds and its 3D's transform; the bound sphere's centre (no extent)
// when it has no bounds
struct Box
{
    glm::vec3 Centre{};
    std::array<glm::vec3, 3> Axes{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)};
    glm::vec3 Half{};
};

Box MakeBox(const TESForm* apBase, const uint8_t* ap3D) noexcept
{
    Box box{};
    const float* pBound = reinterpret_cast<const float*>(ap3D + kWorldBound);
    box.Centre = glm::vec3(pBound[0], pBound[1], pBound[2]);
    if (!apBase || !IsBoundObjectType(static_cast<uint8_t>(apBase->formType)))
        return box;

    const auto* pMin = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(apBase) + kBoundsMin);
    const auto* pMax = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(apBase) + kBoundsMax);
    const float scale = Read<float>(ap3D, kWorldScale);
    if (!Finite(scale) || std::abs(scale) <= kTiny)
        return box;

    // the game keeps the rotation transposed: its row i is where the local axis i points in the world
    const float* pRotate = reinterpret_cast<const float*>(ap3D + kWorldRotate);
    const auto row = [pRotate](int aRow) { return glm::vec3(pRotate[aRow * 4], pRotate[aRow * 4 + 1], pRotate[aRow * 4 + 2]); };
    const glm::vec3 local((pMin[0] + pMax[0]) * 0.5f * scale, (pMin[1] + pMax[1]) * 0.5f * scale, (pMin[2] + pMax[2]) * 0.5f * scale);
    const float* pTranslate = reinterpret_cast<const float*>(ap3D + kWorldTranslate);
    const glm::vec3 centre = row(0) * local.x + row(1) * local.y + row(2) * local.z + glm::vec3(pTranslate[0], pTranslate[1], pTranslate[2]);

    std::array<glm::vec3, 3> axes{};
    for (int i = 0; i < 3; ++i)
    {
        const float length = glm::length(row(i));
        if (!Finite(length) || length <= kTiny)
            return box;
        axes[i] = row(i) / length;
    }
    if (!Finite(centre.x) || !Finite(centre.y) || !Finite(centre.z))
        return box;

    box.Centre = centre;
    box.Axes = axes;
    box.Half = glm::vec3(std::abs(pMax[0] - pMin[0]), std::abs(pMax[1] - pMin[1]), std::abs(pMax[2] - pMin[2])) * 0.5f * std::abs(scale);
    return box;
}

// How far from the point the box is (0 inside)
float DistanceTo(const Box& acBox, const glm::vec3& acPoint) noexcept
{
    const glm::vec3 offset = acPoint - acBox.Centre;
    glm::vec3 closest = acBox.Centre;
    for (int i = 0; i < 3; ++i)
        closest += acBox.Axes[i] * std::clamp(glm::dot(offset, acBox.Axes[i]), -acBox.Half[i], acBox.Half[i]);
    return glm::length(acPoint - closest);
}

// How far off the crosshair the box is (0 when the crosshair is on it), and its centre
bool AnglesTo(const Box& acBox, float& aEdge, float& aCentre) noexcept
{
    float cx = 0.f, cy = 0.f;
    if (!ViewTangents(acBox.Centre, cx, cy))
        return false;
    aCentre = Chord(std::sqrt(cx * cx + cy * cy));
    aEdge = aCentre;

    float minX = cx, maxX = cx, minY = cy, maxY = cy;
    for (int corner = 0; corner < 8; ++corner)
    {
        glm::vec3 point = acBox.Centre;
        for (int i = 0; i < 3; ++i)
            point += acBox.Axes[i] * (corner & (1 << i) ? acBox.Half[i] : -acBox.Half[i]);
        float x = 0.f, y = 0.f;
        // a corner behind the camera: the centre's angle, as the original
        if (!ViewTangents(point, x, y))
            return true;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    const float nearX = std::clamp(0.f, minX, maxX);
    const float nearY = std::clamp(0.f, minY, maxY);
    aEdge = Chord(std::sqrt(nearX * nearX + nearY * nearY));
    return true;
}

// --- the decision

struct Counters
{
    uint32_t Refs = 0, Invalid = 0, Unsupported = 0, State = 0, Special = 0, Geometry = 0, Interaction = 0, Accepted = 0;
    uint32_t InRange = 0, InWidth = 0, Checked = 0, Visible = 0;
};

struct Candidate
{
    TESObjectREFR* pRef = nullptr;
    const uint8_t* p3D = nullptr;
    uint32_t Handle = 0;
    Kind Type = kUnsupported;
    Box Bounds{};
    float Distance = 0.f;
    float Angle = 0.f;
    float Score = 0.f;
};

enum class Outcome : uint8_t
{
    kNone,
    kOff,
    kPlayer,
    kCamera,
    kNoCandidate,
    kNotVisible,
    kNative,
    kSelected,
};
constexpr const char* kOutcomeNames[] = {"none", "off", "player", "camera", "no candidate", "not visible", "native", "selected"};

uint32_t s_previousHandle = 0;

// Why a reference can't be used (the original's per reference checks, by the counter they count for)
enum class Unusable : uint8_t
{
    kNo,
    kInvalid,
    kUnsupported,
    kState,
    kSpecial,
    kInteraction,
};

// The checks before its 3D's: a real reference of a kind that can be used, not disabled, blocked or harvested
Unusable CheckState(TESObjectREFR* apRef, Kind aKind) noexcept
{
    const uint32_t flags = apRef->flags;
    const uint32_t formId = apRef->formID;
    const TESForm* pBase = apRef->baseForm;
    // (the game's activation checks below read the extra data unchecked)
    if (formId == 0 || formId == 0xFFFFFFFF || (flags & kFormDeleted) || !(flags & kFormInitialized) || formId == 0x7 || formId == 0x14 || !pBase ||
        !apRef->extraData)
        return Unusable::kInvalid;
    if (aKind == kUnsupported)
        return Unusable::kUnsupported;

    using TIsActivationBlocked = bool(TESObjectREFR*);
    POINTER_FALLOUT4(TIsActivationBlocked, s_isActivationBlocked, kIsActivationBlocked);
    const bool actor = static_cast<uint8_t>(apRef->formType) == 0x41;
    if ((flags & kFormDisabled) || s_isActivationBlocked.Get()(apRef) || ((aKind == kFlora || aKind == kTree) && (flags & kFormHarvested)) ||
        (actor && apRef->extraData->Contains(kExtraAshPileRef)))
        return Unusable::kState;

    // lights only if they can be carried; projectiles only at rest
    if (aKind == kLight && !(Read<uint8_t>(pBase, kLightFlags) & 2))
        return Unusable::kSpecial;
    if (aKind == kProjectile)
    {
        const void* pProjectile = CallVirtual<const void*>(apRef, kIsProjectile);
        const float* pVelocity = pProjectile ? reinterpret_cast<const float*>(static_cast<const uint8_t*>(pProjectile) + kProjectileVelocity) : nullptr;
        if (pVelocity && !(Finite(pVelocity[0]) && Finite(pVelocity[1]) && Finite(pVelocity[2]) && std::abs(pVelocity[0]) <= 1e-3f &&
                           std::abs(pVelocity[1]) <= 1e-3f && std::abs(pVelocity[2]) <= 1e-3f))
            return Unusable::kSpecial;
    }
    return Unusable::kNo;
}

// The checks after: a bound object, playable, with a name and something to do with it
Unusable CheckInteraction(TESObjectREFR* apRef) noexcept
{
    const TESForm* pBase = apRef->baseForm;
    const uint8_t baseType = static_cast<uint8_t>(pBase->formType);
    if (!IsBoundObjectType(baseType))
        return Unusable::kInteraction;
    if (IsInventoryType(baseType) && !CallVirtual<bool>(pBase, kGetPlayable, InstanceDataOf(apRef, pBase)))
        return Unusable::kInteraction;
    // a name (its own, or the base's) and an activate text ("Take", "Open", ...)
    const bool named = apRef->extraData->Contains(kExtraTextDisplayData) || (pBase->GetName() && *pBase->GetName());
    if (!named || !HasActivateText(apRef))
        return Unusable::kInteraction;
    return Unusable::kNo;
}

// Whether the player can do something with a reference (what the crosshair finds itself can be anything with collision:
// walls, rocks, set dressing)
bool IsUsable(TESObjectREFR* apRef) noexcept
{
    return apRef && CheckState(apRef, KindOf(apRef)) == Unusable::kNo && CheckInteraction(apRef) == Unusable::kNo;
}

void Count(Unusable aReason, Counters& aCounters) noexcept
{
    switch (aReason)
    {
    case Unusable::kInvalid: ++aCounters.Invalid; break;
    case Unusable::kUnsupported: ++aCounters.Unsupported; break;
    case Unusable::kState: ++aCounters.State; break;
    case Unusable::kSpecial: ++aCounters.Special; break;
    case Unusable::kInteraction: ++aCounters.Interaction; break;
    default: break;
    }
}

// Whether a reference can be picked at all (the original's per reference checks), with its kind
bool Accept(TESObjectREFR* apRef, const glm::vec3& acOrigin, const ViewSettings& acView, Candidate& aOut, Counters& aCounters) noexcept
{
    const Kind kind = KindOf(apRef);
    Unusable reason = CheckState(apRef, kind);
    if (reason == Unusable::kNo && !s_settings.Select[GroupOf(kind)])
        reason = Unusable::kUnsupported;
    if (reason != Unusable::kNo)
    {
        Count(reason, aCounters);
        return false;
    }

    const uint8_t* p3D = Get3D(apRef);
    const float* pBound = p3D ? reinterpret_cast<const float*>(p3D + kWorldBound) : nullptr;
    if (!pBound || !Finite(pBound[0]) || !Finite(pBound[1]) || !Finite(pBound[2]) || !Finite(pBound[3]) || pBound[3] < 0.f ||
        glm::length(glm::vec3(pBound[0], pBound[1], pBound[2]) - acOrigin) - pBound[3] > acView.Range)
    {
        ++aCounters.Geometry;
        return false;
    }

    if (const Unusable interaction = CheckInteraction(apRef); interaction != Unusable::kNo)
    {
        Count(interaction, aCounters);
        return false;
    }

    const uint32_t handle = apRef->GetHandle().handle.iBits;
    if (!handle)
    {
        ++aCounters.Invalid;
        return false;
    }
    const TESForm* pBase = apRef->baseForm;

    ++aCounters.Accepted;
    aOut.pRef = apRef;
    aOut.p3D = p3D;
    aOut.Handle = handle;
    aOut.Type = kind;
    aOut.Bounds = MakeBox(pBase, p3D);
    return true;
}

// The cost (lower wins) of a candidate
float Score(const Candidate& acCandidate, const ViewSettings& acView) noexcept
{
    using TGetFormValue = int32_t(const TESForm*, const void*);
    POINTER_FALLOUT4(TGetFormValue, s_getFormValue, kGetFormValue);

    // characters have no value: the most a value can cost, as the original's unvalued
    float valueCost = 1.f;
    if (acCandidate.Type != kNpc && acCandidate.Type != kCreature)
    {
        const TESForm* pBase = acCandidate.pRef->baseForm;
        const int32_t value = s_getFormValue.Get()(pBase, InstanceDataOf(acCandidate.pRef, pBase));
        if (value >= 0)
            valueCost = std::clamp(1.f - static_cast<float>(value) / static_cast<float>(acView.ValueCap), 0.f, 1.f);
    }
    float score = acView.AngleWeight * acCandidate.Angle / acView.Width + acView.DistanceWeight * acCandidate.Distance / acView.Range +
                  acView.TypeWeight * kKindRank[acCandidate.Type] / 24.f + acView.ValueWeight * valueCost;
    if (acCandidate.Handle == s_previousHandle)
        score -= acView.PreviousBonus;
    if (acCandidate.Type == kNpc)
        score -= acView.NpcBonus;
    return score;
}

// Whether the player sees any of the target (rays from their middle to its box: none of them stopped before it)
bool InLineOfSight(PlayerCharacter* apPlayer, TESObjectCELL* apCell, const glm::vec3& acOrigin, const Candidate& acCandidate) noexcept
{
    const Box& box = acCandidate.Bounds;
    const uint32_t filter = WorldRays::GetLineOfSightFilter(apPlayer);
    const glm::vec3 offset = box.Centre - acOrigin;
    const glm::vec3 toward = glm::length(offset) > kTiny ? glm::normalize(offset) : glm::vec3(0.f, 0.f, 1.f);
    // across the two box sides that face the player the most
    std::array<int, 3> order = {0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return std::abs(glm::dot(box.Axes[a], toward)) < std::abs(glm::dot(box.Axes[b], toward)); });
    const glm::vec3 across0 = box.Axes[order[0]] * box.Half[order[0]] * kRaySpread;
    const glm::vec3 across1 = box.Axes[order[1]] * box.Half[order[1]] * kRaySpread;

    for (const float u : {0.f, -1.f, 1.f})
    {
        for (const float v : {0.f, -1.f, 1.f})
        {
            const glm::vec3 point = box.Centre + across0 * u + across1 * v;
            const WorldRays::RayHit hit = WorldRays::PickOnce(apCell, filter, NiPoint3(acOrigin), NiPoint3(point));
            if (!hit.Hit)
                return true;
            for (const uint8_t* pNode = static_cast<const uint8_t*>(hit.pObject); pNode; pNode = Read<const uint8_t*>(pNode, kNodeParent))
            {
                if (pNode == acCandidate.p3D)
                    return true;
            }
        }
    }
    return false;
}

// A cell with its references' 3D in the world (attached, loaded, not on its way out)
bool IsAttached(const TESObjectCELL* apCell) noexcept
{
    return apCell && apCell->IsValid() && Read<uint8_t>(apCell, 0x46) == 0 && Read<const void*>(apCell, 0xD0);
}

// The cells to look in: the player's (inside), or the loaded ones around them (outside)
std::vector<TESObjectCELL*> CellsAround(PlayerCharacter* apPlayer, float aRange) noexcept
{
    std::vector<TESObjectCELL*> cells;
    TESObjectCELL* pCell = apPlayer->parentCell;
    if (!pCell)
        return cells;
    TESWorldSpace* pWorld = pCell->worldspace;
    if (!pWorld || (pCell->cellFlags[0] & 1))
    {
        cells.push_back(pCell);
        return cells;
    }

    const int radius = std::max(1, static_cast<int>(std::ceil(aRange / kCellSize)));
    const int x = static_cast<int>(std::floor(apPlayer->position.x / kCellSize));
    const int y = static_cast<int>(std::floor(apPlayer->position.y / kCellSize));
    for (int dx = -radius; dx <= radius; ++dx)
    {
        for (int dy = -radius; dy <= radius; ++dy)
        {
            if (TESObjectCELL* pAround = ModManager::Get()->GetCellFromCoordinates(x + dx, y + dy, pWorld, false))
                cells.push_back(pAround);
        }
    }
    return cells;
}

struct Decision
{
    Outcome Result = Outcome::kNone;
    int View = -1;
    // a menu that stands us down is open
    bool Menu = false;
    Counters Counts{};
    std::array<std::pair<uint32_t, float>, 3> Ranked{};
    uint32_t Native = 0;
    TESObjectREFR* pChosen = nullptr;
    uint32_t Chosen = 0;
};

Decision Decide(uint32_t aNativeHandle) noexcept
{
    Decision decision{};
    decision.Native = aNativeHandle;
    decision.View = CameraView();
    // RD VATS 76' has the glow for its picked part, as VATS has it in its menu
    decision.Menu = IsStandingDownForMenus() || Vats76Service::IsActive();
    if (decision.View < 0 || !s_settings.Views[decision.View].Enabled || decision.Menu)
    {
        decision.Result = Outcome::kOff;
        return decision;
    }
    const ViewSettings& view = s_settings.Views[decision.View];

    if (aNativeHandle)
    {
        decision.Result = Outcome::kNative;
        decision.pChosen = TESObjectREFR::GetByHandle(aNativeHandle);
        decision.Chosen = aNativeHandle;
        return decision;
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->parentCell : nullptr;
    if (!IsAttached(pCell))
    {
        decision.Result = Outcome::kPlayer;
        return decision;
    }
    // the world camera has to be there (the angles come from it)
    const float height = CallVirtual<float>(pPlayer, kGetActorHeightOrRefBound);
    NiPoint3 camera{};
    if (!Finite(height) || height <= kTiny || !HudMarker::GetCameraPosition(camera))
    {
        decision.Result = Outcome::kCamera;
        return decision;
    }
    const glm::vec3 origin = glm::vec3(pPlayer->position) + glm::vec3(0.f, 0.f, height * 0.5f);

    std::vector<Candidate> candidates;
    for (TESObjectCELL* pAround : CellsAround(pPlayer, view.Range))
    {
        if (!IsAttached(pAround))
            continue;
        for (uint32_t i = 0; i < pAround->objects.length; ++i)
        {
            TESObjectREFR* pRef = pAround->objects[i];
            if (!pRef)
                continue;
            ++decision.Counts.Refs;
            Candidate candidate{};
            if (!Accept(pRef, origin, view, candidate, decision.Counts))
                continue;
            candidate.Distance = DistanceTo(candidate.Bounds, origin);
            float edge = 0.f, centre = 0.f;
            if (!AnglesTo(candidate.Bounds, edge, centre) || candidate.Distance > view.Range)
                continue;
            ++decision.Counts.InRange;
            if (edge > view.Width)
                continue;
            ++decision.Counts.InWidth;
            candidate.Angle = edge + 0.25f * centre;
            candidate.Score = Score(candidate, view);
            candidates.push_back(candidate);
        }
    }
    if (candidates.empty())
    {
        decision.Result = Outcome::kNoCandidate;
        return decision;
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b)
                     {
                         if (a.Score != b.Score)
                             return a.Score < b.Score;
                         if (a.Distance != b.Distance)
                             return a.Distance < b.Distance;
                         return a.Angle < b.Angle;
                     });
    for (size_t i = 0; i < decision.Ranked.size() && i < candidates.size(); ++i)
        decision.Ranked[i] = {candidates[i].Handle, candidates[i].Score};

    for (size_t i = 0; i < candidates.size() && i < kLineOfSightChecks; ++i)
    {
        ++decision.Counts.Checked;
        if (!InLineOfSight(pPlayer, pCell, origin, candidates[i]))
            continue;
        ++decision.Counts.Visible;
        decision.Result = Outcome::kSelected;
        decision.pChosen = candidates[i].pRef;
        decision.Chosen = candidates[i].Handle;
        return decision;
    }
    decision.Result = Outcome::kNotVisible;
    return decision;
}

// --- the outline (SelectionOutline.cpp draws it, as long as it's set every pick)

SelectionOutline::Style OutlineStyle(const ViewSettings& acView) noexcept
{
    // HUD colour, red, gold, green, white, blue, pink
    glm::vec3 colour{};
    switch (acView.OutlineColour)
    {
    case 1: colour = {0.95f, 0.15f, 0.1f}; break;
    case 2: colour = {1.f, 0.75f, 0.2f}; break;
    case 3: colour = {0.2f, 0.95f, 0.3f}; break;
    case 4: colour = {1.f, 1.f, 1.f}; break;
    case 5: colour = {0.25f, 0.55f, 1.f}; break;
    case 6: colour = {1.f, 0.4f, 0.75f}; break;
    default:
    {
        const uint32_t hud = HudMarker::GetHudColour();
        colour = {((hud >> 16) & 0xFF) / 255.f, ((hud >> 8) & 0xFF) / 255.f, (hud & 0xFF) / 255.f};
        break;
    }
    }
    return {colour.r, colour.g, colour.b, acView.OutlineOpacity, acView.OutlineWidth};
}

// --- the Analytical Eye perk: given at Intelligence 6, then the glow instead of the outline

bool s_hasAnalyticalEye = false;
std::chrono::steady_clock::time_point s_perkChecked{};

// The perk's form ID (0 while RD_Perks.esp isn't loaded)
uint32_t AnalyticalEyeId() noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(kPerksPlugin);
    return pMod && pMod->IsLoaded() ? pMod->GetFormId(kAnalyticalEye) : 0;
}

void UpdatePerk() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - s_perkChecked < std::chrono::duration<float>(kPerkInterval))
        return;
    s_perkChecked = now;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const uint32_t perkId = AnalyticalEyeId();
    BGSPerk* pPerk = perkId ? Cast<BGSPerk>(TESForm::GetById(perkId)) : nullptr;
    if (!pPlayer || !pPerk)
    {
        s_hasAnalyticalEye = false;
        return;
    }
    s_hasAnalyticalEye = pPlayer->GetPerkRank(perkId) != 0;
    if (s_hasAnalyticalEye || pPlayer->GetActorPermanentValue(ActorValueInfo::kIntelligence) < kAnalyticalEyeIntelligence)
        return;

    // Actor.AddPerk(perk, notify): the game's own "perk added" note
    using TAddPerk = void(BSScript::IVirtualMachine*, uint32_t, Actor*, BGSPerk*, bool);
    static auto* s_addPerk = reinterpret_cast<TAddPerk*>(const_cast<void*>(World::Get().ctx().at<PapyrusService>().Get("Actor", "AddPerk")));
    if (!s_addPerk)
        return;
    s_addPerk(GameVM::Get()->virtualMachine, 0, pPlayer, pPerk, true);
    s_hasAnalyticalEye = pPlayer->GetPerkRank(perkId) != 0;
    spdlog::info("Better Interact: Intelligence {} reached, Analytical Eye given ({})", kAnalyticalEyeIntelligence, s_hasAnalyticalEye ? "has it" : "not taken");
}

void UpdateHighlight(const Decision& acDecision) noexcept
{
    // VATS and the workshop show their own targets: the glow is theirs now, ours is only forgotten
    if (acDecision.Menu)
    {
        SelectionOutline::Clear();
        SelectionGlow::Release();
        return;
    }
    // what we picked passed the checks already; what the crosshair is on still has to be something to use
    const ViewSettings* pView = acDecision.View >= 0 ? &s_settings.Views[acDecision.View] : nullptr;
    const bool usable = acDecision.Result == Outcome::kSelected || IsUsable(acDecision.pChosen);
    if (acDecision.Result == Outcome::kOff || !acDecision.pChosen || !usable || !pView || !pView->Outline ||
        !s_settings.Outline[GroupOf(KindOf(acDecision.pChosen))])
    {
        SelectionOutline::Clear();
        SelectionGlow::Clear();
        return;
    }

    const SelectionOutline::Style style = OutlineStyle(*pView);
    if (s_hasAnalyticalEye)
    {
        SelectionOutline::Clear();
        SelectionGlow::Set(acDecision.pChosen, acDecision.Chosen, style.Red * style.Opacity, style.Green * style.Opacity, style.Blue * style.Opacity);
    }
    else
    {
        SelectionGlow::Clear();
        SelectionOutline::Set(acDecision.Chosen, style);
    }
}

// --- diagnostics (RD_SMART_DEBUG=1): the last decision every 2 s, as the original's own diagnostic line

const bool s_debug = std::getenv("RD_SMART_DEBUG") != nullptr;
std::chrono::steady_clock::time_point s_lastReport{};

void Report(const Decision& acDecision) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (!s_debug || now - s_lastReport < std::chrono::seconds(2))
        return;
    s_lastReport = now;
    const Counters& c = acDecision.Counts;
    // the outline's meshes are the last drawn frame's
    const SelectionOutline::Stats outline = SelectionOutline::LastStats();
    spdlog::info("Better Interact: view {} outcome {} scan[refs {} invalid {} unsupported {} state {} special {} geometry {} interaction {} accepted {}] "
                 "pipeline[range {} width {} los {}/{}] native {:08X} chosen {:08X} ranked[{:08X}:{:.3f} {:08X}:{:.3f} {:08X}:{:.3f}] glow {:08X} "
                 "(perk {}) outline {:08X} drawn[{} static {} skinned {} skipped {} effects {}{}{}]",
                 acDecision.View, kOutcomeNames[static_cast<size_t>(acDecision.Result)], c.Refs, c.Invalid, c.Unsupported, c.State, c.Special, c.Geometry, c.Interaction,
                 c.Accepted, c.InRange, c.InWidth, c.Visible, c.Checked, acDecision.Native, acDecision.Chosen, acDecision.Ranked[0].first, acDecision.Ranked[0].second,
                 acDecision.Ranked[1].first, acDecision.Ranked[1].second, acDecision.Ranked[2].first, acDecision.Ranked[2].second, SelectionGlow::Current(),
                 s_hasAnalyticalEye, SelectionOutline::Current(),
                 outline.Drawn, outline.Static, outline.Skinned, outline.Skipped, outline.Effects, *outline.Stopped ? ": " : "", outline.Stopped);
}

// --- the hook

// The pick record (0x38 on 1.10.163): the handle, then what the raw pick had (where the crosshair hit), then what the
// game worked out of the reference (stealing it, ...)
struct PickRecord
{
    uint32_t Handle;
    uint8_t Hit[0x20];
    uint8_t Derived[0x14];
};
static_assert(sizeof(PickRecord) == 0x38);
constexpr size_t kRawPickSize = 0x24;

using TSetPick = bool(void* apViewCaster, const PickRecord* apRecord);
TSetPick* s_setPick = nullptr;

bool HookSetPick(void* apViewCaster, const PickRecord* apRecord)
{
    if (!apRecord)
        return s_setPick(apViewCaster, apRecord);

    UpdateSettings();
    UpdatePerk();
    const Decision decision = Decide(apRecord->Handle);
    UpdateHighlight(decision);
    Report(decision);

    // the current pick keeps a bonus only while it stays picked
    s_previousHandle = decision.Result == Outcome::kSelected ? decision.Chosen : 0;
    if (decision.Result != Outcome::kSelected)
        return s_setPick(apViewCaster, apRecord);

    // the game's own record of the chosen reference: its handle with the original's hit, the rest worked out for it
    std::array<uint8_t, kRawPickSize> raw{};
    std::memcpy(raw.data(), apRecord, raw.size());
    std::memcpy(raw.data(), &decision.Chosen, sizeof(decision.Chosen));
    using TBuildPickRecord = PickRecord*(PickRecord*, const void*);
    POINTER_FALLOUT4(TBuildPickRecord, s_buildPickRecord, kBuildPickRecord);
    PickRecord record{};
    s_buildPickRecord.Get()(&record, raw.data());
    return s_setPick(apViewCaster, &record);
}
} // namespace

static TiltedPhoques::Initializer s_betterInteractHooks(
    []()
    {
        // the game's folder, as the script extender is found (ScriptExtender.cpp)
        const auto plugins = std::filesystem::current_path() / L"Data" / L"F4SE" / L"Plugins";
        for (const wchar_t* pDll : kSmartSelectionDlls)
        {
            std::error_code ec;
            if (std::filesystem::exists(plugins / pDll, ec))
            {
                spdlog::info(L"Better Interact: off, Smart Selection ({}) is installed and does the same (remove it to use ours)", pDll);
                return;
            }
        }

        const VersionDbPtr<uint8_t> pick(kViewCasterPick);
        const VersionDbPtr<uint8_t> setter(kViewCasterSetPick);
        uint8_t* pSite = pick.Get() ? pick.Get() + kViewCasterPickSetterCall : nullptr;
        const intptr_t reach = reinterpret_cast<intptr_t>(&HookSetPick) - reinterpret_cast<intptr_t>(pSite + 5);
        if (!pSite || !setter.Get() || pSite[0] != 0xE8 || pSite + 5 + *reinterpret_cast<const int32_t*>(pSite + 1) != setter.Get() ||
            reach != static_cast<int32_t>(reach))
        {
            spdlog::warn("Better Interact: off, the crosshair pick's call isn't the expected one (another mod patched it?)");
            return;
        }

        s_setPick = reinterpret_cast<TSetPick*>(setter.Get());
        TiltedPhoques::PutCall(pSite, &HookSetPick);
        spdlog::info("Better Interact: on{}", s_debug ? " (logging its decisions)" : "");
    });

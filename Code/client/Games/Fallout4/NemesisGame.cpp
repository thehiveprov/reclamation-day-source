#include <Games/Fallout4/NemesisGame.h>

#if TP_FALLOUT4

#include <World.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <TESObjectREFR.h>
#include <Forms/TESNPC.h>
#include <Forms/TESRace.h>
#include <Games/ActorExtension.h>
#include <Forms/TESGlobal.h>
#include <Forms/TESFaction.h>
#include <Forms/BGSKeyword.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESPackage.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <Interface/UI.h>
#include <Interface/HudMarker.h>
#include <Components/TESFullName.h>
#include <SaveLoad.h>
#include <Services/PapyrusService.h>
#include <WorldRays.h>

#include <optional>
#include <random>

namespace
{
// RefrOrInventoryObj (PapyrusService.h, GameScript::RefrOrInventoryObj: the reference first, 0x18 bytes) is the "self"
// of ObjectReference natives bound for inventory objects too (PlaceActorAtMe, AddKeyword). Bigger than 8 bytes, so the
// native gets a pointer to it.
static_assert(sizeof(RefrOrInventoryObj) == 0x18);

// GameScript::FormOrInventoryObj (0x10, the exe's accessors read the form at 0 and the unique id at 8): RemoveItem's
// item. Passed by pointer as well.
struct FormOrInventoryObj
{
    TESForm* pForm;
    uint16_t UniqueId;
};
static_assert(sizeof(FormOrInventoryObj) == 0x10);

// Fallout4.esm
constexpr uint32_t kHumanRace = 0x13746;
constexpr uint32_t kGhoulRace = 0xEAFB6;
constexpr uint32_t kSuperMutantRace = 0x1A009;
constexpr uint32_t kSettlementKeyword = 0x83C9A; // LocTypeWorkshopSettlement
constexpr uint32_t kCaps = 0xF;
constexpr uint32_t kHealth = 0x2D4;
constexpr uint32_t kDamageResist = 0x2E3;
constexpr uint32_t kEnergyResist = 0x2EB;
constexpr uint32_t kSpeedMult = 0x2DA;
constexpr uint32_t kPerceptionCondition = 0x36C;
constexpr uint32_t kEnduranceCondition = 0x36D;
constexpr uint32_t kLeftAttackCondition = 0x36E;
constexpr uint32_t kRightAttackCondition = 0x36F;
constexpr uint32_t kLeftMobilityCondition = 0x370;
constexpr uint32_t kRightMobilityCondition = 0x371;
constexpr uint32_t kCharisma = 0x2C5;
// The game's holdup (the Intimidation perk's HoldupPacifySpell, holdupEffectScript): 1 while a man is held up
constexpr uint32_t kHoldupValue = 0xE7726;

// RD_Nemeses.esp (Tools/BuildNemesesPlugin.js): the quest whose script raises and lowers a man's hands, and the global
// its Interrogate perk sets
constexpr char kNemesesPlugin[] = "RD_Nemeses.esp";
constexpr uint32_t kTabQuest = 0x883;
constexpr uint32_t kInterrogateNow = 0x886;
constexpr char kTabScript[] = "ReclamationDay:NemesesTab";

// ACBS flags (TESActorBaseData)
constexpr uint64_t kFemale = 1 << 0;
constexpr uint64_t kEssential = 1 << 1;
constexpr uint64_t kUnique = 1 << 5;

// KARMA.esm (a light master): its Program quest (script KARM:Program), the player's karma and alignment globals, the
// actor alignment keywords (very evil .. very good) and value, the bounty hunter leader keyword
constexpr char kKarmaPlugin[] = "KARMA.esm";
constexpr uint32_t kKarmaQuest = 0x001;
constexpr uint32_t kKarmaGlobal = 0x10B;
constexpr uint32_t kKarmaAlignmentGlobal = 0xF9A;
constexpr uint32_t kKarmaAlignmentKeywords[5] = {0x105, 0x106, 0x107, 0x108, 0x109};
constexpr uint32_t kKarmaAlignmentValue = 0x171;
constexpr uint32_t kKarmaHunterLeaderKeyword = 0x02B;

constexpr uint32_t kQuestFormType = 80;

// NPC change flags a face is saved with: race, sex, face (the player's sync uses race and face)
constexpr uint32_t kFaceFlags = 0x2000000 | 0x1000000 | 0x800;

// A gang's factions (checked in this order: the specific ones before RaiderFaction) and what its men are spawned as by
// rank 1-5 (the game's legendary encounter NPC where it has one, for ranks 4-5)
struct FactionKey
{
    const char* Plugin;
    uint32_t Faction;
    const char* Gang;
};
const FactionKey kFactions[] = {
    {"DLCNukaWorld.esm", 0xF438, "disciples"},
    {"DLCNukaWorld.esm", 0xF439, "operators"},
    {"DLCNukaWorld.esm", 0xF43A, "pack"},
    {"DLCRobot.esm", 0x4385, "rustdevils"},
    {"DLCCoast.esm", 0x2DB4, "trappers"},
    {"Fallout4.esm", 0x13101D, "forged"},
    {"Fallout4.esm", 0x83AD0, "triggermen"},
    {"Fallout4.esm", 0x58303, "gunners"},
    {"Fallout4.esm", 0x58305, "mutants"},
    {"Fallout4.esm", 0x2FB84, "atom"},
    {"Fallout4.esm", 0x68043, "minutemen"},
    {"Fallout4.esm", 0x1CBED, "raiders"},
};

struct SpawnForms
{
    const char* Gang;
    const char* Plugin;
    std::array<uint32_t, 5> ByRank;
};
const SpawnForms kSpawns[] = {
    {"raiders", "Fallout4.esm", {0x758AF, 0x758B0, 0x758B1, 0x184754, 0x184755}},
    {"rustjaws", "Fallout4.esm", {0x758AF, 0x758B0, 0x758B1, 0x184754, 0x184755}},
    {"saltdogs", "Fallout4.esm", {0x758AF, 0x758B0, 0x758B1, 0x184754, 0x184755}},
    {"butchers", "Fallout4.esm", {0x758AF, 0x758B0, 0x758B1, 0x184754, 0x184755}},
    {"forged", "Fallout4.esm", {0x13101C, 0x13101C, 0x13104B, 0x13105B, 0x215CD2}},
    {"gunners", "Fallout4.esm", {0xFD3A0, 0xFD3A2, 0x70507, 0x184762, 0x184764}},
    {"mutants", "Fallout4.esm", {0xEDCC7, 0xEDCC9, 0xEDCD5, 0x17E6F0, 0x17E6F4}},
    {"thrash", "Fallout4.esm", {0xEDCC7, 0xEDCC9, 0xEDCD5, 0x17E6F0, 0x17E6F4}},
    {"triggermen", "Fallout4.esm", {0x19EF96, 0x19EF97, 0xD3FCA, 0x2404B5, 0x2404B6}},
    {"atom", "Fallout4.esm", {0x1BBCBE, 0x1BBCBF, 0x1BBCC0, 0x213F53, 0x213F53}},
    {"atomites", "Fallout4.esm", {0x1BBCBE, 0x1BBCBF, 0x1BBCC0, 0x213F53, 0x213F53}},
    {"dcsecurity", "Fallout4.esm", {0x2F66, 0x2F66, 0xF61BA, 0xF61BA, 0xF61BA}},
    {"minutemen", "Fallout4.esm", {0x1BF0BB, 0x1BF0BD, 0x1BF0BF, 0x1BF0C1, 0x1BF0C1}},
    {"scavengers", "Fallout4.esm", {0x2BEE9, 0x2BEE9, 0x2BEE9, 0x2BEE9, 0x2BEE9}},
    {"marshals", "KARMA.esm", {0x178, 0x178, 0x179, 0x179, 0x179}},
    {"blackledger", "KARMA.esm", {0x174, 0x174, 0x175, 0x175, 0x175}},
    {"rustdevils", "DLCRobot.esm", {0x5050, 0x5050, 0x5050, 0x5050, 0xE399}},
    {"trappers", "DLCCoast.esm", {0x2DB9, 0x2DBA, 0x2DBB, 0x48BB2, 0x48BB3}},
    {"disciples", "DLCNukaWorld.esm", {0xDBD9, 0xDBD9, 0xDBD9, 0x10E54, 0x10E54}},
    {"operators", "DLCNukaWorld.esm", {0xDBDB, 0xDBDB, 0xDBDB, 0x10E52, 0x10E52}},
    {"pack", "DLCNukaWorld.esm", {0xDBDC, 0xDBDC, 0xDBDC, 0x10E53, 0x10E53}},
};

// Weapon keywords (Fallout4.esm) and the cause they mean (RequestNemesis::Cause)
const std::pair<uint32_t, uint8_t> kWeaponCauses[] = {
    {0x22575C, 5}, {0x22575B, 5}, {0x4C922, 5}, // Fat Man, missile launcher, explosive: explosion
    {0x225760, 3},                              // flamer: fire
    {0x92A84, 4}, {0x92A85, 4}, {0x225762, 4}, {0x22575F, 4}, // laser, plasma, gamma, cryolator: energy
    {0x4A0A4, 1}, {0x4A0A5, 2}, {0x5240E, 2},   // melee 1H: blade, 2H and unarmed: blunt
};

uint32_t PluginForm(const char* acpPlugin, uint32_t aLocalId) noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(acpPlugin);
    return pMod && pMod->IsLoaded() ? pMod->GetFormId(aLocalId) : 0;
}

template <class T> T* PluginFormAs(const char* acpPlugin, uint32_t aLocalId) noexcept
{
    const uint32_t id = PluginForm(acpPlugin, aLocalId);
    return id ? Cast<T>(TESForm::GetById(id)) : nullptr;
}

// A native as the game registered it (nullptr, logged once, if it didn't)
const void* NativeAddress(const char* acpScript, const char* acpName) noexcept
{
    const void* pAddress = World::Get().ctx().at<PapyrusService>().Get(acpScript, acpName);
    if (!pAddress)
    {
        static std::set<std::string> s_missing;
        if (s_missing.insert(std::string(acpScript) + "." + acpName).second)
            spdlog::error("Nemeses: {}.{} isn't registered, its part is skipped", acpScript, acpName);
    }
    return pAddress;
}

// The generated NPC of a character spawned from a leveled or template NPC (its own, nobody else's)
TESNPC* OwnNpcOf(const Actor* apActor) noexcept
{
    auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    return pNpc && pNpc->IsTemporary() ? pNpc : nullptr;
}

ActorValueInfo* ValueForm(uint32_t aId) noexcept
{
    return Cast<ActorValueInfo>(TESForm::GetById(aId));
}

// ObjectReference.GetValue / GetBaseValue / SetValue
float GetValue(Actor* apActor, uint32_t aValueId, bool aBase = false) noexcept
{
    using TGet = float(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESObjectREFR*, const ActorValueInfo*);
    static auto s_get = reinterpret_cast<TGet>(const_cast<void*>(NativeAddress("ObjectReference", "GetValue")));
    static auto s_getBase = reinterpret_cast<TGet>(const_cast<void*>(NativeAddress("ObjectReference", "GetBaseValue")));
    const ActorValueInfo* pValue = ValueForm(aValueId);
    const TGet function = aBase ? s_getBase : s_get;
    return function && pValue && apActor ? function(GameVM::Get()->virtualMachine, 0, apActor, pValue) : 0.f;
}

void SetValue(Actor* apActor, uint32_t aValueId, float aValue) noexcept
{
    using TSet = void(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESObjectREFR*, const ActorValueInfo*, float);
    static auto s_set = reinterpret_cast<TSet>(const_cast<void*>(NativeAddress("ObjectReference", "SetValue")));
    if (const ActorValueInfo* pValue = ValueForm(aValueId); s_set && pValue && apActor)
        s_set(GameVM::Get()->virtualMachine, 0, apActor, pValue, aValue);
}

bool FormHasKeyword(TESForm* apForm, TESForm* apKeyword) noexcept
{
    using THasKeyword = bool(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESForm*, TESForm*);
    static auto s_hasKeyword = reinterpret_cast<THasKeyword>(const_cast<void*>(NativeAddress("Form", "HasKeyword")));
    return s_hasKeyword && apForm && apKeyword && s_hasKeyword(GameVM::Get()->virtualMachine, 0, apForm, apKeyword);
}

// --- KARMA's ModKarma, a method of the script on its quest, run on the VM's own time (IVirtualMachine::DispatchMethodCall
// by handle, slot 0x2E). The same call as the dogs' (CompanionService), which has its own copy.

struct VmPointer
{
    void* pObject = nullptr;
};

void** VmTable() noexcept
{
    return *reinterpret_cast<void***>(GameVM::Get()->virtualMachine);
}

uint64_t VmHandleOf(uint32_t aFormType, const void* apForm) noexcept
{
    auto* pVm = GameVM::Get()->virtualMachine;
    using TGetPolicy = void*(__fastcall*)(BSScript::IVirtualMachine*);
    void* pPolicy = reinterpret_cast<TGetPolicy>(VmTable()[0x33])(pVm);
    using TGetHandle = uint64_t(__fastcall*)(void*, uint32_t, const void*);
    return reinterpret_cast<TGetHandle>((*reinterpret_cast<void***>(pPolicy))[7])(pPolicy, aFormType, apForm);
}

// The game's std::function (BSTThreadScrapFunction) as Visual C++ 2012 lays it out: 0x18 bytes of room, then the
// callable (see CompanionService's GameArguments for why ours can't be used)
class GameArguments
{
public:
    using TFill = std::function<void(BSScript::Statement&)>;

    explicit GameArguments(TFill aFill) noexcept
        : m_pCallable(new (&m_room) Callable(std::make_shared<const TFill>(std::move(aFill))))
    {
    }
    ~GameArguments() { m_pCallable->DeleteThis(false); }

    GameArguments(const GameArguments&) = delete;
    GameArguments& operator=(const GameArguments&) = delete;

private:
    struct __declspec(novtable) CallableBase
    {
        virtual CallableBase* Copy(void* apWhere) const = 0;
        virtual CallableBase* Move(void* apWhere) = 0;
        virtual bool Call(BSScript::Statement& aStatement) = 0;
        virtual const std::type_info& TargetType() const = 0;
        virtual void DeleteThis(bool aFree) = 0;
        virtual ~CallableBase() = default;
        virtual const void* Get() const = 0;
    };

    struct Callable final : CallableBase
    {
        explicit Callable(std::shared_ptr<const TFill> aFill) noexcept
            : m_fill(std::move(aFill))
        {
        }

        CallableBase* Copy(void* apWhere) const override { return new (apWhere ? apWhere : ::operator new(sizeof(Callable))) Callable(m_fill); }
        CallableBase* Move(void* apWhere) override { return new (apWhere ? apWhere : ::operator new(sizeof(Callable))) Callable(std::move(m_fill)); }
        bool Call(BSScript::Statement& aStatement) override
        {
            if (m_fill && *m_fill)
                (*m_fill)(aStatement);
            return true;
        }
        const std::type_info& TargetType() const override { return typeid(TFill); }
        void DeleteThis(bool aFree) override
        {
            this->~Callable();
            if (aFree)
                ::operator delete(this);
        }
        const void* Get() const override { return m_fill.get(); }

        std::shared_ptr<const TFill> m_fill;
    };
    static_assert(sizeof(Callable) <= 0x18);

    alignas(8) uint8_t m_room[0x18];
    CallableBase* m_pCallable;
};
static_assert(sizeof(GameArguments) == 0x20);

bool CallQuestMethod(TESForm* apQuest, const char* acpScript, const char* acpFunction, std::function<void(BSScript::Statement&)> aArguments) noexcept
{
    if (!apQuest)
        return false;

    using TDispatch = bool(__fastcall*)(BSScript::IVirtualMachine*, uint64_t, const BSFixedString&, const BSFixedString&, const GameArguments&, const VmPointer&);

    BSFixedString script(acpScript);
    BSFixedString function(acpFunction);
    const GameArguments arguments(std::move(aArguments));
    const VmPointer noCallback;
    const bool sent = reinterpret_cast<TDispatch>(VmTable()[0x2E])(GameVM::Get()->virtualMachine, VmHandleOf(kQuestFormType, apQuest), script, function, arguments, noCallback);
    script.Release();
    function.Release();

    if (!sent)
        spdlog::error("Nemeses: couldn't call {}.{}", acpScript, acpFunction);
    return sent;
}

std::mt19937& Random() noexcept
{
    static std::mt19937 s_random{std::random_device{}()};
    return s_random;
}

float RandomRange(float aLo, float aHi) noexcept
{
    return std::uniform_real_distribution<float>(aLo, aHi)(Random());
}
} // namespace

namespace NemesisGame
{
std::string GangOf(const Actor* apActor, bool aWinterOfAtom) noexcept
{
    const auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    if (!pNpc)
        return {};

    const auto& factions = pNpc->actorData.factions;
    for (const FactionKey& key : kFactions)
    {
        const uint32_t factionId = PluginForm(key.Plugin, key.Faction);
        if (!factionId)
            continue;
        for (uint32_t i = 0; i < factions.length; ++i)
        {
            const auto& entry = factions.data[i];
            if (entry.faction && entry.faction->formID == factionId && entry.rank >= 0)
            {
                // Atom's faithful in 2282 are the Last Son's
                if (aWinterOfAtom && std::strcmp(key.Gang, "atom") == 0)
                    return "atomites";
                return key.Gang;
            }
        }
    }
    return {};
}

bool IsPerson(const Actor* apActor) noexcept
{
    if (!apActor || apActor == PlayerCharacter::Get())
        return false;
    auto* pActor = const_cast<Actor*>(apActor);
    if (pActor->GetExtension()->IsPlayer() || pActor->IsEssential())
        return false;
    const auto* pNpc = Cast<TESNPC>(apActor->baseForm);
    if (!pNpc || (pNpc->actorData.flags & (kEssential | kUnique)))
        return false;
    const auto* pRace = pNpc->raceForm.race;
    const uint32_t race = pRace ? pRace->formID : 0;
    return race == kHumanRace || race == kGhoulRace || race == kSuperMutantRace;
}

uint8_t BodyOf(const Actor* apActor) noexcept
{
    const auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    if (!pNpc)
        return 0;
    const uint32_t race = pNpc->raceForm.race ? pNpc->raceForm.race->formID : 0;
    const bool female = (pNpc->actorData.flags & kFemale) != 0;
    if (race == kSuperMutantRace)
        return 4;
    if (race == kGhoulRace)
        return female ? 3 : 2;
    return female ? 1 : 0;
}

bool IsKarmaHunter(const Actor* apActor) noexcept
{
    const auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    const auto* pKeyword = PluginFormAs<BGSKeyword>(kKarmaPlugin, kKarmaHunterLeaderKeyword);
    return pNpc && pKeyword && pNpc->keywordForm.HasKeyword(pKeyword);
}

std::string KeyOf(uint32_t aFormId) noexcept
{
    if (!aFormId)
        return {};
    const bool lite = (aFormId >> 24) == 0xFE;
    const uint32_t localId = lite ? aFormId & 0xFFF : aFormId & 0xFFFFFF;
    for (auto* pEntry = &ModManager::Get()->mods.entry; pEntry && pEntry->data; pEntry = pEntry->next)
    {
        const Mod* pMod = pEntry->data;
        if (!pMod->IsLoaded() || pMod->IsLite() != lite)
            continue;
        if (lite ? pMod->liteId == ((aFormId >> 12) & 0xFFF) : pMod->standardId == (aFormId >> 24))
            return fmt::format("{}:{:X}", pMod->filename, localId);
    }
    return {};
}

std::string WorldKeyOf(const TESObjectREFR* apRef) noexcept
{
    const auto* pWorldSpace = apRef ? apRef->GetWorldSpace() : nullptr;
    return pWorldSpace ? KeyOf(pWorldSpace->formID) : std::string();
}

TESForm* PlayerLocation() noexcept
{
    const auto* pPlayer = PlayerCharacter::Get();
    return pPlayer ? pPlayer->locationForm : nullptr;
}

std::string PlaceName() noexcept
{
    const TESForm* pLocation = PlayerLocation();
    return pLocation ? std::string(pLocation->GetName()) : std::string();
}

bool InSettlement() noexcept
{
    TESForm* pLocation = PlayerLocation();
    return pLocation && FormHasKeyword(pLocation, TESForm::GetById(kSettlementKeyword));
}

bool IsBusy() noexcept
{
    static const char* s_menus[] = {"LoadingMenu", "MainMenu", "DialogueMenu", "PipboyMenu", "WorkshopMenu", "TerminalMenu", "BarterMenu", "ContainerMenu",
                                    "SleepWaitMenu", "LooksMenu", "LevelUpMenu", "VATSMenu", "PauseMenu", "Console", "CookingMenu", "ExamineMenu"};
    static std::vector<std::unique_ptr<BSFixedString>> s_names = []()
    {
        std::vector<std::unique_ptr<BSFixedString>> names;
        for (const char* pName : s_menus)
            names.push_back(std::make_unique<BSFixedString>(pName));
        return names;
    }();

    UI* pUi = UI::Get();
    if (!pUi)
        return true;
    for (const auto& pName : s_names)
    {
        if (pUi->GetMenuOpen(*pName))
            return true;
    }
    return false;
}

TESNPC* SpawnBase(const std::string& acGang, int aRank, bool aLegendary) noexcept
{
    const int rank = std::clamp(aLegendary ? std::max(aRank, 4) : std::min(aRank, 3), 1, 5);
    for (const SpawnForms& spawn : kSpawns)
    {
        if (acGang != spawn.Gang)
            continue;
        if (auto* pNpc = PluginFormAs<TESNPC>(spawn.Plugin, spawn.ByRank[rank - 1]))
            return pNpc;
    }
    // A gang whose plugin is gone: generic raiders
    return Cast<TESNPC>(TESForm::GetById(0x758AF));
}

bool FindSpot(float aDistance, NiPoint3& aSpot) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell || !pPlayer->GetWorldSpace())
        return false;

    const uint32_t filter = WorldRays::GetLineOfSightFilter(pPlayer);
    NiPoint3 eye = pPlayer->position;
    eye.z += 120.f;
    bool haveFallback = false;
    NiPoint3 fallback{};

    for (int attempt = 0; attempt < 12; ++attempt)
    {
        // Behind the player first, then anywhere around
        const float spread = attempt < 6 ? 1.2f : 3.1f;
        const float angle = pPlayer->rotation.z + 3.14159f + RandomRange(-spread, spread);
        const float distance = aDistance * RandomRange(0.85f, 1.15f);
        NiPoint3 top = pPlayer->position;
        top.x += std::sin(angle) * distance;
        top.y += std::cos(angle) * distance;
        top.z += 2500.f;
        NiPoint3 bottom = top;
        bottom.z -= 6000.f;

        const WorldRays::RayHit ground = WorldRays::PickOnce(pCell, filter, top, bottom);
        if (!ground.Hit || ground.NormalZ < 0.7f || WorldRays::IsCharacterLayer(ground.Layer))
            continue;

        NiPoint3 head = ground.Point;
        head.z += 110.f;
        const WorldRays::RayHit sight = WorldRays::PickOnce(pCell, filter, eye, head);
        if (sight.Hit && !WorldRays::IsCharacterLayer(sight.Layer))
        {
            aSpot = ground.Point;
            return true;
        }
        if (!haveFallback)
        {
            fallback = ground.Point;
            haveFallback = true;
        }
    }

    if (haveFallback)
        aSpot = fallback;
    return haveFallback;
}

bool GroundAt(NiPoint3& aSpot) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell)
        return false;
    NiPoint3 top = aSpot;
    top.z += 1500.f;
    NiPoint3 bottom = aSpot;
    bottom.z -= 3000.f;
    const WorldRays::RayHit ground = WorldRays::PickOnce(pCell, WorldRays::GetLineOfSightFilter(pPlayer), top, bottom);
    if (!ground.Hit || ground.NormalZ < 0.7f || WorldRays::IsCharacterLayer(ground.Layer))
        return false;
    aSpot = ground.Point;
    return true;
}

Actor* Spawn(TESNPC* apBase, const NiPoint3& acSpot) noexcept
{
    // Actor* PlaceActorAtMe(VM*, stack, RefrOrInventoryObj* self, TESNPC* npc, uint32_t levelMod, BGSEncounterZone* zone)
    using TPlace = Actor*(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, const RefrOrInventoryObj*, TESNPC*, uint32_t, void*);
    static auto s_place = reinterpret_cast<TPlace>(const_cast<void*>(NativeAddress("ObjectReference", "PlaceActorAtMe")));
    auto* pPlayer = PlayerCharacter::Get();
    if (!s_place || !apBase || !pPlayer)
        return nullptr;

    const RefrOrInventoryObj self{pPlayer, nullptr, 0};
    // Level mod 1: medium (0 easy .. 3 boss, 4 none)
    Actor* pActor = s_place(GameVM::Get()->virtualMachine, 0, &self, apBase, 1, nullptr);
    if (!pActor)
        return nullptr;

    pActor->ForcePosition(acSpot);
    const float dx = pPlayer->position.x - acSpot.x;
    const float dy = pPlayer->position.y - acSpot.y;
    pActor->SetRotation(0.f, 0.f, std::atan2(dx, dy));
    return pActor;
}

void SetName(Actor* apActor, const std::string& acName) noexcept
{
    TESNPC* pNpc = OwnNpcOf(apActor);
    if (!pNpc || acName.empty())
        return;
    const char* pCurrent = pNpc->fullName.value.AsAscii();
    if (pCurrent && acName == pCurrent)
        return;

    // BSStringPool::GetEntry<char> (CommonLibF4 id 507142), as the dogs' names
    using TGetEntry = void(BSFixedString::Data*&, const char*, bool);
    POINTER_FALLOUT4(TGetEntry, s_getEntry, 507143);
    BSFixedString::Data* pEntry = nullptr;
    s_getEntry.Get()(pEntry, acName.c_str(), false);
    if (pEntry)
        pNpc->fullName.value.data = pEntry;
}

void ApplyRank(Actor* apActor, int aRank, uint8_t aInjuries) noexcept
{
    if (!apActor)
        return;
    // +15 resistances a rank above the first; the base health stays the encounter NPC's (its level already rises with it)
    const float bonus = 15.f * static_cast<float>(std::clamp(aRank, 1, 5) - 1);
    if (bonus > 0.f)
    {
        SetValue(apActor, kDamageResist, GetValue(apActor, kDamageResist, true) + bonus);
        SetValue(apActor, kEnergyResist, GetValue(apActor, kEnergyResist, true) + bonus);
    }
    // NemesisMan::kLimp
    if (aInjuries & 1)
        SetValue(apActor, kSpeedMult, 85.f);
}

void SetAlignment(Actor* apActor, int aAlignment) noexcept
{
    const int index = std::clamp(aAlignment, -2, 2) + 2;
    auto* pKeyword = PluginFormAs<BGSKeyword>(kKarmaPlugin, kKarmaAlignmentKeywords[index]);
    if (!apActor || !pKeyword)
        return;

    // void AddKeyword(VM*, stack, RefrOrInventoryObj* self, const BGSKeyword*)
    using TAddKeyword = void(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, const RefrOrInventoryObj*, const BGSKeyword*);
    static auto s_addKeyword = reinterpret_cast<TAddKeyword>(const_cast<void*>(NativeAddress("ObjectReference", "AddKeyword")));
    if (s_addKeyword)
    {
        const RefrOrInventoryObj self{apActor, nullptr, 0};
        s_addKeyword(GameVM::Get()->virtualMachine, 0, &self, pKeyword);
    }
    if (const uint32_t valueId = PluginForm(kKarmaPlugin, kKarmaAlignmentValue))
        SetValue(apActor, valueId, static_cast<float>(index));
}

bool TravelTo(Actor* apActor, TESObjectREFR* apTarget) noexcept
{
    constexpr uint32_t kCloseIn = 0x8E2;               // RDVehicles.esp RDV_RaiderCloseIn
    constexpr uint32_t kTravelWeaponDrawn = 0x249038;  // Fallout4.esm BoS_TravelToLinkedRefWeaponDrawn
    if (!apActor || !apTarget || apActor->IsDead())
        return false;
    auto* pPackage = PluginFormAs<TESPackage>("RDVehicles.esp", kCloseIn);
    if (!pPackage)
        pPackage = Cast<TESPackage>(TESForm::GetById(kTravelWeaponDrawn));
    // void SetLinkedRef(VM*, stack, TESObjectREFR* self, TESObjectREFR* linked, BGSKeyword* keyword): NativeFunction2<TESObjectREFR, void,
    // TESObjectREFR*, BGSKeyword*> in the 1.10.163 exe (callback 0x141416440)
    using TSetLinkedRef = void(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESObjectREFR*, TESObjectREFR*, BGSKeyword*);
    static auto s_setLinkedRef = reinterpret_cast<TSetLinkedRef>(const_cast<void*>(NativeAddress("ObjectReference", "SetLinkedRef")));
    if (!pPackage || !s_setLinkedRef)
        return false;
    s_setLinkedRef(GameVM::Get()->virtualMachine, 0, apActor, apTarget, nullptr);
    apActor->SetPackage(pPackage);
    return true;
}

namespace
{
// Game.GetAggressionAV: NativeFunction0<StaticFunctionTag, const ActorValueInfo*> (callback 0x1413c2160). The AI's values aren't records in the
// esm: the engine makes them.
const ActorValueInfo* AggressionValue() noexcept
{
    using TGetAv = const ActorValueInfo*(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, void*);
    static auto s_get = reinterpret_cast<TGetAv>(const_cast<void*>(NativeAddress("Game", "GetAggressionAV")));
    static const ActorValueInfo* s_value = s_get ? s_get(GameVM::Get()->virtualMachine, 0, nullptr) : nullptr;
    return s_value;
}
} // namespace

// (ObjectReference.GetValue / SetValue with the value itself: it isn't looked up again by its id)
float Aggression(Actor* apActor) noexcept
{
    using TGet = float(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESObjectREFR*, const ActorValueInfo*);
    static auto s_get = reinterpret_cast<TGet>(const_cast<void*>(NativeAddress("ObjectReference", "GetValue")));
    const ActorValueInfo* pValue = AggressionValue();
    return s_get && pValue && apActor ? s_get(GameVM::Get()->virtualMachine, 0, apActor, pValue) : -1.f;
}

void SetAggression(Actor* apActor, float aValue) noexcept
{
    using TSet = void(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESObjectREFR*, const ActorValueInfo*, float);
    static auto s_set = reinterpret_cast<TSet>(const_cast<void*>(NativeAddress("ObjectReference", "SetValue")));
    if (const ActorValueInfo* pValue = AggressionValue(); s_set && pValue && apActor && aValue >= 0.f)
        s_set(GameVM::Get()->virtualMachine, 0, apActor, pValue, aValue);
}

float HealthPercent(Actor* apActor) noexcept
{
    const float base = GetValue(apActor, kHealth, true);
    return base > 0.f ? std::clamp(GetValue(apActor, kHealth) / base, 0.f, 1.f) : 1.f;
}

uint8_t CrippledLimbs(Actor* apActor) noexcept
{
    uint8_t limbs = 0;
    if (GetValue(apActor, kLeftMobilityCondition) <= 0.f)
        limbs |= 1;
    if (GetValue(apActor, kRightMobilityCondition) <= 0.f)
        limbs |= 2;
    if (GetValue(apActor, kLeftAttackCondition) <= 0.f)
        limbs |= 4;
    if (GetValue(apActor, kRightAttackCondition) <= 0.f)
        limbs |= 8;
    if (GetValue(apActor, kPerceptionCondition) <= 0.f)
        limbs |= 16;
    return limbs;
}

uint8_t CauseOf(Actor* apKiller) noexcept
{
    TESForm* pWeapon = apKiller ? apKiller->GetEquippedWeapon(0) : nullptr;
    if (!pWeapon)
        return 0;
    for (const auto& [keyword, cause] : kWeaponCauses)
    {
        if (FormHasKeyword(pWeapon, TESForm::GetById(keyword)))
            return cause;
    }
    return 0;
}

// A function of RD_Nemeses.esp's tab quest script on a man (his form id: Game.GetForm finds created references too)
void CallOnMan(const char* acpFunction, const Actor* apActor, std::optional<bool> aFlag = std::nullopt) noexcept
{
    TESForm* pQuest = apActor ? TESForm::GetById(PluginForm(kNemesesPlugin, kTabQuest)) : nullptr;
    if (!pQuest)
        return;
    const auto formId = static_cast<int32_t>(apActor->formID);
    CallQuestMethod(pQuest, kTabScript, acpFunction, [formId, aFlag](BSScript::Statement& aStatement)
    {
        aStatement.SetSize(aFlag ? 2 : 1);
        aStatement.vars[0].Set(formId);
        if (aFlag)
            aStatement.vars[1].Set(*aFlag);
    });
}

void Surrender(Actor* apActor) noexcept
{
    // As the game's holdup: out of combat, unaggressive, hands up; and the captive keyword Interrogate shows on
    CallOnMan("HandsUp", apActor);
}

void Release(Actor* apActor) noexcept
{
    CallOnMan("HandsDown", apActor);
}

void LetGo(Actor* apActor, bool aFights) noexcept
{
    CallOnMan("LetGo", apActor, aFights);
}

void MarkCaptive(Actor* apActor) noexcept
{
    CallOnMan("MarkCaptive", apActor);
}

bool IsHeldUp(Actor* apActor) noexcept
{
    return apActor && GetValue(apActor, kHoldupValue) >= 1.f;
}

int PlayerCharisma() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    return pPlayer ? static_cast<int>(GetValue(pPlayer, kCharisma)) : 1;
}

bool TakeInterrogateRequest() noexcept
{
    auto* pGlobal = PluginFormAs<TESGlobal>(kNemesesPlugin, kInterrogateNow);
    if (!pGlobal || pGlobal->f <= 0.f)
        return false;
    pGlobal->f = 0.f;
    return true;
}

void Calm(Actor* apActor) noexcept
{
    if (!apActor)
        return;
    using TSetRestrained = bool(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, Actor*, bool);
    static auto s_setRestrained = reinterpret_cast<TSetRestrained>(const_cast<void*>(NativeAddress("Actor", "SetRestrained")));
    if (s_setRestrained)
        s_setRestrained(GameVM::Get()->virtualMachine, 0, apActor, false);
    // Without his gang's factions nobody is his enemy: he wanders off
    apActor->RemoveFromAllFactions();
    apActor->StopCombat();
}

void Restrain(Actor* apActor, bool aRestrained) noexcept
{
    using TSetRestrained = bool(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, Actor*, bool);
    static auto s_setRestrained = reinterpret_cast<TSetRestrained>(const_cast<void*>(NativeAddress("Actor", "SetRestrained")));
    if (s_setRestrained && apActor)
        s_setRestrained(GameVM::Get()->virtualMachine, 0, apActor, aRestrained);
}

void Attack(Actor* apActor, Actor* apTarget) noexcept
{
    if (apActor && apTarget && !apActor->IsDead())
        apActor->StartCombat(apTarget);
}

void Kill(Actor* apActor, Actor* apKiller) noexcept
{
    // void Kill(VM*, stack, Actor*, Actor* killer)
    using TKill = void(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, Actor*, Actor*);
    static auto s_kill = reinterpret_cast<TKill>(const_cast<void*>(NativeAddress("Actor", "Kill")));
    if (s_kill && apActor && !apActor->IsDead())
        s_kill(GameVM::Get()->virtualMachine, 0, apActor, apKiller);
}

bool ReadKarma(int& aKarma, int& aAlignment) noexcept
{
    const auto* pKarma = PluginFormAs<TESGlobal>(kKarmaPlugin, kKarmaGlobal);
    const auto* pAlignment = PluginFormAs<TESGlobal>(kKarmaPlugin, kKarmaAlignmentGlobal);
    if (!pKarma || !pAlignment)
        return false;
    aKarma = static_cast<int>(pKarma->f);
    aAlignment = std::clamp(static_cast<int>(pAlignment->f), 0, 4);
    return true;
}

void ModKarma(int aAmount, bool aSilent) noexcept
{
    TESForm* pQuest = TESForm::GetById(PluginForm(kKarmaPlugin, kKarmaQuest));
    if (!pQuest || !aAmount)
        return;
    // KARM:Program.ModKarma(int aiAmount, bool abSilent, bool abDirect): through its multipliers, as KARMA's own deeds
    CallQuestMethod(pQuest, "KARM:Program", "ModKarma", [aAmount, aSilent](BSScript::Statement& aStatement)
    {
        aStatement.SetSize(3);
        aStatement.vars[0].Set(static_cast<int32_t>(aAmount));
        aStatement.vars[1].Set(aSilent);
        aStatement.vars[2].Set(false);
    });
}

int PlayerCaps() noexcept
{
    using TGetItemCount = uint32_t(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESObjectREFR*, TESForm*);
    static auto s_count = reinterpret_cast<TGetItemCount>(const_cast<void*>(NativeAddress("ObjectReference", "GetItemCount")));
    auto* pPlayer = PlayerCharacter::Get();
    TESForm* pCaps = TESForm::GetById(kCaps);
    return s_count && pPlayer && pCaps ? static_cast<int>(s_count(GameVM::Get()->virtualMachine, 0, pPlayer, pCaps)) : 0;
}

bool PayCaps(int aAmount) noexcept
{
    // bool RemoveItem(VM*, stack, TESObjectREFR*, FormOrInventoryObj* item, int count, bool silent, TESObjectREFR* to)
    using TRemoveItem = bool(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, TESObjectREFR*, const FormOrInventoryObj*, int32_t, bool, TESObjectREFR*);
    static auto s_remove = reinterpret_cast<TRemoveItem>(const_cast<void*>(NativeAddress("ObjectReference", "RemoveItem")));
    auto* pPlayer = PlayerCharacter::Get();
    TESForm* pCaps = TESForm::GetById(kCaps);
    if (!s_remove || !pPlayer || !pCaps || aAmount <= 0 || PlayerCaps() < aAmount)
        return false;
    const FormOrInventoryObj item{pCaps, 0};
    s_remove(GameVM::Get()->virtualMachine, 0, pPlayer, &item, aAmount, false, nullptr);
    return true;
}

bool CaptureFace(Actor* apActor, std::string& aBuffer, uint32_t& aFlags) noexcept
{
    TESNPC* pNpc = OwnNpcOf(apActor);
    if (!pNpc)
        return false;

    // As TESNPC::Serialize, with only the face's change flags
    ScopedSaveLoadOverride saveLoadOverride;
    std::vector<char> buffer(1 << 15);
    BGSSaveFormBuffer saveBuffer;
    saveBuffer.buffer = buffer.data();
    saveBuffer.capacity = static_cast<uint32_t>(buffer.size());
    saveBuffer.changeFlags = kFaceFlags;
    pNpc->Save(&saveBuffer);
    aBuffer.assign(saveBuffer.buffer, saveBuffer.position);
    saveBuffer.buffer = nullptr;
    aFlags = kFaceFlags;
    return !aBuffer.empty();
}

bool ApplyFace(Actor* apActor, const std::string& acBuffer, uint32_t aFlags) noexcept
{
    TESNPC* pNpc = OwnNpcOf(apActor);
    if (!pNpc || acBuffer.empty() || aFlags != kFaceFlags)
        return false;

    // As TESNPC::Deserialize, on his own generated NPC only
    ScopedSaveLoadOverride saveLoadOverride;
    BGSLoadFormBuffer loadBuffer(aFlags);
    loadBuffer.SetSize(acBuffer.size() & 0xFFFFFFFF);
    loadBuffer.buffer = acBuffer.data();
    loadBuffer.formId = pNpc->formID;
    loadBuffer.form = pNpc;
    pNpc->Load(&loadBuffer);
    loadBuffer.buffer = nullptr;
    apActor->QueueUpdate();
    return true;
}

void ShowHud(const std::string& acText) noexcept
{
    if (!acText.empty())
        Utils::ShowHudMessage(String(acText.c_str()));
}

std::array<float, 6> LimbConditions(Actor* apActor) noexcept
{
    return {GetValue(apActor, kPerceptionCondition), GetValue(apActor, kEnduranceCondition), GetValue(apActor, kLeftAttackCondition),
            GetValue(apActor, kRightAttackCondition), GetValue(apActor, kLeftMobilityCondition), GetValue(apActor, kRightMobilityCondition)};
}

std::string WeaponName(Actor* apActor) noexcept
{
    TESForm* pWeapon = apActor ? apActor->GetEquippedWeapon(0) : nullptr;
    const auto* pName = pWeapon ? Cast<TESFullName>(pWeapon) : nullptr;
    const char* pText = pName ? pName->value.AsAscii() : nullptr;
    return pText && *pText ? pText : "fists";
}

std::string AmmoName(Actor* apActor) noexcept
{
    // AIProcess::GetCurrentAmmo(BGSEquipIndex) (CommonLibF4 id 1154936, 0x140E70530): the ammo the gun in hand is
    // loaded with, from the process's equipped weapon data (+0x10), so a receiver mod's ammo too; the base form's
    // weaponData.ammo is only the default. Null bare-handed.
    using TGetCurrentAmmo = TESForm*(void*, uint32_t);
    POINTER_FALLOUT4(TGetCurrentAmmo, s_getCurrentAmmo, 1154937);
    TESForm* pAmmo = apActor && apActor->currentProcess ? s_getCurrentAmmo.Get()(apActor->currentProcess, 0) : nullptr;
    const auto* pName = pAmmo && pAmmo->GetFormType() == FormType::Ammo ? Cast<TESFullName>(pAmmo) : nullptr;
    const char* pText = pName ? pName->value.AsAscii() : nullptr;
    return pText ? pText : "";
}

bool AimedAt(Actor* apTarget, float aMaxDistance) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !apTarget || !pPlayer->actorState.IsWeaponDrawn())
        return false;
    const NiPoint3 feet = apTarget->position;
    const float dx = feet.x - pPlayer->position.x, dy = feet.y - pPlayer->position.y, dz = feet.z - pPlayer->position.z;
    if (dx * dx + dy * dy + dz * dz > aMaxDistance * aMaxDistance)
        return false;
    // The middle of the view goes through him, as the actor bars count it: this wide, from his feet to over his head
    const float scale = apTarget->GetScale();
    NiPoint3 head = feet;
    head.z += 125.f * scale;
    float headRight, headUp, headAhead, feetRight, feetUp, feetAhead;
    if (!HudMarker::WorldToView(head, headRight, headUp, headAhead) || !HudMarker::WorldToView(feet, feetRight, feetUp, feetAhead) || headAhead <= 0.f ||
        feetAhead <= 0.f)
        return false;
    return std::abs(0.5f * (headRight + feetRight)) < 32.f * scale && feetUp <= 0.f && headUp + 12.f * scale >= 0.f;
}

bool IsPipboyOpen() noexcept
{
    UI* pUI = UI::Get();
    return pUI && pUI->GetMenuOpen(BSFixedString("PipboyMenu"));
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

bool AimingDownSights() noexcept
{
    // The gun state, actorState.flags2 (Actor+0x134) >> 14 & 0xF: 6 sighted, 8 firing sighted. The game's activate
    // handler makes the same test (0x140F4ACF6), and ActorBarsService reads it so
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;
    const uint32_t gunState = (pPlayer->actorState.flags2 >> 14) & 0xF;
    return gunState == 6 || gunState == 8;
}

int ActivateKey() noexcept
{
    // ControlMap (CommonLibF4 id 325206, 0x1459DA210): controlMaps[context] at +8, each an InputContext whose first
    // array is the keyboard's UserEventMapping {BSFixedString eventID, int32 inputKey, ...} (0x18 bytes); main
    // gameplay is context 0. Its keyboard codes are Windows virtual keys (the built-in map: Activate 0x45, E)
    struct Mapping
    {
        BSFixedString::Data* pEventId;
        int32_t InputKey;
        uint8_t pad[0xC];
    };
    static_assert(sizeof(Mapping) == 0x18);

    POINTER_FALLOUT4(uint8_t*, s_controlMap, 325207);
    const uint8_t* pMap = *s_controlMap.Get();
    const auto* pKeyboard = pMap ? *reinterpret_cast<GameArray<Mapping>* const*>(pMap + 0x8) : nullptr;
    if (!pKeyboard || !pKeyboard->data)
        return 0;
    for (uint32_t i = 0; i < pKeyboard->length; ++i)
    {
        const Mapping& mapping = pKeyboard->data[i];
        const char* pName = mapping.pEventId ? mapping.pEventId->AsAscii() : nullptr;
        if (pName && _stricmp(pName, "Activate") == 0)
            return mapping.InputKey > 0 && mapping.InputKey < 0xFF ? mapping.InputKey : 0;
    }
    return 0;
}
} // namespace NemesisGame

#endif

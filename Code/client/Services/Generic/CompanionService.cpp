#include <Services/CompanionService.h>

#if TP_FALLOUT4

#include <World.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Events/UpdateEvent.h>
#include <Forms/TESNPC.h>
#include <Forms/TESQuest.h>
#include <Forms/TESGlobal.h>
#include <Forms/TESFaction.h>
#include <Forms/BGSKeyword.h>
#include <Forms/ActorValueInfo.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <SaveFolder.h>
#include <Services/PapyrusService.h>
#include <Services/OverlayService.h>
#include <MiniJson.h>
#include <PerfProbe.h>

namespace
{
constexpr const char* kPlugin = "RD_Companions.esp";
constexpr uint32_t kAdoptableDogKeyword = 0x800;  // RD_AdoptableDog, local ID in kPlugin

// Fallout4.esm
constexpr uint32_t kDogmeat = 0x0001D15C;         // his NPC_ (GetIsID's parameter in his dialogue)
constexpr uint32_t kDogmeatRef = 0x0001D162;
constexpr uint32_t kFollowersQuest = 0x000289E4;
constexpr uint32_t kDogmeatCompanionAlias = 5;    // Followers' DogmeatCompanion
constexpr uint32_t kDogmeatQuest = 0x0006DFAF;
constexpr uint16_t kDogmeatGoHomeStage = 90;      // its fragment calls Followers.DismissDogmeatCompanion
constexpr uint32_t kWorkshopParentQuest = 0x0002058E;
constexpr uint32_t kAoQuest = 0x000260DA;         // AOScript: Dogmeat's Find stuff
constexpr uint32_t kHasBeenCompanionFaction = 0x000A1B85;
constexpr uint32_t kFollowerState = 0x00000344;   // CommonProperties.FollowerState
constexpr uint32_t kFollowerDistance = 0x00000345;
constexpr uint32_t kFollowCommand = 0x0002A106;   // iFollower_Com_Follow
constexpr uint32_t kDistanceMedium = 0x0002A10A;  // iFollower_Dist_Medium
constexpr uint32_t kPlayerHasActiveDogmeatCompanion = 0x00145868;

constexpr uint32_t kQuestFormType = 80;
constexpr uint32_t kCharacterFormType = 65;
constexpr size_t kMaxNameLength = 24;

// The adopted dog in the local player's Dogmeat slot (reference form ID), read by the GetIsID hook on any thread
std::atomic<uint32_t> s_slotDog{0};

void ShowHudMessage(const std::string& acText) noexcept
{
    // SendHUDMessage::ShowHUDMessage (CommonLibF4 id 1163005): message, sound, throttle, warning
    using TShowHUDMessage = void(const char*, const char*, bool, bool);
    POINTER_FALLOUT4(TShowHUDMessage, s_showHUDMessage, 1163006);
    s_showHUDMessage.Get()(acText.c_str(), "", false, false);
}

// --- GetIsID: Dogmeat's dialogue, commands and tricks for the dog in our own Dogmeat slot

// The condition function of GetIsID (script command 0x1048): [params] is the reference the condition runs on, the form
// to compare with is the first parameter, the answer (1/0) goes to aResult
using TGetIsIDCondition = bool(void* apParams, TESForm* apForm, void* apParam2, float& aResult);
TGetIsIDCondition* RealGetIsIDCondition = nullptr;

bool HookGetIsIDCondition(void* apParams, TESForm* apForm, void* apParam2, float& aResult)
{
    if (apParams && apForm && apForm->formID == kDogmeat)
    {
        const uint32_t dog = s_slotDog.load(std::memory_order_relaxed);
        const auto* pSubject = *static_cast<TESObjectREFR**>(apParams);
        if (dog && pSubject && pSubject->formID == dog)
        {
            aResult = 1.f;
            return true;
        }
    }
    return RealGetIsIDCondition(apParams, apForm, apParam2, aResult);
}

// --- calling Papyrus from here

// A native Papyrus function by its script and name, as the game registered it (nullptr if it didn't)
template <class Return, class Self, class... Args> Return(__fastcall* Native(const char* acpScript, const char* acpName))(BSScript::IVirtualMachine*, uint32_t, Self*, Args...)
{
    using TFunction = Return(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, Self*, Args...);
    return reinterpret_cast<TFunction>(const_cast<void*>(World::Get().ctx().at<PapyrusService>().Get(acpScript, acpName)));
}

// BSTSmartPointer of a VM object: one intrusive pointer
struct VmPointer
{
    void* pObject = nullptr;
};

void** VmTable() noexcept
{
    return *reinterpret_cast<void***>(GameVM::Get()->virtualMachine);
}

// IVirtualMachine::GetObjectHandlePolicy (slot 0x33), then IObjectHandlePolicy::GetHandleForObject (slot 7)
uint64_t VmHandleOf(uint32_t aFormType, const void* apForm) noexcept
{
    auto* pVm = GameVM::Get()->virtualMachine;
    using TGetPolicy = void*(__fastcall*)(BSScript::IVirtualMachine*);
    void* pPolicy = reinterpret_cast<TGetPolicy>(VmTable()[0x33])(pVm);
    using TGetHandle = uint64_t(__fastcall*)(void*, uint32_t, const void*);
    return reinterpret_cast<TGetHandle>((*reinterpret_cast<void***>(pPolicy))[7])(pPolicy, aFormType, apForm);
}

// A form as a script argument: its bound script object (IVirtualMachine::FindBoundObject, slot 0x1F), None without one
void SetFormArgument(BSScript::Variable& aVariable, uint32_t aFormType, const void* apForm, const char* acpScriptType) noexcept
{
    aVariable.Clear();
    if (!apForm)
        return;

    using TFindBoundObject = bool(__fastcall*)(BSScript::IVirtualMachine*, uint64_t, const char*, bool, VmPointer&, bool);
    VmPointer object;
    if (!reinterpret_cast<TFindBoundObject>(VmTable()[0x1F])(GameVM::Get()->virtualMachine, VmHandleOf(aFormType, apForm), acpScriptType, false, object, false) || !object.pObject)
        return;

    // Variable = object: its type is the object's ObjectTypeInfo (Object + 0x08), the reference moves in
    reinterpret_cast<uint64_t&>(aVariable.type) = *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(object.pObject) + 0x08);
    *reinterpret_cast<void**>(&aVariable.data) = object.pObject;
}

// The game's std::function<bool(BSScrapArray<Variable>&)> (BSTThreadScrapFunction): the game is built with Visual C++
// 2012, whose std::function is 0x18 bytes of room for a small callable, then a pointer to the callable (in that room or
// on the heap), which is a std::_Func_base (CommonLibF4's msvc::function). Ours has another layout: the game found no
// callable in it, threw bad_function_call, and nothing caught it (adopting a dog ended the game, 2026-10-17).
// The callable here is its vtable and a shared pointer to the arguments, so it always fits in the room.
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
    // std::_Func_base's virtual functions, in its order (the destructor is slot 5)
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

    // The game copies it (into its own room, or on the heap when apWhere is null) and later deletes it with DeleteThis
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

// Calls a method of the script on a quest, on the VM's own time (IVirtualMachine::DispatchMethodCall by handle, slot 0x2E)
bool CallQuestMethod(uint32_t aQuestId, const char* acpScript, const char* acpFunction, std::function<void(BSScript::Statement&)> aArguments) noexcept
{
    auto* pQuest = TESForm::GetById(aQuestId);
    if (!pQuest)
        return false;

    using TDispatch = bool(__fastcall*)(BSScript::IVirtualMachine*, uint64_t, const BSFixedString&, const BSFixedString&, const GameArguments&, const VmPointer&);

    BSFixedString script(acpScript);
    BSFixedString function(acpFunction);
    const GameArguments arguments(std::move(aArguments));
    const VmPointer noCallback;
    const bool sent = reinterpret_cast<TDispatch>(VmTable()[0x2E])(GameVM::Get()->virtualMachine, VmHandleOf(kQuestFormType, pQuest), script, function, arguments, noCallback);
    script.Release();
    function.Release();

    if (!sent)
        spdlog::error("Companions: couldn't call {}.{}", acpScript, acpFunction);
    return sent;
}

// --- game helpers

BGSBaseAlias* GetDogmeatAlias() noexcept
{
    auto* pQuest = Cast<TESQuest>(TESForm::GetById(kFollowersQuest));
    if (!pQuest)
        return nullptr;

    for (uint32_t i = 0; i < pQuest->aliases.length; ++i)
    {
        if (pQuest->aliases.data[i] && pQuest->aliases.data[i]->id == kDogmeatCompanionAlias)
            return pQuest->aliases.data[i];
    }
    return nullptr;
}

const char* NameOf(const Actor* apDog) noexcept
{
    if (const auto* pNpc = Cast<TESNPC>(apDog->baseForm))
    {
        if (const char* pName = pNpc->fullName.value.AsAscii(); pName && *pName)
            return pName;
    }
    return "The dog";
}

void SetNpcName(TESNPC* apNpc, const std::string& acName) noexcept
{
    // BSStringPool::GetEntry<char>(Entry*& result, const char* str, bool caseSensitive), CommonLibF4 id 507142. The old
    // entry isn't released: the record's own string may be shared, leaking one reference is harmless.
    using TGetEntry = void(BSFixedString::Data*&, const char*, bool);
    POINTER_FALLOUT4(TGetEntry, s_getEntry, 507143);

    BSFixedString::Data* pEntry = nullptr;
    s_getEntry.Get()(pEntry, acName.c_str(), false);
    if (pEntry)
        apNpc->fullName.value.data = pEntry;
}

float GlobalValue(uint32_t aId) noexcept
{
    const auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(aId));
    return pGlobal ? pGlobal->f : 0.f;
}

void SetGlobalValue(uint32_t aId, float aValue) noexcept
{
    if (auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(aId)))
        pGlobal->f = aValue;
}

// ObjectReference.SetValue(akAV, afValue)
void SetActorValue(Actor* apActor, uint32_t aActorValueId, float aValue) noexcept
{
    static auto s_setValue = Native<void, TESObjectREFR, ActorValueInfo*, float>("ObjectReference", "SetValue");
    auto* pActorValue = Cast<ActorValueInfo>(TESForm::GetById(aActorValueId));
    if (s_setValue && pActorValue)
        s_setValue(GameVM::Get()->virtualMachine, 0, apActor, pActorValue, aValue);
}

// The plugin a form comes from and its ID in it
const Mod* ModOf(uint32_t aFormId, uint32_t& aLocalId) noexcept
{
    const bool lite = (aFormId >> 24) == 0xFE;
    aLocalId = lite ? aFormId & 0xFFF : aFormId & 0xFFFFFF;
    for (auto* pEntry = &ModManager::Get()->mods.entry; pEntry && pEntry->data; pEntry = pEntry->next)
    {
        const Mod* pMod = pEntry->data;
        if (!pMod->IsLoaded() || pMod->IsLite() != lite)
            continue;
        if (lite ? pMod->liteId == ((aFormId >> 12) & 0xFFF) : pMod->standardId == (aFormId >> 24))
            return pMod;
    }
    return nullptr;
}

std::string Trim(std::string aText) noexcept
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    aText.erase(aText.begin(), std::find_if(aText.begin(), aText.end(), notSpace));
    aText.erase(std::find_if(aText.rbegin(), aText.rend(), notSpace).base(), aText.end());
    return aText;
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
} // namespace

static TiltedPhoques::Initializer s_companionHooks(
    []()
    {
        // The game's script command table (CommonLibF4 id 75173): entry 72 is GetIsID, its condition function at 0x40
        POINTER_FALLOUT4(uint8_t, s_scriptFunctions, 75174);
        uint8_t* pEntry = s_scriptFunctions.Get() + 72 * 0x50;
        const char* pName = *reinterpret_cast<const char**>(pEntry);
        if (!pName || std::strcmp(pName, "GetIsID") != 0)
        {
            spdlog::error("Companions: GetIsID not found in the script command table ({})", pName ? pName : "null");
            return;
        }
        RealGetIsIDCondition = *reinterpret_cast<TGetIsIDCondition**>(pEntry + 0x40);
        TP_HOOK(&RealGetIsIDCondition, HookGetIsIDCondition);
    });

CompanionService::CompanionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CompanionService::OnUpdate>(this);
    GetEventDispatcher_TESQuestStageEvent()->RegisterSink(this);
}

BGSKeyword* CompanionService::GetAdoptableKeyword() const noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(kPlugin);
    if (!pMod || !pMod->IsLoaded())
        return nullptr;
    return Cast<BGSKeyword>(TESForm::GetById(pMod->GetFormId(kAdoptableDogKeyword)));
}

bool CompanionService::IsAdoptable(const TESObjectREFR* apObject) const noexcept
{
    if (!apObject || !apObject->baseForm)
        return false;

    const auto* pNpc = Cast<TESNPC>(apObject->baseForm);
    const BGSKeyword* pKeyword = GetAdoptableKeyword();
    return pNpc && pKeyword && pNpc->keywordForm.HasKeyword(pKeyword);
}

Actor* CompanionService::GetDogmeatSlotActor() const noexcept
{
    static auto s_getReference = Native<TESObjectREFR*, BGSBaseAlias>("ReferenceAlias", "GetReference");
    BGSBaseAlias* pAlias = GetDogmeatAlias();
    if (!s_getReference || !pAlias)
        return nullptr;
    return Cast<Actor>(s_getReference(GameVM::Get()->virtualMachine, 0, pAlias));
}

Actor* CompanionService::GetDog() const noexcept
{
    Actor* pActor = GetDogmeatSlotActor();
    return IsAdoptable(pActor) ? pActor : nullptr;
}

bool CompanionService::OnActivate(TESObjectREFR* apObject, Actor* apActivator) noexcept
{
    if (!apActivator || apActivator != PlayerCharacter::Get() || !IsAdoptable(apObject))
        return false;

    auto* pDog = Cast<Actor>(apObject);
    if (!pDog || pDog->IsDead())
        return false;

    // Your own dog: his talk (commands, tricks, Find stuff, its inventory, go home)
    Actor* pMine = GetDog();
    if (pMine == pDog)
        return false;

    if (pMine)
    {
        ShowHudMessage(fmt::format("You already have {}.", NameOf(pMine)));
        return true;
    }

    Adopt(pDog);
    return true;
}

// FollowersScript.SetDogmeatCompanion without its DismissCompanion (the human companion stays)
bool CompanionService::Adopt(Actor* apDog) noexcept
{
    static auto s_forceRefTo = Native<void, BGSBaseAlias, TESObjectREFR*>("ReferenceAlias", "ForceRefTo");
    static auto s_ignoreFriendlyHits = Native<void, TESObjectREFR, bool>("ObjectReference", "IgnoreFriendlyHits");
    static auto s_setPlayerTeammate = Native<void, Actor, bool, bool, bool>("Actor", "SetPlayerTeammate");

    BGSBaseAlias* pAlias = GetDogmeatAlias();
    if (!apDog || !pAlias || !s_forceRefTo || !s_ignoreFriendlyHits || !s_setPlayerTeammate)
    {
        spdlog::error("Companions: can't adopt {:X} (alias {}, natives {} {} {})", apDog ? apDog->formID : 0, static_cast<void*>(pAlias), s_forceRefTo != nullptr,
                      s_ignoreFriendlyHits != nullptr, s_setPlayerTeammate != nullptr);
        return false;
    }

    // Vanilla Dogmeat with us: out of the slot below, he stops following and his own packages take him home
    if (Actor* pCurrent = GetDogmeatSlotActor(); pCurrent && pCurrent != apDog)
    {
        Unfollow(pCurrent, false);
        ShowHudMessage(fmt::format("{} is heading home.", NameOf(pCurrent)));
    }

    auto* pVm = GameVM::Get()->virtualMachine;
    s_forceRefTo(pVm, 0, pAlias, apDog);
    s_ignoreFriendlyHits(pVm, 0, apDog, true);
    s_setPlayerTeammate(pVm, 0, apDog, true, true, true);
    if (auto* pFaction = Cast<TESFaction>(TESForm::GetById(kHasBeenCompanionFaction)))
        apDog->SetFactionRank(pFaction, 0);
    SetActorValue(apDog, kFollowerState, GlobalValue(kFollowCommand));
    SetActorValue(apDog, kFollowerDistance, GlobalValue(kDistanceMedium));
    SetGlobalValue(kPlayerHasActiveDogmeatCompanion, 1.f);
    // Find stuff: AOScript starts its timer when Dogmeat joins or loads
    CallQuestMethod(kAoQuest, "AOScript", "DogmeatLoading", [](BSScript::Statement& aStatement)
    {
        aStatement.SetSize(1);
        aStatement.vars[0].Set(true);
    });

    s_slotDog = apDog->formID;
    m_slotActorId = apDog->formID;

    spdlog::info("Companions: adopted {:X} ({})", apDog->formID, NameOf(apDog));
    auto* pNpc = Cast<TESNPC>(apDog->baseForm);
    if (pNpc && !m_names.contains(KeyOf(pNpc)))
        AskForName(pNpc);
    else
        ShowHudMessage(fmt::format("{} is coming with you.", NameOf(apDog)));
    return true;
}

// FollowersScript.ClearDogmeatCompanion's steps
void CompanionService::Unfollow(Actor* apDog, bool aClearSlot) noexcept
{
    static auto s_clear = Native<void, BGSBaseAlias>("ReferenceAlias", "Clear");
    static auto s_ignoreFriendlyHits = Native<void, TESObjectREFR, bool>("ObjectReference", "IgnoreFriendlyHits");
    static auto s_setPlayerTeammate = Native<void, Actor, bool, bool, bool>("Actor", "SetPlayerTeammate");
    if (!apDog || !s_clear || !s_ignoreFriendlyHits || !s_setPlayerTeammate)
        return;

    auto* pVm = GameVM::Get()->virtualMachine;
    s_ignoreFriendlyHits(pVm, 0, apDog, false);
    s_setPlayerTeammate(pVm, 0, apDog, false, true, false);
    if (aClearSlot)
    {
        if (BGSBaseAlias* pAlias = GetDogmeatAlias())
            s_clear(pVm, 0, pAlias);
        SetGlobalValue(kPlayerHasActiveDogmeatCompanion, 0.f);
    }
}

void CompanionService::Release(bool aToSettlement) noexcept
{
    Actor* pDog = GetDog();
    if (!pDog)
        return;

    // As his DismissDogmeatCompanion: the settlement list (WorkshopNPCScript takes the dog there when one is picked);
    // without a pick it walks back to its own spot
    if (aToSettlement)
    {
        CallQuestMethod(kWorkshopParentQuest, "WorkshopParentScript", "AddPermanentActorToWorkshopPlayerChoice", [dogId = pDog->formID](BSScript::Statement& aStatement)
        {
            aStatement.SetSize(2);
            SetFormArgument(aStatement.vars[0], kCharacterFormType, TESForm::GetById(dogId), "Actor");
            aStatement.vars[1].Set(false);
        });
    }

    Unfollow(pDog, true);
    s_slotDog = 0;
    m_slotActorId = 0;

    spdlog::info("Companions: released {:X} ({}){}", pDog->formID, NameOf(pDog), aToSettlement ? ", settlement list" : "");
    ShowHudMessage(fmt::format("{} is heading home.", NameOf(pDog)));
}

BSTEventResult CompanionService::OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*)
{
    // "Go home" said to our dog. A stage set for us by another player's game isn't our dog's business.
    if (apEvent->formId != kDogmeatQuest || apEvent->stageId != kDogmeatGoHomeStage || ScopedQuestOverride::IsOverriden())
        return BSTEventResult::kOk;

    spdlog::info("Companions: DogmeatQuest stage {} (go home)", apEvent->stageId);

    // The fragment runs after the event (and does nothing for an adopted dog): ours on the next frame
    m_world.GetRunner().Queue([this]() { Release(true); });
    return BSTEventResult::kOk;
}

void CompanionService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Companion");
    m_nextCheck -= acEvent.Delta;
    if (m_nextCheck > 0.0)
        return;
    m_nextCheck = 1.0;

    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->baseForm)
        return;

    // Without RD_Companions.esp none of this is on: vanilla Dogmeat stays as he is, unnamed
    if (!GetAdoptableKeyword())
    {
        s_slotDog = 0;
        return;
    }

    LoadNames();
    ApplyNames();

    // Who's in our Dogmeat slot now
    Actor* pSlot = GetDogmeatSlotActor();
    const uint32_t slotId = pSlot ? pSlot->formID : 0;
    s_slotDog = IsAdoptable(pSlot) ? slotId : 0;
    if (slotId == m_slotActorId)
        return;

    // An adopted dog pushed out of the slot (vanilla Dogmeat recruited by his own dialogue): it's a stray again
    if (auto* pPrevious = Cast<Actor>(TESForm::GetById(m_slotActorId)); pPrevious && IsAdoptable(pPrevious))
    {
        Unfollow(pPrevious, false);
        spdlog::info("Companions: {:X} ({}) left the Dogmeat slot to {:X}", pPrevious->formID, NameOf(pPrevious), slotId);
        ShowHudMessage(fmt::format("{} is heading home.", NameOf(pPrevious)));
    }
    m_slotActorId = slotId;

    // Vanilla Dogmeat joined: he gets a name too
    if (pSlot && pSlot->formID == kDogmeatRef)
    {
        auto* pNpc = Cast<TESNPC>(pSlot->baseForm);
        if (pNpc && !m_names.contains(KeyOf(pNpc)))
            AskForName(pNpc);
    }
}

// --- names

std::string CompanionService::KeyOf(const TESNPC* apNpc) const noexcept
{
    uint32_t localId = 0;
    const Mod* pMod = apNpc ? ModOf(apNpc->formID, localId) : nullptr;
    return pMod ? fmt::format("{}:{:X}", pMod->filename, localId) : std::string{};
}

void CompanionService::AskForName(TESNPC* apNpc) noexcept
{
    m_promptNpcId = apNpc->formID;
    const std::string current = apNpc->fullName.value.AsAscii() ? apNpc->fullName.value.AsAscii() : "";
    World::Get().ctx().at<OverlayService>().SendEvent("companionName", fmt::format(R"({{"id":"{:X}","name":{}}})", apNpc->formID, MiniJson::Quote(current)));
    ShowOverlayCursor(true);
    spdlog::info("Companions: asking for a name for {:X} ({})", apNpc->formID, current);
}

void CompanionService::CloseNamePrompt() noexcept
{
    m_promptNpcId = 0;
    ShowOverlayCursor(false);
}

void CompanionService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    const auto colon = acPayload.find(':');
    const uint32_t npcId = static_cast<uint32_t>(std::strtoul(acPayload.substr(0, colon).c_str(), nullptr, 16));
    auto* pNpc = Cast<TESNPC>(TESForm::GetById(npcId));
    if (!pNpc || npcId != m_promptNpcId)
    {
        CloseNamePrompt();
        return;
    }

    const std::string key = KeyOf(pNpc);
    std::string name = acRequest == "name" && colon != std::string::npos ? Trim(acPayload.substr(colon + 1)) : std::string{};
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return c < 0x20; }), name.end());
    if (name.size() > kMaxNameLength)
        name.resize(kMaxNameLength);

    // "keep" (or an empty name): the name it has, and no more asking
    if (name.empty())
        name = pNpc->fullName.value.AsAscii() ? pNpc->fullName.value.AsAscii() : "Dog";

    if (!key.empty())
    {
        m_names[key] = name;
        SaveNames();
    }
    SetNpcName(pNpc, name);
    CloseNamePrompt();

    spdlog::info("Companions: {:X} is named {}", npcId, name);
    ShowHudMessage(fmt::format("{} is coming with you.", name));
}

// Kept per character (one save folder can hold several): "character<TAB>plugin:localId<TAB>name" lines
void CompanionService::LoadNames() noexcept
{
    const auto* pPlayerNpc = Cast<TESNPC>(PlayerCharacter::Get()->baseForm);
    const std::string character = pPlayerNpc && pPlayerNpc->fullName.value.AsAscii() ? pPlayerNpc->fullName.value.AsAscii() : "";
    const auto file = SaveFolder::GetGameUserFolder() / SaveFolder::GetLocalSavePath() / "companion-names.txt";
    if (file == m_namesFile && character == m_character)
        return;

    m_namesFile = file;
    m_character = character;
    m_names.clear();

    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (second == std::string::npos || line.compare(0, first, character) != 0 || first != character.size())
            continue;
        m_names[line.substr(first + 1, second - first - 1)] = line.substr(second + 1);
    }
    if (!m_names.empty())
        spdlog::info("Companions: {} dog name(s) for {}", m_names.size(), character);
}

void CompanionService::SaveNames() const noexcept
{
    if (m_namesFile.empty())
        return;

    // The other characters' lines stay as they are
    std::vector<std::string> lines;
    {
        std::ifstream in(m_namesFile);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.rfind(m_character + '\t', 0) != 0)
                lines.push_back(line);
        }
    }
    for (const auto& [key, name] : m_names)
        lines.push_back(fmt::format("{}\t{}\t{}", m_character, key, name));

    std::error_code error;
    std::filesystem::create_directories(m_namesFile.parent_path(), error);
    std::ofstream out(m_namesFile, std::ios::trunc);
    for (const auto& line : lines)
        out << line << '\n';
}

void CompanionService::ApplyNames() noexcept
{
    for (const auto& [key, name] : m_names)
    {
        const auto colon = key.rfind(':');
        const Mod* pMod = ModManager::Get()->GetByName(key.substr(0, colon).c_str());
        if (!pMod || !pMod->IsLoaded())
            continue;
        auto* pNpc = Cast<TESNPC>(TESForm::GetById(pMod->GetFormId(std::strtoul(key.c_str() + colon + 1, nullptr, 16))));
        const char* pCurrent = pNpc ? pNpc->fullName.value.AsAscii() : nullptr;
        if (pNpc && (!pCurrent || name != pCurrent))
            SetNpcName(pNpc, name);
    }
}

#endif

#include <Services/BetterInteractService.h>

#if TP_FALLOUT4

#include <Services/EmoteService.h>
#include <Services/GrappleService.h>
#include <Services/ParkourService.h>
#include <Services/PipboyMapService.h>
#include <Services/TransportService.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyMovementAction.h>
#include <Messages/RequestMovementAction.h>
#include <World.h>
#include <Utils.h>

#include <PlayerCharacter.h>
#include <Components/TESModel.h>
#include <Forms/TESForm.h>
#include <Games/Misc/Lock.h>
#include <Games/Fallout4/BikeLink.h>
#include <Games/Fallout4/DualWieldPose.h>
#include <Games/Fallout4/NodeTools.h>
#include <Interface/HudMarker.h>
#include <Interface/UI.h>
#include <Misc/BSFixedString.h>

#include <chrono>
#include <cmath>
#include <filesystem>

namespace
{
using BetterInteractClip::Item;
using BetterInteractClip::Kind;
using BetterInteractClip::View;

// What Better Interact's other settings' file is (BetterInteract.cpp), read again when it changes, looked at this often (s)
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr wchar_t kSettingsSection[] = L"BetterInteract";
constexpr float kSettingsInterval = 2.f;

// TESObjectREFR: flora already harvested (TESForm flags)
constexpr uint32_t kFormHarvested = 0x2000;
// TESBoundObject: its bounds, six int16s (min, max), in the model's axes
constexpr uintptr_t kBoundOffset = 0x20;
// TESBipedModelForm: the ground models, BGSModelMaterialSwap worldModel[2] (a TESModel each, 0x40 apart)
constexpr uintptr_t kWorldModelOffset = 0x8;
// TESFlora: TESProduceForm at 0x148, its produceItem at +0x10 (CommonLibF4, TESFlora's size checked 0x168)
constexpr uintptr_t kFloraProduceOffset = 0x148 + 0x10;
// PlayerCamera: worldFOV, firstPersonFOV
constexpr uintptr_t kWorldFovOffset = 0x168;
constexpr uintptr_t kFirstPersonFovOffset = 0x16C;
// A thing bigger than this (its world bound's radius) is reached for where the crosshair is on it, not at its middle
constexpr float kBigThing = 30.f;
// A copy with more nodes than this is no thing to pick up by hand: nothing in the hand
constexpr size_t kMostNodes = 400;
// A hand that hasn't been put on its skeleton for this long is over (its bone tree isn't updating), s
constexpr float kUnposedSeconds = 2.f;
// The item's copy's root and anything in it of the skeleton's names is renamed with this (a weapon's root can be called WEAPON)
constexpr char kCopyName[] = "BetterInteractItem";

// Actor state: move mode's swimming bit; flags2's gun state as the dual wield logs show it: 4 for a reload's length after a shot,
// 6 down the sights, 7 for a moment after a shot from the hip, 8 firing down the sights (0 is the plain ready one)
constexpr uint32_t kMoveModeSwimming = 1u << 10;
constexpr uint32_t kGunReload = 4;
constexpr uint32_t kGunSighted = 6;
constexpr uint32_t kGunFire = 7;
constexpr uint32_t kGunFireSighted = 8;

constexpr const char* kBlockingMenus[] = {"PipboyMenu", "DialogueMenu", "VATSMenu", "WorkshopMenu", "LooksMenu", "ContainerMenu", "BarterMenu", "ExamineMenu",
                                          "TerminalMenu", "LockpickingMenu", "PauseMenu", "FavoritesMenu", "LoadingMenu", "MainMenu",
                                          "SleepWaitMenu", "MessageBoxMenu", "CookingMenu", "PowerArmorModMenu", "LevelUpMenu", "SPECIALMenu"};

bool GraphBool(const Actor* apActor, const char* acpName) noexcept
{
    BSFixedString name(acpName);
    bool value = false;
    auto* pActor = const_cast<Actor*>(apActor);
    return pActor->animationGraphHolder.GetVariableBool(&name, &value) && value;
}

// The world camera now, and what first person's own field of view does to a place on the screen
View CurrentView(const Actor* apActor) noexcept
{
    View view{};
    NiPoint3 eye{};
    if (HudMarker::GetCameraPosition(eye))
        view.Eye = glm::vec3(eye);
    else
        view.Eye = glm::vec3(apActor->position) + glm::vec3(0.f, 0.f, 120.f);

    float m[4][4];
    bool known = false;
    if (HudMarker::GetWorldToClip(m))
    {
        const glm::vec3 forward(m[3][0], m[3][1], m[3][2]);
        const glm::vec3 right(m[0][0], m[0][1], m[0][2]);
        if (glm::length(forward) > 1e-6f && glm::length(right) > 1e-6f)
        {
            view.Forward = glm::normalize(forward);
            view.Right = glm::normalize(right);
            known = true;
        }
    }
    if (!known)
    {
        const float pitch = apActor->rotation.x;
        const float yaw = apActor->rotation.z;
        view.Forward = glm::vec3(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
        view.Right = glm::vec3(std::cos(yaw), -std::sin(yaw), 0.f);
    }
    view.Up = glm::normalize(glm::cross(view.Right, view.Forward));

    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    if (const uint8_t* pCamera = *s_playerCamera.Get())
    {
        const float world = *reinterpret_cast<const float*>(pCamera + kWorldFovOffset);
        const float first = *reinterpret_cast<const float*>(pCamera + kFirstPersonFovOffset);
        if (world > 10.f && world < 170.f && first > 10.f && first < 170.f)
            view.FovRatio = std::tan(glm::radians(first) * 0.5f) / std::tan(glm::radians(world) * 0.5f);
    }
    return view;
}

glm::vec3 BodyForwardOf(const Actor* apActor) noexcept
{
    const float yaw = apActor->rotation.z;
    return glm::vec3(std::sin(yaw), std::cos(yaw), 0.f);
}

// What the hand reaches for: a small thing's middle, or where the crosshair is on a big one (a door)
glm::vec3 TargetOf(const TESObjectREFR* apObject, uint8_t* ap3D, const View& acView) noexcept
{
    glm::vec3 centre(apObject->position);
    float radius = 0.f;
    if (ap3D)
    {
        const auto* pBound = reinterpret_cast<const float*>(ap3D + NodeTools::kWorldBoundOffset);
        if (std::isfinite(pBound[0]) && std::isfinite(pBound[1]) && std::isfinite(pBound[2]) && std::isfinite(pBound[3]) && pBound[3] > 0.f)
        {
            centre = glm::vec3(pBound[0], pBound[1], pBound[2]);
            radius = pBound[3];
        }
    }
    if (radius <= kBigThing)
        return centre;
    return acView.Eye + acView.Forward * std::max(glm::dot(centre - acView.Eye, acView.Forward), 20.f);
}

// The copy's names: nothing in it for a skeleton's bone lookups to find. How many nodes it has
size_t PrepareCopy(uint8_t* apNode) noexcept
{
    const std::string name = NodeTools::GetName(apNode);
    if (_stricmp(name.c_str(), "WEAPON") == 0 || _stricmp(name.c_str(), "WeaponLeft") == 0 || name.rfind("LArm_", 0) == 0 || name.rfind("RArm_", 0) == 0)
        NodeTools::SetName(apNode, fmt::format("{}{}", kCopyName, name).c_str());
    size_t count = 1;
    NodeTools::ForEachChild(apNode, [&count](uint8_t* apChild) { count += PrepareCopy(apChild); });
    return count;
}

// Its model's bounds (the form's own)
void BoundsOf(const TESForm* apForm, glm::vec3& aMin, glm::vec3& aMax) noexcept
{
    const auto* pBound = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(apForm) + kBoundOffset);
    aMin = glm::vec3(pBound[0], pBound[1], pBound[2]);
    aMax = glm::vec3(pBound[3], pBound[4], pBound[5]);
}

// The model a form shows lying in the world (armor's its ground model): empty without one
std::string ModelPathOf(TESForm* apForm) noexcept
{
    const char* pPath = nullptr;
    if (apForm->formType == FormType::Armor)
    {
        if (auto* pBiped = Cast<TESBipedModelForm>(apForm))
            pPath = reinterpret_cast<TESModel*>(reinterpret_cast<uint8_t*>(pBiped) + kWorldModelOffset)->cModel.AsAscii();
    }
    else if (auto* pModel = Cast<TESModel>(apForm))
        pPath = pModel->cModel.AsAscii();
    return pPath ? pPath : "";
}

// A copy of a form's model, ours to give to the hand; nullptr if it can't be had. BSModelDB::Demand (CommonLibF4 id 1225688, as
// OffHandModel's muzzle flash): waits for the file when it isn't loaded, which a thing just seen lying in the world is
uint8_t* LoadCopy(TESForm* apForm) noexcept
{
    const std::string path = ModelPathOf(apForm);
    if (path.empty())
        return nullptr;
    struct ArgsType
    {
        int32_t LodFadeMult;
        uint32_t LoadLevel;
        uint8_t Flags;
        uint8_t Pad[3];
    };
    static_assert(sizeof(ArgsType) == 0x0C);
    using TDemand = uint32_t(const char*, uint8_t**, const ArgsType&);
    POINTER_FALLOUT4(TDemand, s_demand, 1225689);
    const ArgsType args{1, 0, 0x29, {}};
    uint8_t* pLoaded = nullptr;
    uint32_t error = s_demand.Get()(("Meshes\\" + path).c_str(), &pLoaded, args);
    if (error != 0 || !pLoaded)
    {
        NodeTools::DecRef(pLoaded);
        pLoaded = nullptr;
        error = s_demand.Get()(path.c_str(), &pLoaded, args);
    }
    if (error != 0 || !pLoaded)
    {
        NodeTools::DecRef(pLoaded);
        spdlog::info("Better Interact: couldn't load {} for the hand (error {})", path, error);
        return nullptr;
    }
    uint8_t* pCopy = NodeTools::Clone(pLoaded);
    NodeTools::IncRef(pCopy);
    NodeTools::DecRef(pLoaded);
    return pCopy;
}

// The hand's hold of that form's model
void SetHold(Item& aItem, TESForm* apForm, float aScale) noexcept
{
    if (apForm->formType == FormType::Weapon)
    {
        aItem.OnWeaponNode = true;
        aItem.InHand = BetterInteractClip::HoldWeapon(aScale);
        return;
    }
    glm::vec3 min{}, max{};
    BoundsOf(apForm, min, max);
    aItem.OnWeaponNode = false;
    aItem.InHand = BetterInteractClip::HoldInHand(min, max, aScale);
}

// A thing taken by hand, what the hand's motion is for it (false: no hand for it)
thread_local int t_activateDepth = 0;

bool KindOf(const TESObjectREFR* apObject, Kind& aKind) noexcept
{
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
    case FormType::Component: aKind = Kind::kGrab; return true;
    case FormType::Flora:
        if (apObject->flags & kFormHarvested)
            return false;
        aKind = Kind::kGrab;
        return true;
    case FormType::Door:
        if (const Lock* pLock = apObject->GetLock(); pLock && pLock->IsLocked())
            return false;
        aKind = Kind::kPush;
        return true;
    case FormType::Activator:
    case FormType::TalkingActivator: aKind = Kind::kPress; return true;
    default: return false;
    }
}
} // namespace

BetterInteractService::BetterInteractService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&BetterInteractService::OnUpdate>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&BetterInteractService::OnNotifyMovementAction>(this);
}

void BetterInteractService::LoadSettings() noexcept
{
    // the game's folder, as BetterInteract.cpp finds it
    const auto path = std::filesystem::current_path() / L"Data" / kSettingsFile;
    static std::filesystem::file_time_type s_time{};
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    const bool there = !ec;
    if (m_settingsRead && (there ? time : std::filesystem::file_time_type{}) == s_time)
        return;
    s_time = there ? time : std::filesystem::file_time_type{};
    m_settingsRead = true;

    Settings settings{};
    if (there)
    {
        const std::wstring file = path.wstring();
        const auto get = [&file](const wchar_t* acpKey, bool aDefault)
        {
            wchar_t text[16]{};
            GetPrivateProfileStringW(kSettingsSection, acpKey, aDefault ? L"1" : L"0", text, static_cast<DWORD>(std::size(text)), file.c_str());
            return std::wcstof(text, nullptr) != 0.f;
        };
        settings.Hands = get(L"bHands", settings.Hands);
        settings.Grab = get(L"bHandGrab", settings.Grab);
        settings.Press = get(L"bHandPress", settings.Press);
        settings.Push = get(L"bHandPush", settings.Push);
        settings.ItemInHand = get(L"bItemInHand", settings.ItemInHand);
        settings.Pull = get(L"bItemFlies", settings.Pull);
        settings.ThirdPerson = get(L"bHandsThirdPerson", settings.ThirdPerson);
        settings.Others = get(L"bHandsOtherPlayers", settings.Others);
    }
    m_settings = settings;
    spdlog::info("Better Interact: hands {} (grab {}, press {}, push {}, item in hand {}, it flies {}, third person {}, other players {})", settings.Hands ? "on" : "off", settings.Grab,
                 settings.Press, settings.Push, settings.ItemInHand, settings.Pull, settings.ThirdPerson, settings.Others);
}

const char* BetterInteractService::WhyNot(const Actor* apActor, uint8_t* apRoot, bool aLocal) const noexcept
{
    const auto& state = apActor->actorState;
    if (apActor->IsDead() || state.IsBleedingOut())
        return "down";
    if (apActor->IsInPowerArmor())
        return "in power armor";
    if (state.GetMoveMode() & kMoveModeSwimming)
        return "swimming";
    // knocked down or using furniture
    if (((state.flags1 >> 21) & 0xF) != 0 || ((state.flags2 >> 18) & 0x3) != 0)
        return "knocked down or in furniture";
    if (DualWieldPose::HasArms(apRoot))
        return "the left hand is busy (dual wield, the grapple)";
    if (!aLocal)
        return nullptr;

    // the local player's own: what the others' games don't know of
    if (EmoteService::IsPlayerEmoting())
        return "emoting";
    if (m_world.ctx().at<ParkourService>().IsPlayerMoving())
        return "in a parkour move";
    if (GrappleService::HoldsPlayer())
        return "on the grapple";
    if (BikeLink::IsOnBike())
        return "on the bike";
    if (m_world.ctx().at<PipboyMapService>().IsShowing())
        return "the Pip-Boy map";
    if (UI* pUi = UI::Get())
    {
        for (const char* pMenu : kBlockingMenus)
        {
            if (pUi->GetMenuOpen(BSFixedString(pMenu)))
                return pMenu;
        }
    }
    const uint32_t gunState = state.GetGunState();
    if (gunState == kGunSighted || gunState == kGunFire || gunState == kGunFireSighted)
        return "down the sights or firing";
    if (state.GetMeleeAttackState() != 0)
        return "swinging";
    if (gunState == kGunReload || GraphBool(apActor, "isReloading"))
        return "reloading";
    if (GraphBool(apActor, "bAnimationDriven") || GraphBool(apActor, "bIsSynced") || GraphBool(apActor, "pipboyUp"))
        return "an animation has the character";
    return nullptr;
}

void BetterInteractService::BeforeActivate(TESObjectREFR* apObject, TESObjectREFR* apActivator, bool aFromScript, bool aLooping) noexcept
{
    // Only the outermost Activate looked at (a parent's children, a script's), counted on each thread, and only the player's. Other threads' are NPCs'. It used to go by the
    // thread the updates run on as well, and no log of any run has a hand or a refusal in it, for all that was pressed.
    if (t_activateDepth++ > 0 || !m_settings.Hands)
        return;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || apActivator != pPlayer)
        return;
    // What the player's own key really passes, the first few times
    if (m_activatesLogged < 12)
    {
        ++m_activatesLogged;
        spdlog::info("Better Interact: the player activates {:X}, form type {}, from script {}, looping {}, thread {} (updates on {})", apObject ? apObject->formID : 0,
                     apObject && apObject->baseForm ? static_cast<int>(apObject->baseForm->formType) : -1, aFromScript, aLooping, GetCurrentThreadId(), m_mainThread);
    }
    if (aFromScript || aLooping || !apObject || !apObject->baseForm || !pPlayer->parentCell)
        return;
    Kind kind{};
    if (!KindOf(apObject, kind))
        return;
    if (kind == Kind::kGrab ? !m_settings.Grab : kind == Kind::kPress ? !m_settings.Press : !m_settings.Push)
        return;
    // Off the update's thread the models can't be touched: the hand reaches next frame with nothing copied to hold
    if (GetCurrentThreadId() != m_mainThread)
    {
        Reach reach{};
        reach.Valid = true;
        reach.Which = kind;
        reach.ObjectId = apObject->formID;
        reach.BaseId = apObject->baseForm->formID;
        reach.FirstPerson = NodeTools::IsFirstPerson();
        const std::scoped_lock lock(m_otherThreadLock);
        m_otherThreadReaches.push_back(reach);
        return;
    }

    const bool firstPerson = NodeTools::IsFirstPerson();
    if (!firstPerson && !m_settings.ThirdPerson)
        return;
    uint8_t* pRoot = firstPerson ? NodeTools::GetFirstPersonRoot(pPlayer) : NodeTools::GetThirdPersonRoot(pPlayer);
    if (!pRoot)
        return;
    if (const char* pWhy = WhyNot(pPlayer, pRoot, true))
    {
        spdlog::info("Better Interact: no hand for {:X}: {}", apObject->formID, pWhy);
        return;
    }

    Reach reach{};
    reach.Valid = true;
    reach.Which = kind;
    reach.ObjectId = apObject->formID;
    reach.BaseId = apObject->baseForm->formID;
    reach.FirstPerson = firstPerson;
    uint8_t* p3D = NodeTools::GetThirdPersonRoot(apObject);
    reach.HasTarget = true;
    reach.Target = TargetOf(apObject, p3D, CurrentView(pPlayer));

    if (kind == Kind::kGrab && m_settings.ItemInHand)
    {
        if (apObject->baseForm->formType == FormType::Flora)
        {
            // a plant's produce, not the plant: its model is loaded on the update
            auto* pProduce = *reinterpret_cast<TESForm**>(reinterpret_cast<uint8_t*>(apObject->baseForm) + kFloraProduceOffset);
            reach.ModelFormId = pProduce ? pProduce->formID : 0;
        }
        else if (p3D)
        {
            // Now, while it's there: the game's Activate takes it away. A copy of what's lying there, its mods' parts and all
            if (uint8_t* pCopy = NodeTools::Clone(p3D))
            {
                NodeTools::IncRef(pCopy);
                const size_t nodes = PrepareCopy(pCopy);
                if (nodes > kMostNodes)
                {
                    NodeTools::DecRef(pCopy);
                    spdlog::info("Better Interact: {:X} has {} nodes, nothing in the hand", apObject->formID, nodes);
                }
                else
                {
                    NodeTools::SetName(pCopy, kCopyName);
                    reach.Holding.pModel = pCopy;
                    const NodeTools::Transform lay = NodeTools::Read(p3D, NodeTools::kWorldOffset);
                    SetHold(reach.Holding, apObject->baseForm, lay.Scale);
                    reach.Holding.Lay = m_settings.Pull;
                    reach.Holding.LayAt = lay;
                }
            }
        }
    }
    m_preparing = reach;
}

void BetterInteractService::AfterActivate(bool aActivated) noexcept
{
    if (--t_activateDepth > 0 || GetCurrentThreadId() != m_mainThread || !m_preparing.Valid)
        return;
    Reach reach = m_preparing;
    m_preparing = Reach{};
    // a thing that wasn't taken isn't in the hand (a press still happened: a script can answer it with no activation)
    if (reach.Which == Kind::kGrab && !aActivated)
    {
        if (reach.Holding.pModel)
            NodeTools::DecRef(reach.Holding.pModel);
        spdlog::info("Better Interact: {:X} wasn't taken, no hand", reach.ObjectId);
        return;
    }
    m_reaches.push_back(reach);
}

void BetterInteractService::Share(const Reach& acReach) noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;
    RequestMovementAction request{};
    request.Type = RequestMovementAction::kInteract;
    request.Interact.Kind = static_cast<uint8_t>(acReach.Which);
    // (a form they can't name: the hand without the item)
    if (acReach.Which == Kind::kGrab)
    {
        const uint32_t itemId = acReach.ModelFormId ? acReach.ModelFormId : acReach.BaseId;
        m_world.GetModSystem().GetServerModId(itemId, request.Interact.Item);
    }
    request.Interact.HasTarget = acReach.HasTarget;
    for (int i = 0; i < 3; ++i)
        request.Interact.Target[i] = acReach.Target[i];
    transport.Send(request);
}

void BetterInteractService::StartLocal(Reach& aReach) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || aReach.FirstPerson != NodeTools::IsFirstPerson())
    {
        if (aReach.Holding.pModel)
            NodeTools::DecRef(aReach.Holding.pModel);
        return;
    }
    uint8_t* pRoot = aReach.FirstPerson ? NodeTools::GetFirstPersonRoot(pPlayer) : NodeTools::GetThirdPersonRoot(pPlayer);
    if (!pRoot)
    {
        if (aReach.Holding.pModel)
            NodeTools::DecRef(aReach.Holding.pModel);
        return;
    }
    if (aReach.ModelFormId)
    {
        if (TESForm* pForm = TESForm::GetById(aReach.ModelFormId))
        {
            if (uint8_t* pCopy = LoadCopy(pForm))
            {
                PrepareCopy(pCopy);
                NodeTools::SetName(pCopy, kCopyName);
                aReach.Holding.pModel = pCopy;
                SetHold(aReach.Holding, pForm, 1.f);
            }
        }
    }
    const char* kNames[] = {"grab", "press", "push"};
    spdlog::info("Better Interact: {} {:X} ({} person, {}, target ({:.0f}, {:.0f}, {:.0f}))", kNames[static_cast<size_t>(aReach.Which)], aReach.ObjectId, aReach.FirstPerson ? "first" : "third",
                 aReach.Holding.pModel ? (aReach.Holding.Lay ? "its copy flies to the hand" : "its copy in the hand") : "nothing in the hand", aReach.Target.x, aReach.Target.y, aReach.Target.z);

    const View view = CurrentView(pPlayer);
    const glm::vec3* pTarget = aReach.HasTarget ? &aReach.Target : nullptr;
    if (!BetterInteractClip::Start(pRoot, aReach.FirstPerson, aReach.Which, pTarget, aReach.Holding, view, BodyForwardOf(pPlayer)))
        return;
    const auto it = std::find_if(m_playing.begin(), m_playing.end(), [pRoot](const Playing& acPlaying) { return acPlaying.pRoot == pRoot; });
    if (it != m_playing.end())
        *it = Playing{pPlayer->formID, pRoot, true, aReach.FirstPerson, false};
    else
        m_playing.push_back(Playing{pPlayer->formID, pRoot, true, aReach.FirstPerson, false});
    Share(aReach);
}

void BetterInteractService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    m_mainThread = GetCurrentThreadId();
    if ((m_settingsCheck -= static_cast<float>(acEvent.Delta)) <= 0.f)
    {
        m_settingsCheck = kSettingsInterval;
        LoadSettings();
    }

    for (Reach& reach : m_reaches)
        StartLocal(reach);
    m_reaches.clear();
    {
        const std::scoped_lock lock(m_otherThreadLock);
        for (Reach& reach : m_otherThreadReaches)
            StartLocal(reach);
        m_otherThreadReaches.clear();
    }

    if (m_playing.empty())
        return;
    const bool firstPersonNow = NodeTools::IsFirstPerson();
    for (size_t i = 0; i < m_playing.size();)
    {
        Playing& playing = m_playing[i];
        auto* pActor = Cast<Actor>(TESForm::GetById(playing.ActorId));
        uint8_t* pRoot = nullptr;
        if (pActor)
            pRoot = playing.FirstPerson ? NodeTools::GetFirstPersonRoot(static_cast<PlayerCharacter*>(pActor)) : NodeTools::GetThirdPersonRoot(pActor);
        // gone, the skeleton rebuilt, the view changed under it, or run its course
        const bool gone = !pActor || pRoot != playing.pRoot || (playing.Local && playing.FirstPerson != firstPersonNow);
        if (gone || BetterInteractClip::IsDone(playing.pRoot) || BetterInteractClip::SecondsSincePosed(playing.pRoot) > kUnposedSeconds)
        {
            BetterInteractClip::Stop(playing.pRoot);
            m_playing.erase(m_playing.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        BetterInteractClip::Update(playing.pRoot, playing.Local ? CurrentView(pActor) : View{}, BodyForwardOf(pActor));
        if (!playing.Fading)
        {
            if (const char* pWhy = WhyNot(pActor, playing.pRoot, playing.Local))
            {
                playing.Fading = true;
                BetterInteractClip::FadeOut(playing.pRoot);
                spdlog::info("Better Interact: the hand gives way: {}", pWhy);
            }
        }
        ++i;
    }
}

void BetterInteractService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    if (acMessage.Type != RequestMovementAction::kInteract || !m_settings.Hands || !m_settings.Others)
        return;
    if (acMessage.Interact.Kind >= static_cast<uint8_t>(Kind::kCount))
        return;
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor || !pActor->parentCell)
        return;
    uint8_t* pRoot = NodeTools::GetThirdPersonRoot(pActor);
    if (!pRoot)
        return;
    if (const char* pWhy = WhyNot(pActor, pRoot, false))
    {
        spdlog::info("Better Interact: no hand on {:X}: {}", pActor->formID, pWhy);
        return;
    }

    const Kind kind = static_cast<Kind>(acMessage.Interact.Kind);
    Item item{};
    if (kind == Kind::kGrab && m_settings.ItemInHand && acMessage.Interact.Item)
    {
        if (TESForm* pForm = TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.Interact.Item)))
        {
            if (uint8_t* pCopy = LoadCopy(pForm))
            {
                PrepareCopy(pCopy);
                NodeTools::SetName(pCopy, kCopyName);
                item.pModel = pCopy;
                SetHold(item, pForm, 1.f);
            }
        }
    }
    const glm::vec3 target(acMessage.Interact.Target[0], acMessage.Interact.Target[1], acMessage.Interact.Target[2]);
    if (!BetterInteractClip::Start(pRoot, false, kind, acMessage.Interact.HasTarget ? &target : nullptr, item, View{}, BodyForwardOf(pActor)))
        return;
    const auto it = std::find_if(m_playing.begin(), m_playing.end(), [pRoot](const Playing& acPlaying) { return acPlaying.pRoot == pRoot; });
    if (it != m_playing.end())
        *it = Playing{pActor->formID, pRoot, false, false, false};
    else
        m_playing.push_back(Playing{pActor->formID, pRoot, false, false, false});
    spdlog::info("Better Interact: {:X}'s hand ({}, {})", pActor->formID, static_cast<int>(kind), item.pModel ? "its item in it" : "nothing in it");
}

#endif

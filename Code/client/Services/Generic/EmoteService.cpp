#include <Services/EmoteService.h>

#if TP_FALLOUT4

#include <Services/GrappleService.h>
#include <Services/OverlayService.h>
#include <Services/ParkourService.h>
#include <Services/TransportService.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestMovementAction.h>
#include <Messages/NotifyMovementAction.h>
#include <World.h>
#include <Utils.h>

#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <BSAnimationGraphManager.h>
#include <Forms/TESIdleForm.h>
#include <Forms/TESNPC.h>
#include <Forms/BGSAction.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>
#include <Games/IFormFactory.h>
#include <Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <NodeTools.h>

#include <fstream>
#include <sstream>
#include <PerfProbe.h>

// Animation.cpp: lets an action through on a remote character, and lets one idle through once when the game performs
// it later
extern thread_local bool g_forceAnimation;
void AllowRemoteIdle(uint32_t aActorId, uint32_t aIdleId) noexcept;

namespace
{
// A clip under Meshes\Actors\Character\Animations and how long it plays (read from the clip, Fallout4 - Animations.ba2)
struct Clip
{
    const char* File{nullptr};
    float Duration{0.f};
};

struct Emote
{
    const char* Name;  // its chat command and its name in config/Emotes.ini
    const char* Label; // on the wheel
    Clip Start;        // a gesture; a pose's way down
    Clip FemaleStart;  // a woman's, where the game has its own (none: the same)
    Clip Hold;         // a pose's loop (none: a gesture)
    Clip Stop;         // a pose's way up
};

// The game's own clips (docs/EMOTES.md has where each comes from). The order is the wire's (EmoteCue::Emote): new ones
// go at the end.
constexpr Emote kEmotes[] = {
    {"salute", "Salute", {"MT\\Neutral\\PoseA_Salute1.hkx", 5.43f}},
    {"bossalute", "Brotherhood salute", {"MT\\Neutral\\PoseA_BOSSalute1.hkx", 5.00f}},
    {"point", "Point", {"MT\\Neutral\\PoseA_PointForward1.hkx", 3.70f}},
    {"pointleft", "Point left", {"MT\\Neutral\\PoseA_PointLeft1.hkx", 3.60f}},
    {"pointright", "Point right", {"MT\\Neutral\\PoseA_PointRight1.hkx", 4.37f}},
    {"pointback", "Point back", {"MT\\Neutral\\PoseA_PointBack1.hkx", 3.50f}},
    {"yes", "Yes", {"MT\\Player\\PoseA_Agree1.hkx", 1.87f}, {"MT\\Player\\Female\\PoseA_Agree1.hkx", 2.87f}},
    {"no", "No", {"MT\\Player\\PoseA_Disagree1.hkx", 2.83f}, {"MT\\Player\\Female\\PoseA_Disagree1.hkx", 2.30f}},
    {"clap", "Clap", {"Furniture\\Clap\\clap_standing.hkx", 3.00f}},
    {"cheer", "Cheer", {"Quest\\Cheering\\Stand\\Cheering1.hkx", 4.33f}, {"Quest\\Cheering\\Stand\\Female\\Cheering1.hkx", 4.33f}},
    {"boo", "Boo", {"Quest\\Cheering\\Stand\\Booing1.hkx", 5.97f}, {"Quest\\Cheering\\Stand\\Female\\Booing1.hkx", 5.97f}},
    {"think", "Think", {"MT\\Neutral\\PoseA_IdleThink1.hkx", 6.67f}},
    {"lookaround", "Look around", {"DynamicAnims\\IdleLookUpAround.hkx", 8.00f}},
    {"offer", "Offer", {"MT\\Neutral\\PoseA_Give1.hkx", 4.50f}},
    {"victory", "Victory", {"Quest\\Fighter\\Winner.hkx", 11.67f}},
    {"sit", "Sit", {"Furniture\\FloorSitRadioListen\\Neutral\\EnterFromStand.hkx", 4.17f}, {}, {"Furniture\\FloorSitRadioListen\\Neutral\\PoseA_Idle1.hkx", 5.90f},
     {"Furniture\\FloorSitRadioListen\\Neutral\\ExitToStand.hkx", 4.57f}},
    {"kneel", "Kneel", {"Furniture\\KneelSit\\EnterFromStand.hkx", 5.00f}, {}, {"Furniture\\KneelSit\\PoseA_Idle1.hkx", 2.67f}, {"Furniture\\KneelSit\\ExitToStand.hkx", 2.33f}},
    {"pray", "Pray", {"Furniture\\KneelPraying\\EnterFromStand.hkx", 5.00f}, {}, {"Furniture\\KneelPraying\\PoseA_Idle1.hkx", 2.67f}, {"Furniture\\KneelPraying\\ExitToStand.hkx", 2.00f}},
    {"surrender", "Surrender", {"Furniture\\HeldHostageKneelHandsUp\\EnterFromStand.hkx", 5.33f}, {}, {"Furniture\\HeldHostageKneelHandsUp\\PoseA_Idle1.hkx", 8.00f},
     {"Furniture\\HeldHostageKneelHandsUp\\ExitToStand.hkx", 1.33f}},
    // ours (Tools/ConvertEmoteAnimations: the_bird.py, the right hand up in front, its back to the viewer, the middle
    // finger up; hands_up.py, standing with the kneeling hands-up loop's arms)
    {"bird", "The Bird", {"ReclamationDay\\Emotes\\TheBird.hkx", 3.20f}},
    {"handsup", "Hands Up!", {"ReclamationDay\\Emotes\\HandsUp.hkx", 3.50f}},
};
constexpr size_t kEmoteCount = std::size(kEmotes);
static_assert(kEmoteCount < 256);

// The wheel's eight when config/Emotes.ini doesn't name them (Hands Up! and The Bird in place of yes and no, since
// 2026-10-27)
constexpr const char* kDefaultWheel[] = {"point", "handsup", "bird", "clap", "cheer", "salute", "sit", "think"};

constexpr char kThirdPersonGraph[] = "Actors\\Character\\Behaviors\\RaiderRootBehavior.hkx";
constexpr char kAnimations[] = "Actors\\Character\\Animations\\";
// The MT graph's dynamic idle events a pose's clips play on (MTBehavior.hkx, MTRoot_SM's wildcard transitions), taking turns:
// each clip goes to the dynamic state the last one isn't in, so every new clip (the next emote, a pose's loop and its
// way up) is a transition from one state to another, blended over 0.2 s. An event for the state already playing does
// nothing and the old clip goes on (seen in game 2026-10-26: with dyn_ActivationCullWeapons only, switching emotes
// played the same one again; with dyn_ActivationLoop only, whose transition may go to itself (flags 0xA00), some
// switches took and some didn't: a sit's way down went on in place of its loop). Every dynamic state loops its clip until
// IdleStop (none of these clips ends itself).
constexpr const char* kEvents[] = {"dyn_ActivationCullWeapons", "dyn_ActivationLoop"};
// Those clips loop (hkbClipGenerator mode 1), so a pose's way down and way up hand over this long before their end (the
// blends: 0.2 s into the next clip, 0.3 s out on IdleStop), never back to their first frame
constexpr float kHandOverEarly = 0.3f;

constexpr uint32_t kActionIdleStop = 0x18BA8;
constexpr uint64_t kFemale = 1 << 0;
constexpr uint32_t kMoveModeSwimming = 1u << 10;
constexpr uint32_t kStanceSneaking = 1;
// ActorState::GetWeaponState (CommonLibF4 WEAPON_STATE): 0 away, 1 to be drawn, 2 coming out, 3 out, 4 and 5 going away
constexpr uint32_t kWeaponAway = 0;
constexpr uint32_t kWeaponWantToDraw = 1;
constexpr uint32_t kWeaponDrawn = 3;
// How long an emote waits for the weapon put away for it before it starts anyway (seconds)
constexpr float kHolsterWait = 2.5f;

// PlayerCamera::cameraStates (CommonLibF4 CameraStates) at 0xE0
constexpr uint32_t kFirstPersonState = 0;
constexpr uint32_t kThirdPersonState = 8;

// Menus an emote doesn't start under (the menu mode covers the rest)
constexpr const char* kBlockingMenus[] = {"PipboyMenu", "DialogueMenu", "VATSMenu", "WorkshopMenu", "LooksMenu", "ContainerMenu", "BarterMenu", "ExamineMenu",
                                          "TerminalMenu", "LockpickingMenu", "BookMenu", "PauseMenu", "Console", "FavoritesMenu", "LoadingMenu", "MainMenu",
                                          "SleepWaitMenu", "MessageBoxMenu", "CookingMenu", "PowerArmorModMenu", "LevelUpMenu", "SPECIALMenu"};

std::atomic<bool> s_playerEmoting{false};

// Full Body First Person (a bundled mod) shows the third person body in first person, run by the player's third person
// graph: the first of the two, first person's being the second and the active one then (as ParkourService). An idle
// plays on the active graph only.
bool IsBodyShownInFirstPerson() noexcept
{
    static const bool s_loaded = GetModuleHandleW(L"FullBodyFirstPerson.dll") != nullptr;
    return s_loaded;
}

// BSAnimationGraphVariableCache::SetAnimationGraph (CommonLibF4 id 497717): the manager's graph variable cache (at
// 0x88) points at one graph (its +0x38), the active one, and the game reads and writes graph variables through it, the
// dynamic idle's clip too
void BindVariableCache(BSAnimationGraphManager* apManager, uint32_t aGraph) noexcept
{
    using TSetAnimationGraph = void(void*, BShkbAnimationGraph**);
    POINTER_FALLOUT4(TSetAnimationGraph, s_setAnimationGraph, 497718);
    auto& graphs = apManager->animationGraphs;
    BShkbAnimationGraph** pSlots = graphs.capacity >= 0 ? graphs.data : reinterpret_cast<BShkbAnimationGraph**>(&graphs.data);
    s_setAnimationGraph.Get()(reinterpret_cast<uint8_t*>(apManager) + 0x88, &pSlots[aGraph]);
}

// Runs aFunction on one of the player's graphs (0 the third person one, the body's; 1 first person's): as the active one
// for the call while the other one is (the active graph changes some frames after the view does), else as it is.
// Borrowed under the manager's lock, the active index and the variable cache both moved to it and back.
// With the index alone (seen in game 2026-10-26) the event reached the body's graph but the clip went to first person's
// through the cache. Full Body First Person passes an event the player's graph takes on to the other graph (its mirror).
template <class T> void OnGraph(Actor* apActor, uint32_t aGraph, T&& aFunction) noexcept
{
    BSAnimationGraphManager* pManager = nullptr;
    apActor->animationGraphHolder.GetBSAnimationGraph(&pManager);
    if (pManager && pManager->animationGraphs.size >= 2 && pManager->animationGraphs.size < 9 && aGraph < 2 && pManager->animationGraphIndex == 1 - aGraph)
    {
        const uint32_t was = pManager->animationGraphIndex;
        pManager->lock.Lock();
        pManager->animationGraphIndex = aGraph;
        BindVariableCache(pManager, aGraph);
        aFunction();
        pManager->animationGraphIndex = was;
        BindVariableCache(pManager, was);
        pManager->lock.Unlock();
    }
    else
        aFunction();
    if (pManager)
        pManager->Release();
}

template <class T> void OnBodyGraph(Actor* apActor, T&& aFunction) noexcept
{
    OnGraph(apActor, 0, std::forward<T>(aFunction));
}

// The human graph's foot IK (bHumanoidFootIKDisable): the looping state leaves it on, which pulls the feet to the ground
// under the character through a pose (ParkourService turns it off for its moves the same way)
void SetFootIk(Actor* apActor, bool aOn) noexcept
{
    static BSFixedString s_footIkDisable("bHumanoidFootIKDisable");
    apActor->animationGraphHolder.SetVariableBool(&s_footIkDisable, !aOn);
}

// After a first person emote the player couldn't walk while first person's graph was the active one, and the saves
// kept it (seen in game 2026-10-27, "it just locked my movement when doing the bird"; walking only in third person,
// after loading too). The first person graph's flags are logged a moment after a first person emote ends and after a
// load, and bAnimationDriven (the animation moving the player, not the controls) turned off. (bAllowRotation, which
// emotes had set on every frame for looking around without effect, isn't touched any more.)
std::string GraphFlags(Actor* apActor, uint32_t aGraph) noexcept
{
    static BSFixedString s_animationDriven("bAnimationDriven"), s_graphDriven("bGraphDriven"), s_allowRotation("bAllowRotation"),
        s_manualGraphChange("bManualGraphChange");
    BSFixedString* names[] = {&s_animationDriven, &s_graphDriven, &s_allowRotation, &s_manualGraphChange};
    std::string out;
    OnGraph(apActor, aGraph, [&] {
        for (BSFixedString* pName : names)
        {
            bool value = false;
            const bool found = apActor->animationGraphHolder.GetVariableBool(pName, &value);
            out += fmt::format(" {}={}", pName->AsAscii(), found ? (value ? "on" : "off") : "none");
        }
    });
    return out;
}

// The first person graph's emote ended as the parkour ends its first person moves: the plain IdleStop event (the
// game's IdleStop action is what third person emotes end with)
void StopFirstPersonIdle(Actor* apActor) noexcept
{
    static BSFixedString s_idleStop("IdleStop");
    OnGraph(apActor, 1, [apActor] { apActor->animationGraphHolder.SendAnimationEvent(&s_idleStop); });
}

void FreeFirstPersonGraph(Actor* apActor, const char* acpWhen) noexcept
{
    const std::string before = GraphFlags(apActor, 1);
    static BSFixedString s_animationDriven("bAnimationDriven");
    OnGraph(apActor, 1, [apActor] { apActor->animationGraphHolder.SetVariableBool(&s_animationDriven, false); });
    spdlog::info("Emotes: the first person graph {}:{}; bAnimationDriven turned off (now{}); the body's:{}", acpWhen, before, GraphFlags(apActor, 1),
                 GraphFlags(apActor, 0));
}

// A pose in first person (sit, kneel, pray, surrender; the camera follows the clip in first person, so you actually
// sit with it): the body plays it, and Full Body First Person's view follows the
// body's head down with it. The game updates the active graph alone (BSAnimationGraphManager::Update, id 1534254) and
// Full Body First Person updates the body's itself while first person's is the active one, which doesn't play a
// dynamic idle's clip (seen in game 2026-10-26 21:39), so for the pose the body's graph is held as the active one, index
// and variable cache (from the first clip to after the IdleStop): the game updates it as in third person, and Full Body
// First Person leaves it to the game (its update runs the body's only while graph 1 is the active one). Every frame the
// player's animation update (its holder's, id 740787) puts the view's graph back through
// PlayerCharacter::SwitchBehaviorGraph (id 1209774; the player's +0xDFE bit 4 says which), so that's skipped while the
// body's is held. Put back while the view is still first person and the game hasn't changed it (s_heldManager: compared only). Seen working in game 2026-10-26 22:46 (every emote on the body).
BSAnimationGraphManager* s_heldManager = nullptr;
std::atomic<bool> s_holdingBody{false};

TP_THIS_FUNCTION(TSwitchBehaviorGraph, void, PlayerCharacter);
TSwitchBehaviorGraph* RealSwitchBehaviorGraph = nullptr;

void TP_MAKE_THISCALL(HookSwitchBehaviorGraph, PlayerCharacter)
{
    if (s_holdingBody.load(std::memory_order_relaxed))
        return;
    TiltedPhoques::ThisCall(RealSwitchBehaviorGraph, apThis);
}

TiltedPhoques::Initializer s_emoteHooks(
    []()
    {
        POINTER_FALLOUT4(TSwitchBehaviorGraph, s_switchBehaviorGraph, 1209775);
        RealSwitchBehaviorGraph = s_switchBehaviorGraph.Get();
        TP_HOOK(&RealSwitchBehaviorGraph, HookSwitchBehaviorGraph);
    });

bool HoldBodyGraph(Actor* apActor) noexcept
{
    BSAnimationGraphManager* pManager = nullptr;
    apActor->animationGraphHolder.GetBSAnimationGraph(&pManager);
    if (!pManager)
        return false;
    const bool hold = pManager->animationGraphs.size >= 2 && pManager->animationGraphs.size < 9 && pManager->animationGraphIndex == 1;
    if (hold)
    {
        s_holdingBody = true;
        pManager->lock.Lock();
        pManager->animationGraphIndex = 0;
        BindVariableCache(pManager, 0);
        pManager->lock.Unlock();
        s_heldManager = pManager;
    }
    pManager->Release();
    return hold;
}

void ReleaseBodyGraph(Actor* apActor, bool aFirstPerson) noexcept
{
    s_holdingBody = false;
    if (!s_heldManager)
        return;
    BSAnimationGraphManager* pManager = nullptr;
    if (apActor)
        apActor->animationGraphHolder.GetBSAnimationGraph(&pManager);
    if (pManager && pManager == s_heldManager && aFirstPerson && pManager->animationGraphIndex == 0 && pManager->animationGraphs.size >= 2 && pManager->animationGraphs.size < 9)
    {
        pManager->lock.Lock();
        pManager->animationGraphIndex = 1;
        BindVariableCache(pManager, 1);
        pManager->lock.Unlock();
    }
    s_heldManager = nullptr;
    if (pManager)
        pManager->Release();
}

// Full Body First Person's settings: a std::shared_ptr at the DLL's +0x1483C0 (the object's pointer), in the build
// Reclamation Day asks for (TimeDateStamp 0x6ABE5588, image 0x154000 bytes); another build is left alone. Its
// iCameraBobMode is the int at +0x6C (its settings are read into one block and copied: iArmHideFrom at +0xC0 and
// fHiddenArmScale at +0xC8 match their defaults; iCameraBobMode comes 0x54 before iArmHideFrom), 0 to 3 the view on the
// head (1 light), 4 locked at standing height (bundled default; its body shifted up to 40 under the view). Set to 1 for a
// first person pose when it reads 4, put back after.
uint8_t* FullBodySettings() noexcept
{
    static uint8_t* s_pModule = []() -> uint8_t*
    {
        auto* pBase = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"FullBodyFirstPerson.dll"));
        if (!pBase)
            return nullptr;
        const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
        if (pNt->FileHeader.TimeDateStamp != 0x6ABE5588 || pNt->OptionalHeader.SizeOfImage != 0x154000)
        {
            spdlog::warn("Emotes: not the Full Body First Person build this knows (stamp {:X}): the view stays at standing height in a first person pose",
                         pNt->FileHeader.TimeDateStamp);
            return nullptr;
        }
        return pBase;
    }();
    return s_pModule ? *reinterpret_cast<uint8_t**>(s_pModule + 0x1483C0) : nullptr;
}

constexpr int kViewOnHead = 1;
constexpr int kViewLocked = 4;

// The first person arms are folded away while the body plays a pose: the first person skeleton's upper arms sized to
// 0.001 (as Full Body First Person hides the body's, fHiddenArmScale), its hands drawn down to the shoulders. That
// skeleton stands still meanwhile (its graph isn't the active one), frozen in its last pose and still drawn: neither
// its parts' culled bits nor its root culled through NiAVObject::SetAppCulled hid it (seen in game 2026-10-26 23:19,
// 23:37); the sizes did (23:43).
constexpr float kFoldedArmScale = 0.001f;
constexpr const char* kFirstPersonUpperArms[] = {"RArm_UpperArm", "LArm_UpperArm"};

struct FirstPersonPose
{
    uint8_t* pSettings{nullptr};
    int ViewMode{-1};
    uint8_t* pFirstPersonRoot{nullptr};
    std::array<uint8_t*, 2> FoldedArms{};
    std::array<float, 2> ArmScales{};
    // the body's head over its root when the pose started, standing
    float StandingHead{-1.f};
};
FirstPersonPose s_firstPersonPose;

// The view goes down with the body's head through a first person pose: Full Body First Person leaves the camera to
// the game while the body's graph is held (the view stayed at standing height with its view mode on the head, seen in
// game 2026-10-27 00:31 and 00:47), so first person's own camera position, FirstPersonState::GetTranslation (vtable
// +0x68, id 45357: the first person camera node's world place), is taken down by how far the head is under where it
// stood
std::atomic<float> s_viewDrop{0.f};
std::atomic<bool> s_loggedViewDrop{false};

TP_THIS_FUNCTION(TFirstPersonTranslation, void, void, float*);
TFirstPersonTranslation* RealFirstPersonTranslation = nullptr;

void TP_MAKE_THISCALL(HookFirstPersonTranslation, void, float* apTranslation)
{
    TiltedPhoques::ThisCall(RealFirstPersonTranslation, apThis, apTranslation);
    const float drop = s_viewDrop.load(std::memory_order_relaxed);
    if (drop > 0.f && apTranslation)
    {
        apTranslation[2] -= drop;
        if (drop > 20.f && !s_loggedViewDrop.exchange(true))
            spdlog::info("Emotes: the first person view goes down {:.0f} with the body's head", drop);
    }
}

TiltedPhoques::Initializer s_viewHooks(
    []()
    {
        POINTER_FALLOUT4(TFirstPersonTranslation, s_firstPersonTranslation, 45358);
        RealFirstPersonTranslation = s_firstPersonTranslation.Get();
        TP_HOOK(&RealFirstPersonTranslation, HookFirstPersonTranslation);
    });

void SetLocalScale(uint8_t* apNode, float aScale) noexcept
{
    NodeTools::Transform local = NodeTools::Read(apNode, NodeTools::kLocalOffset);
    local.Scale = aScale;
    NodeTools::Write(apNode, NodeTools::kLocalOffset, local);
}

// every frame of a first person pose: the view on the body's head, the first person arms folded
void KeepFirstPersonPose(PlayerCharacter* apPlayer) noexcept
{
    auto& pose = s_firstPersonPose;
    // a new settings object when Full Body First Person reads them again (closing the pause menu): its own value
    if (uint8_t* pSettings = FullBodySettings(); pSettings && pSettings != pose.pSettings)
    {
        pose.pSettings = pSettings;
        pose.ViewMode = *reinterpret_cast<int*>(pSettings + 0x6C);
        spdlog::info("Emotes: Full Body First Person's view mode {}{}", pose.ViewMode, pose.ViewMode == kViewLocked ? ", on the head for the pose" : ", left as it is");
    }
    if (pose.pSettings && pose.ViewMode == kViewLocked)
        *reinterpret_cast<int*>(pose.pSettings + 0x6C) = kViewOnHead;

    // the view down with the head
    static BSFixedString s_head("HEAD");
    if (uint8_t* pBody = NodeTools::GetThirdPersonRoot(apPlayer))
    {
        if (uint8_t* pHead = NodeTools::GetObjectByName(pBody, s_head))
        {
            const float height = NodeTools::Read(pHead, NodeTools::kWorldOffset).Translation.z - NodeTools::Read(pBody, NodeTools::kWorldOffset).Translation.z;
            if (pose.StandingHead < 0.f)
                pose.StandingHead = height;
            s_viewDrop = std::max(0.f, pose.StandingHead - height);
        }
    }

    uint8_t* pRoot = NodeTools::GetFirstPersonRoot(apPlayer);
    if (!pRoot)
        return;
    if (pRoot != pose.pFirstPersonRoot)
    {
        pose.pFirstPersonRoot = pRoot;
        pose.FoldedArms = {};
    }
    for (size_t i = 0; i < std::size(kFirstPersonUpperArms); ++i)
    {
        if (!pose.FoldedArms[i])
        {
            if (uint8_t* pArm = NodeTools::GetObjectByName(pRoot, BSFixedString(kFirstPersonUpperArms[i])))
            {
                pose.FoldedArms[i] = pArm;
                pose.ArmScales[i] = NodeTools::Read(pArm, NodeTools::kLocalOffset).Scale;
            }
        }
        if (uint8_t* pArm = pose.FoldedArms[i])
            SetLocalScale(pArm, kFoldedArmScale);
    }
}

void EndFirstPersonPose() noexcept
{
    auto& pose = s_firstPersonPose;
    if (pose.pSettings && pose.ViewMode == kViewLocked && FullBodySettings() == pose.pSettings)
        *reinterpret_cast<int*>(pose.pSettings + 0x6C) = pose.ViewMode;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (uint8_t* pRoot = pPlayer ? NodeTools::GetFirstPersonRoot(pPlayer) : nullptr; pRoot && pRoot == pose.pFirstPersonRoot)
    {
        for (size_t i = 0; i < std::size(kFirstPersonUpperArms); ++i)
        {
            if (uint8_t* pArm = pose.FoldedArms[i]; pArm && NodeTools::GetObjectByName(pRoot, BSFixedString(kFirstPersonUpperArms[i])) == pArm)
                SetLocalScale(pArm, pose.ArmScales[i] > 0.01f ? pose.ArmScales[i] : 1.f);
        }
    }
    pose = FirstPersonPose{};
    s_viewDrop = 0.f;
    s_loggedViewDrop = false;
}

// First person (with Full Body First Person): the emote's own first person clip on the first person graph, the active
// one, as ParkourService plays its first person moves; the body doesn't do it here (the others' copies of the player
// do). The clips are the body's with the hands brought into the view (Tools/ConvertEmoteAnimations, docs/EMOTES.md),
// each at its third person clip's path under ReclamationDay\Emotes. The body's own clips in first person showed only
// what came up into the view (a cheer), not a clap or a salute at the chest or the face under it, and so did the body's
// arm turns copied onto the first person arms (seen in game 2026-10-26 23:43 and 23:50): each view gets clips of its
// own.
constexpr char kFirstPersonAnimations[] = "Actors\\Character\\_1stPerson\\Animations\\ReclamationDay\\Emotes\\";
// the vanilla first person landing idle: its graph is the one the first person clips play on (as ParkourService's)
constexpr uint32_t kIdle1stPJumpImpactLand = 0x0005B339;
// A gesture plays in the graphs' dynamic state that leaves the player walking (no movement lock while emoting, except
// for the poses: sitting, kneeling): on
// dyn_ActivationAllowMovement, whose state is a layer generator, the clip with the walking state machine
// (DefaultOrSneak_SM) over it (third person MTBehavior.hkx; first person MTBehavior.hkx "DynamicActivation_AllowMovement",
// both left on idleStop). A pose keeps kEvents' states, which hold the player still.
constexpr char kGestureEvent[] = "dyn_ActivationAllowMovement";

// The body's weapon in first person: the emote's weapon culling (AIProcess, id 397172) hides the active view's weapon
// only, and Full Body First Person shows the body with the gun in its hand (seen in game 2026-10-26 23:19). The models
// on its WEAPON bone are culled through NiAVObject::SetAppCulled (vtable +0x168), and only those still on it shown again
// after. (The culled bit alone doesn't take a model off the screen: the first person model's parts, 23:19.)
struct BodyWeapon
{
    uint8_t* pBone{nullptr};
    std::vector<uint8_t*> Hidden;
};
BodyWeapon s_bodyWeapon;

bool IsCulled(const uint8_t* apNode) noexcept
{
    return (*reinterpret_cast<const uint64_t*>(apNode + NodeTools::kFlagsOffset) & 1) != 0;
}

void SetAppCulled(uint8_t* apNode, bool aCulled) noexcept
{
    using TSetAppCulled = void(uint8_t*, bool);
    reinterpret_cast<TSetAppCulled*>((*reinterpret_cast<void***>(apNode))[0x168 / sizeof(void*)])(apNode, aCulled);
}

void HideBodyWeapon(PlayerCharacter* apPlayer) noexcept
{
    static BSFixedString s_weapon("WEAPON");
    uint8_t* pBody = NodeTools::GetThirdPersonRoot(apPlayer);
    uint8_t* pBone = pBody ? NodeTools::GetObjectByName(pBody, s_weapon) : nullptr;
    if (!pBone)
        return;
    if (pBone != s_bodyWeapon.pBone)
        s_bodyWeapon = BodyWeapon{pBone, {}};
    NodeTools::ForEachChild(pBone, [](uint8_t* apChild) {
        if (IsCulled(apChild))
            return;
        SetAppCulled(apChild, true);
        s_bodyWeapon.Hidden.push_back(apChild);
    });
}

void ShowBodyWeapon() noexcept
{
    static BSFixedString s_weapon("WEAPON");
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    uint8_t* pBody = pPlayer ? NodeTools::GetThirdPersonRoot(pPlayer) : nullptr;
    if (uint8_t* pBone = pBody ? NodeTools::GetObjectByName(pBody, s_weapon) : nullptr; pBone && pBone == s_bodyWeapon.pBone && !s_bodyWeapon.Hidden.empty())
    {
        NodeTools::ForEachChild(pBone, [](uint8_t* apChild) {
            const auto& hidden = s_bodyWeapon.Hidden;
            if (std::find(hidden.begin(), hidden.end(), apChild) != hidden.end())
                SetAppCulled(apChild, false);
        });
    }
    s_bodyWeapon = BodyWeapon{};
}

// How far each stage of the local player's emote moved the hands, turned the head and took the head up or down on the
// skeleton it plays on (in its root's space; a first person gesture: the hands against the still camera bone), from a
// quarter second into the stage (past the blend into it): the log's check that the clip played, and that a pose holds
// (its head stays low)
constexpr float kMotionFrom = 0.25f;
struct Motion
{
    bool Started{false};
    glm::vec3 RightLow{0.f}, RightHigh{0.f};
    glm::vec3 LeftLow{0.f}, LeftHigh{0.f};
    NodeTools::Transform Head{};
    float HeadMax{0.f};
    float HeadFirst{0.f}, HeadLast{0.f}, HeadLow{0.f}, HeadHigh{0.f};
    float From{0.f};
    int Samples{0};
};
Motion s_motion;

bool ReadMotion(PlayerCharacter* apPlayer, bool aFirstPerson, glm::vec3& aRight, glm::vec3& aLeft, NodeTools::Transform& aHead) noexcept
{
    static BSFixedString s_right("RArm_Hand");
    static BSFixedString s_left("LArm_Hand");
    static BSFixedString s_head("HEAD");
    static BSFixedString s_camera("Camera");
    uint8_t* pRoot = aFirstPerson ? NodeTools::GetFirstPersonRoot(apPlayer) : NodeTools::GetThirdPersonRoot(apPlayer);
    uint8_t* pRight = pRoot ? NodeTools::GetObjectByName(pRoot, s_right) : nullptr;
    uint8_t* pLeft = pRoot ? NodeTools::GetObjectByName(pRoot, s_left) : nullptr;
    uint8_t* pHead = pRoot ? NodeTools::GetObjectByName(pRoot, aFirstPerson ? s_camera : s_head) : nullptr;
    if (!pRight || !pLeft || !pHead)
        return false;
    const NodeTools::Transform root = NodeTools::Read(pRoot, NodeTools::kWorldOffset);
    aRight = NodeTools::Relative(root, NodeTools::Read(pRight, NodeTools::kWorldOffset)).Translation;
    aLeft = NodeTools::Relative(root, NodeTools::Read(pLeft, NodeTools::kWorldOffset)).Translation;
    aHead = NodeTools::Relative(root, NodeTools::Read(pHead, NodeTools::kWorldOffset));
    return true;
}

// degrees between two rotations
float TurnBetween(const NodeTools::Transform& acA, const NodeTools::Transform& acB) noexcept
{
    const float trace = glm::dot(acA.Rows[0], acB.Rows[0]) + glm::dot(acA.Rows[1], acB.Rows[1]) + glm::dot(acA.Rows[2], acB.Rows[2]);
    return glm::degrees(std::acos(std::clamp((trace - 1.f) * 0.5f, -1.f, 1.f)));
}

const Emote& EmoteAt(uint8_t aEmote) noexcept
{
    return kEmotes[aEmote < kEmoteCount ? aEmote : 0];
}

int FindEmote(const std::string& acName) noexcept
{
    for (size_t i = 0; i < kEmoteCount; ++i)
    {
        if (_stricmp(kEmotes[i].Name, acName.c_str()) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

const Clip& ClipOf(const Emote& acEmote, bool aFemale, int aStage) noexcept
{
    if (aStage == 1)
        return acEmote.Hold;
    if (aStage == 2)
        return acEmote.Stop;
    return aFemale && acEmote.FemaleStart.File ? acEmote.FemaleStart : acEmote.Start;
}

bool IsPose(const Emote& acEmote) noexcept
{
    return acEmote.Hold.File != nullptr;
}

bool IsFemale(const Actor* apActor) noexcept
{
    const auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    return pNpc && (pNpc->actorData.flags & kFemale) != 0;
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

// Third person, the camera turns around the character through a pose instead of turning it (so you can look around
// and see yourself sit), as in furniture (a gesture lets
// the player walk, the camera turning the character as ever): the camera state's own
// ThirdPersonState::SetFreeRotationMode(enable, modifyRotation) (vtable +0x98, id 670300; it doesn't turn it on while
// the character moves), which sets freeRotationEnabled (+0x128). Nothing while the view isn't third person.
uint8_t* ThirdPersonCamera() noexcept
{
    const uint8_t* pCamera = PlayerCamera();
    uint8_t* pState = pCamera ? *reinterpret_cast<uint8_t* const*>(pCamera + 0x28) : nullptr;
    return pState && *reinterpret_cast<const uint32_t*>(pState + 0x20) == kThirdPersonState ? pState : nullptr;
}

bool IsCameraFree() noexcept
{
    const uint8_t* pState = ThirdPersonCamera();
    return pState && pState[0x128] != 0;
}

void SetCameraFree(bool aFree) noexcept
{
    if (uint8_t* pState = ThirdPersonCamera())
    {
        using TSetFreeRotationMode = void(uint8_t*, bool, bool);
        reinterpret_cast<TSetFreeRotationMode*>((*reinterpret_cast<void***>(pState))[0x98 / sizeof(void*)])(pState, aFree, false);
    }
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

bool IsMenuOpen(const char* apName) noexcept
{
    UI* pUi = UI::Get();
    return pUi && pUi->GetMenuOpen(BSFixedString(apName));
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

// The game's window is in front (the keys aren't going to another window)
bool GameHasKeyboard() noexcept
{
    HWND window = GetForegroundWindow();
    DWORD processId = 0;
    if (window)
        GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

// The keyboard key of one of the game's controls ("Forward"), as the player has it bound: ControlMap (CommonLibF4 id
// 325206) controlMaps[context] at +8, main gameplay's first, its keyboard UserEventMapping {eventID, inputKey...} (0x18
// bytes) the first array; Windows virtual keys (as NemesisGame::ActivateKey)
int GameKey(const char* acpControl) noexcept
{
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
        if (pName && _stricmp(pName, acpControl) == 0)
            return mapping.InputKey > 0 && mapping.InputKey < 0xFF ? mapping.InputKey : 0;
    }
    return 0;
}

// Out of the emote's idle into the graph's normal state, through the game's action (the animation sync carries it to
// the others' copy of a local player), as the parkour ends its moves
void StopIdle(Actor* apActor) noexcept
{
    if (auto* pAction = Cast<BGSAction>(TESForm::GetById(kActionIdleStop)))
    {
        TESActionData actionData(2, apActor, pAction, nullptr);
        if (ActorMediator::Get()->PerformAction(&actionData))
            return;
    }
    BSFixedString idleStop("IdleStop");
    apActor->animationGraphHolder.SendAnimationEvent(&idleStop);
    idleStop.Release();
}

std::string Lower(std::string aText) noexcept
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return aText;
}

std::string JsonString(const char* acpText) noexcept
{
    std::string out = "\"";
    for (const char* p = acpText; *p; ++p)
    {
        if (*p == '"' || *p == '\\')
            out += '\\';
        out += *p;
    }
    return out + "\"";
}
} // namespace

EmoteService::EmoteService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&EmoteService::OnUpdate>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&EmoteService::OnNotifyMovementAction>(this);
    LoadSettings();
}

bool EmoteService::IsPlayerEmoting() noexcept
{
    return s_playerEmoting.load(std::memory_order_relaxed);
}

// config/Emotes.ini: [Keys] Wheel = a letter, a digit, F1-F12 or a virtual key code ("0x47"), G by default; [Wheel]
// Emotes = the wheel's eight, by name, from the top going round to the right
void EmoteService::LoadSettings() noexcept
{
    const auto path = TiltedPhoques::GetPath() / "config" / "Emotes.ini";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path);
        file << "; Reclamation Day's emotes. Wheel: the key held for the emote wheel: a letter, a digit, F1-F12 or a virtual\n"
                "; key code such as 0x47 (G is free in Fallout 4's own controls). Emotes: the wheel's eight, from the top going\n"
                "; round to the right. All of them: salute, bossalute, point, pointleft, pointright, pointback, yes, no, clap,\n"
                "; cheer, boo, think, lookaround, offer, victory, sit, kneel, pray, surrender, bird, handsup (each is a chat\n"
                "; command too: /sit).\n"
                "[Keys]\nWheel=G\n[Wheel]\nEmotes=point,handsup,bird,clap,cheer,salute,sit,think\n";
    }

    wchar_t text[256]{};
    GetPrivateProfileStringW(L"Keys", L"Wheel", L"G", text, static_cast<DWORD>(std::size(text)), path.c_str());
    std::wstring value = text;
    value.erase(std::remove_if(value.begin(), value.end(), iswspace), value.end());
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    int key = 0;
    std::string name;
    if (value.size() == 1 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'0' && value[0] <= L'9')))
        key = value[0], name = std::string(1, static_cast<char>(value[0]));
    else if (value.size() >= 2 && value[0] == L'F' && iswdigit(value[1]))
    {
        const int number = std::stoi(value.substr(1));
        if (number >= 1 && number <= 12)
            key = VK_F1 + number - 1, name = "F" + std::to_string(number);
    }
    else if (value.rfind(L"0X", 0) == 0)
    {
        key = static_cast<int>(std::wcstoul(value.c_str() + 2, nullptr, 16));
        name = fmt::format("key 0x{:X}", key);
    }
    if (key <= 0 || key > 0xFE)
        key = 'G', name = "G";
    m_wheelKey = key;
    m_wheelKeyName = name;

    // the eight, any that aren't emotes taken from the defaults
    GetPrivateProfileStringW(L"Wheel", L"Emotes", L"", text, static_cast<DWORD>(std::size(text)), path.c_str());
    std::string list;
    for (const wchar_t* p = text; *p; ++p)
        list += static_cast<char>(*p < 0x80 ? *p : '?');
    std::vector<int> wheel;
    std::stringstream stream(list);
    for (std::string item; std::getline(stream, item, ',');)
    {
        item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char c) { return std::isspace(c); }), item.end());
        const int emote = FindEmote(item);
        if (emote >= 0 && std::find(wheel.begin(), wheel.end(), emote) == wheel.end() && wheel.size() < m_wheel.size())
            wheel.push_back(emote);
    }
    for (const char* pDefault : kDefaultWheel)
    {
        const int emote = FindEmote(pDefault);
        if (wheel.size() < m_wheel.size() && std::find(wheel.begin(), wheel.end(), emote) == wheel.end())
            wheel.push_back(emote);
    }
    for (size_t i = 0; i < m_wheel.size(); ++i)
        m_wheel[i] = static_cast<uint8_t>(wheel[i]);

    spdlog::info("Emotes: {} holds the emote wheel (config/Emotes.ini)", m_wheelKeyName);
}

// --- input

void EmoteService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Emote");
    const float delta = static_cast<float>(acEvent.Delta);

    CheckWheelKey();

    // the emote asked for starts on the next frame (in third person, a frame after the camera's state changed), once a
    // weapon put away for it is away
    if (m_pendingEmote >= 0 && --m_pendingFrames <= 0 && WeaponAway(delta))
        StartLocal();
    if (m_local.ActorId)
        UpdateLocal(delta);

    // After a load (a new first person model) and after a first person emote: the first person graph's bAnimationDriven
    // off (FreeFirstPersonGraph), a moment later, while no emote plays
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
    {
        if (uint8_t* pRoot = NodeTools::GetFirstPersonRoot(pPlayer); pRoot && pRoot != m_lastFirstPersonRoot)
        {
            m_lastFirstPersonRoot = pRoot;
            m_freeIn = 2.f;
            m_freeWhen = "after a load";
        }
        if (m_freeIn >= 0.f && !m_local.ActorId && !IsMenuOpen("LoadingMenu") && (m_freeIn -= delta) < 0.f)
        {
            FreeFirstPersonGraph(pPlayer, m_freeWhen);
            m_freeIn = -1.f;
        }
    }

    // A first person emote ended: the first person graph's IdleStop sent again a moment later, twice (Finish)
    if (m_stopAgainIn >= 0.f && !m_local.ActorId && (m_stopAgainIn -= delta) < 0.f)
    {
        if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
            StopFirstPersonIdle(pPlayer);
        m_stopAgainIn = --m_stopsAgain > 0 ? 0.4f : -1.f;
    }
    else if (m_local.ActorId)
        m_stopAgainIn = -1.f;

    for (auto it = m_remote.begin(); it != m_remote.end();)
    {
        UpdateRemote(*it, delta);
        it = it->ActorId ? it + 1 : m_remote.erase(it);
    }
}

void EmoteService::CheckWheelKey() noexcept
{
    const bool down = GameHasKeyboard() && (GetAsyncKeyState(m_wheelKey) & 0x8000) != 0;
    const bool pressed = down && !m_wheelKeyDown;
    const bool released = !down && m_wheelKeyDown;
    m_wheelKeyDown = down;

    if (m_wheelOpen && released)
        CloseWheel(true);
    else if (pressed && !m_wheelOpen && !World::Get().ctx().at<OverlayService>().GetActive())
        OpenWheel();
}

void EmoteService::OpenWheel() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell || pPlayer->IsDead())
        return;
    for (const char* pMenu : kBlockingMenus)
    {
        if (IsMenuOpen(pMenu))
            return;
    }

    std::string emotes;
    for (const uint8_t emote : m_wheel)
    {
        if (!emotes.empty())
            emotes += ",";
        emotes += fmt::format("{{\"name\":{},\"label\":{}}}", JsonString(kEmotes[emote].Name), JsonString(kEmotes[emote].Label));
    }
    m_wheelOpen = true;
    m_hovered = -1;
    World::Get().ctx().at<OverlayService>().SendEvent("emoteWheel", fmt::format("{{\"key\":{},\"emotes\":[{}]}}", JsonString(m_wheelKeyName.c_str()), emotes));
    ShowOverlayCursor(true);

    // the cursor from the middle: the wheel picks by the way it's moved
    if (HWND window = GetForegroundWindow())
    {
        RECT rect{};
        GetClientRect(window, &rect);
        POINT middle{(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
        ClientToScreen(window, &middle);
        SetCursorPos(middle.x, middle.y);
    }
}

void EmoteService::CloseWheel(bool aPlay) noexcept
{
    if (!m_wheelOpen)
        return;
    m_wheelOpen = false;
    World::Get().ctx().at<OverlayService>().SendEvent("emoteWheel", "null");
    ShowOverlayCursor(false);

    if (aPlay && m_hovered >= 0)
    {
        std::string whyNot;
        if (!Play(static_cast<uint8_t>(m_hovered), whyNot))
            World::Get().ctx().at<OverlayService>().SendSystemMessage(whyNot);
    }
    m_hovered = -1;
}

void EmoteService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    if (acRequest == "hover")
        m_hovered = acPayload.empty() ? -1 : FindEmote(acPayload);
    else if (acRequest == "play")
    {
        m_hovered = FindEmote(acPayload);
        CloseWheel(true);
    }
    else if (acRequest == "close")
        CloseWheel(false);
    else if (acRequest == "command" && !HandleChatCommand(acPayload))
        World::Get().ctx().at<OverlayService>().SendSystemMessage(fmt::format("No emote called {}. /emotes lists them.", acPayload));
}

bool EmoteService::HandleChatCommand(const std::string& acCommand) noexcept
{
    const std::string command = Lower(acCommand.substr(0, acCommand.find(' ')));
    auto& overlay = World::Get().ctx().at<OverlayService>();
    if (command == "emotes" || command == "emote")
    {
        std::string names;
        for (const Emote& emote : kEmotes)
            names += fmt::format("{}/{}", names.empty() ? "" : ", ", emote.Name);
        overlay.SendSystemMessage(fmt::format("Emotes: {}. Hold {} for the emote wheel.", names, m_wheelKeyName));
        return true;
    }

    const int emote = FindEmote(command);
    if (emote < 0)
        return false;
    std::string whyNot;
    if (!Play(static_cast<uint8_t>(emote), whyNot))
        overlay.SendSystemMessage(whyNot);
    return true;
}

// --- the local player

bool EmoteService::WeaponAway(float aDelta) noexcept
{
    if (m_holsterWait < 0.f)
        return true;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->actorState.GetWeaponState() == kWeaponAway)
    {
        m_holsterWait = -1.f;
        return true;
    }
    if ((m_holsterWait -= aDelta) < 0.f)
    {
        spdlog::info("Emotes: the weapon isn't away after {} s (state {}), the emote starts anyway", kHolsterWait, pPlayer->actorState.GetWeaponState());
        return true;
    }
    return false;
}

bool EmoteService::Play(uint8_t aEmote, std::string& aWhyNot) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell || !pPlayer->currentProcess)
        return false;

    const auto& state = pPlayer->actorState;
    if (pPlayer->IsDead() || state.IsBleedingOut())
        aWhyNot = "You can't emote right now.";
    else if (pPlayer->IsInPowerArmor())
        aWhyNot = "Emotes can't be used in power armor.";
    else if (state.GetMoveMode() & kMoveModeSwimming)
        aWhyNot = "Emotes can't be used while swimming.";
    else if (state.GetStance() == kStanceSneaking)
        aWhyNot = "Stand up to emote.";
    // knocked down or using furniture
    else if (((state.flags1 >> 21) & 0xF) != 0 || ((state.flags2 >> 18) & 0x3) != 0)
        aWhyNot = "You can't emote right now.";
    else if (GrappleService::HoldsPlayer() || m_world.ctx().at<ParkourService>().IsPlayerMoving())
        aWhyNot = "You can't emote right now.";
    else
    {
        for (const char* pMenu : kBlockingMenus)
        {
            if (IsMenuOpen(pMenu))
            {
                aWhyNot = "Close the menu to emote.";
                break;
            }
        }
    }
    if (!aWhyNot.empty())
        return false;

    // the emote starts on the next frame; one already playing gives way to it there, straight from where it is (a pose
    // without getting up). In first person the view stays (emotes are seen in first person as
    // in third): a gesture's first person clip on the first person arms, a pose on the body Full Body First Person shows.
    // Without that mod there's no body to see in first person, so the view goes to third person for the emote and comes
    // back after.
    m_pendingEmote = aEmote;
    m_pendingFrames = 1;
    // A weapon out is put away first and drawn again when the emote ends (2026-10-27): with one out, the game holds the
    // hand in the weapon's grip in third person whatever the idle does with the fingers (the bird's fist looked like it
    // held a gun; right when holstered)
    if (!m_local.ActorId && m_holsterWait < 0.f)
    {
        const uint32_t weaponState = state.GetWeaponState();
        if (weaponState >= kWeaponWantToDraw && weaponState <= kWeaponDrawn)
        {
            pPlayer->SetWeaponDrawn(false);
            m_redraw = true;
            m_holsterWait = kHolsterWait;
        }
    }
    if (!m_local.ActorId && IsFirstPerson() && !IsBodyShownInFirstPerson())
    {
        m_restoreFirstPerson = true;
        SetCameraState(kThirdPersonState);
    }
    return true;
}

void EmoteService::StartLocal() noexcept
{
    const auto emote = static_cast<uint8_t>(m_pendingEmote);
    m_pendingEmote = -1;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->currentProcess)
        return;

    // in place of one playing: that one's clip gives way to this one's (kEvents), the weapons staying hidden
    bool replacing = m_local.ActorId == pPlayer->formID;
    Playing playing{};
    playing.ActorId = pPlayer->formID;
    playing.Emote = emote;
    playing.Female = IsFemale(pPlayer);
    // first person (with Full Body First Person): a gesture's first person clip, or a pose on the body
    playing.FirstPerson = replacing ? m_local.FirstPerson : IsFirstPerson() && IsBodyShownInFirstPerson();
    playing.BodyPose = playing.FirstPerson && IsPose(EmoteAt(emote));
    // (those play on different graphs: one in place of the other ends it first)
    if (replacing && playing.BodyPose != m_local.BodyPose)
    {
        // (the weapon stays away for the next one)
        const bool redraw = std::exchange(m_redraw, false);
        Finish(m_local, false);
        m_redraw = redraw;
        replacing = false;
    }
    if (replacing)
        playing.State = m_local.State;
    bool held = false;
    if (playing.BodyPose && !replacing)
    {
        held = HoldBodyGraph(pPlayer);
        // (the body's graph already the active one: the view's on its way to third person)
        playing.BodyPose = held;
    }
    if (!PlayClip(pPlayer, playing, Stage::kStart, false))
    {
        if (held)
            ReleaseBodyGraph(pPlayer, IsFirstPerson());
        spdlog::info("Emotes: {} didn't play (the game refused its idle)", kEmotes[emote].Name);
        World::Get().ctx().at<OverlayService>().SendSystemMessage("You can't emote right now.");
        if (!replacing && m_restoreFirstPerson)
            SetCameraState(kFirstPersonState);
        if (!replacing)
        {
            m_restoreFirstPerson = false;
            if (std::exchange(m_redraw, false) && pPlayer->actorState.GetWeaponState() == kWeaponAway)
                pPlayer->SetWeaponDrawn(true);
        }
        return;
    }

    // the body's foot IK off for a pose, on for a gesture (the player walks through it)
    if (!playing.FirstPerson || playing.BodyPose)
        OnBodyGraph(pPlayer, [pPlayer, emote] { SetFootIk(pPlayer, !IsPose(EmoteAt(emote))); });
    // third person, a pose: the camera free to turn around the character (the view may still be on its way there: then
    // from the frame it is, UpdateLocal); a gesture in place of a pose gives the camera back to turning the character
    if (const bool freeCamera = !playing.FirstPerson && IsPose(EmoteAt(emote)); freeCamera != m_freeCamera)
    {
        SetCameraFree(freeCamera);
        if (freeCamera)
            spdlog::info("Emotes: the camera {} around the character", IsCameraFree() ? "turns" : "doesn't turn yet");
        m_freeCamera = freeCamera;
    }
    if (replacing)
        LogMotion(m_local);
    else
    {
        pPlayer->CullWeapons(true);

        // the movement keys as the player has them, which end a pose (a gesture plays on as the player walks)
        m_movementKeys.clear();
        for (const char* pControl : {"Forward", "Back", "StrafeLeft", "StrafeRight", "Jump"})
        {
            if (const int key = GameKey(pControl))
                m_movementKeys.push_back(key);
        }
    }
    m_movementWasDown = true;
    m_local = playing;
    s_playerEmoting = true;
    s_motion = Motion{};

    Share(emote, RequestMovementAction::kEmoteStart);
    uint32_t graphs = 0, active = 0;
    BSAnimationGraphManager* pManager = nullptr;
    pPlayer->animationGraphHolder.GetBSAnimationGraph(&pManager);
    if (pManager)
    {
        graphs = pManager->animationGraphs.size;
        active = pManager->animationGraphIndex;
        pManager->Release();
    }
    spdlog::info("Emotes: {}{} ({} person, graph {} of {} active{})", kEmotes[emote].Name, replacing ? ", in place of the last one" : "", IsFirstPerson() ? "first" : "third",
                 active, graphs, playing.BodyPose ? ", the body's held as the active one" : playing.FirstPerson ? ", its first person clip" : "");
}

void EmoteService::LogMotion(const Playing& acPlaying) noexcept
{
    constexpr const char* kStages[] = {"", ", way down", ", held", ", way up"};
    const char* pStage = IsPose(EmoteAt(acPlaying.Emote)) ? kStages[1 + static_cast<int>(acPlaying.Now)] : kStages[0];
    if (s_motion.Started)
        spdlog::info("Emotes: {}{}: the right hand over {:.1f}, the left over {:.1f}, the head turned {:.1f} deg, {:.0f} high to {:.0f} ({:.0f}-{:.0f}) ({} frames, "
                     "{:.2f}-{:.2f} s)",
                     EmoteAt(acPlaying.Emote).Name, pStage, glm::length(s_motion.RightHigh - s_motion.RightLow), glm::length(s_motion.LeftHigh - s_motion.LeftLow),
                     s_motion.HeadMax, s_motion.HeadFirst, s_motion.HeadLast, s_motion.HeadLow, s_motion.HeadHigh, s_motion.Samples, s_motion.From, acPlaying.Time);
    s_motion = Motion{};
}

bool EmoteService::MovementPressed() noexcept
{
    bool down = false;
    if (GameHasKeyboard() && !World::Get().ctx().at<OverlayService>().GetActive())
    {
        for (const int key : m_movementKeys)
            down = down || (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    // a key already held when the emote started counts once it's let go and pressed again
    const bool pressed = down && !m_movementWasDown;
    m_movementWasDown = down;
    return pressed;
}

void EmoteService::UpdateLocal(float aDelta) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->formID != m_local.ActorId || pPlayer->IsDead() || pPlayer->actorState.IsBleedingOut() || IsMenuOpen("LoadingMenu"))
    {
        if (pPlayer && pPlayer->formID == m_local.ActorId)
            Share(m_local.Emote, RequestMovementAction::kEmoteStop);
        Finish(m_local, false);
        return;
    }

    m_local.Time += aDelta;
    const Emote& emote = EmoteAt(m_local.Emote);
    const bool moved = MovementPressed();

    // the view switched to the other one: the clip is the other view's, so the emote ends there. Only first or third
    // person counts: another camera state in between doesn't (one ended a first person sit 2.4 s into its loop, seen in
    // game 2026-10-27 00:29)
    if (m_local.FirstPerson ? ThirdPersonCamera() != nullptr : IsFirstPerson() && IsBodyShownInFirstPerson())
    {
        spdlog::info("Emotes: {} ended: the view switched to {} person", emote.Name, m_local.FirstPerson ? "third" : "first");
        Share(m_local.Emote, RequestMovementAction::kEmoteStop);
        Finish(m_local, false);
        return;
    }
    if (m_local.FirstPerson)
    {
        HideBodyWeapon(pPlayer);
        if (m_local.BodyPose)
            KeepFirstPersonPose(pPlayer);
    }
    else if (m_freeCamera && !IsCameraFree() && ThirdPersonCamera())
        SetCameraFree(true);

    // the game's own switch of the active graph (a view change) ends the hold on the body's
    if (s_heldManager)
    {
        BSAnimationGraphManager* pManager = nullptr;
        pPlayer->animationGraphHolder.GetBSAnimationGraph(&pManager);
        if (pManager != s_heldManager || pManager->animationGraphIndex != 0)
        {
            spdlog::info("Emotes: the game changed the active graph during the pose (now graph {}): the body's no longer held", pManager ? pManager->animationGraphIndex : 0u);
            s_heldManager = nullptr;
            s_holdingBody = false;
        }
        if (pManager)
            pManager->Release();
    }

    glm::vec3 right{}, left{};
    NodeTools::Transform head{};
    if (m_local.Time >= kMotionFrom && ReadMotion(pPlayer, m_local.FirstPerson && !m_local.BodyPose, right, left, head))
    {
        if (!s_motion.Started)
        {
            s_motion.Started = true;
            s_motion.RightLow = s_motion.RightHigh = right;
            s_motion.LeftLow = s_motion.LeftHigh = left;
            s_motion.Head = head;
            s_motion.HeadFirst = s_motion.HeadLow = s_motion.HeadHigh = head.Translation.z;
            s_motion.From = m_local.Time;
        }
        s_motion.RightLow = glm::min(s_motion.RightLow, right);
        s_motion.RightHigh = glm::max(s_motion.RightHigh, right);
        s_motion.LeftLow = glm::min(s_motion.LeftLow, left);
        s_motion.LeftHigh = glm::max(s_motion.LeftHigh, left);
        s_motion.HeadMax = std::max(s_motion.HeadMax, TurnBetween(head, s_motion.Head));
        s_motion.HeadLast = head.Translation.z;
        s_motion.HeadLow = std::min(s_motion.HeadLow, head.Translation.z);
        s_motion.HeadHigh = std::max(s_motion.HeadHigh, head.Translation.z);
        ++s_motion.Samples;
    }

    // a pose's next stage: its clip
    const auto nextStage = [this, pPlayer](Stage aStage, uint8_t aCue)
    {
        LogMotion(m_local);
        m_local.Now = aStage;
        m_local.Time = 0.f;
        if (!PlayClip(pPlayer, m_local, aStage, false))
            spdlog::info("Emotes: {}'s {} didn't play (the game refused its idle)", EmoteAt(m_local.Emote).Name, aStage == Stage::kHold ? "loop" : "way up");
        Share(m_local.Emote, aCue);
    };

    switch (m_local.Now)
    {
    case Stage::kStart:
        if (!IsPose(emote))
        {
            // (moving doesn't end a gesture: it plays on as the player walks)
            if (m_local.Time >= ClipOf(emote, m_local.Female, 0).Duration)
                Finish(m_local, false);
        }
        else if (m_local.Time >= ClipOf(emote, m_local.Female, 0).Duration - kHandOverEarly)
            nextStage(Stage::kHold, RequestMovementAction::kEmoteHold);
        break;
    case Stage::kHold:
        if (moved)
            nextStage(Stage::kStop, RequestMovementAction::kEmoteStop);
        break;
    case Stage::kStop:
        if (m_local.Time >= emote.Stop.Duration - kHandOverEarly)
            Finish(m_local, false);
        break;
    }
}

void EmoteService::Share(uint8_t aEmote, uint8_t aStage) noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;

    RequestMovementAction request{};
    request.Type = RequestMovementAction::kEmote;
    request.Emote.Emote = aEmote;
    request.Emote.Stage = aStage;
    transport.Send(request);
}

// --- the others' copies

void EmoteService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    if (acMessage.Type != RequestMovementAction::kEmote || acMessage.Emote.Emote >= kEmoteCount)
        return;

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor || pActor->IsDead() || !pActor->parentCell || !pActor->currentProcess)
        return;

    auto it = std::find_if(m_remote.begin(), m_remote.end(), [pActor](const Playing& acPlaying) { return acPlaying.ActorId == pActor->formID; });
    const Emote& emote = EmoteAt(acMessage.Emote.Emote);

    switch (acMessage.Emote.Stage)
    {
    case RequestMovementAction::kEmoteStart:
    {
        // in place of one playing: its clip gives way to this one's (kEvents)
        Playing playing{};
        playing.ActorId = pActor->formID;
        playing.Emote = acMessage.Emote.Emote;
        playing.Female = IsFemale(pActor);
        if (it != m_remote.end())
            playing.State = it->State;
        const bool animated = PlayClip(pActor, playing, Stage::kStart, true);
        // the foot IK off for a pose, on for a gesture (they walk through it)
        SetFootIk(pActor, !IsPose(emote));
        if (it != m_remote.end())
            *it = playing;
        else
        {
            pActor->CullWeapons(true);
            m_remote.push_back(playing);
        }
        spdlog::info("Emotes: {:X} does {}{}", pActor->formID, emote.Name, animated ? "" : " (the animation didn't play)");
        break;
    }
    case RequestMovementAction::kEmoteHold:
        // their pose came down: the loop, in step with theirs
        if (it != m_remote.end() && IsPose(emote) && it->Now != Stage::kHold)
        {
            it->Now = Stage::kHold;
            it->Time = 0.f;
            PlayClip(pActor, *it, Stage::kHold, true);
        }
        break;
    case RequestMovementAction::kEmoteStop:
        if (it == m_remote.end())
            break;
        if (IsPose(emote) && it->Now == Stage::kHold)
        {
            it->Now = Stage::kStop;
            it->Time = 0.f;
            PlayClip(pActor, *it, Stage::kStop, true);
        }
        else
        {
            Finish(*it, true);
            m_remote.erase(it);
        }
        break;
    default: break;
    }
}

void EmoteService::UpdateRemote(Playing& aPlaying, float aDelta) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aPlaying.ActorId));
    if (!pActor || pActor->IsDead() || !pActor->parentCell)
    {
        aPlaying.ActorId = 0;
        return;
    }

    aPlaying.Time += aDelta;
    const Emote& emote = EmoteAt(aPlaying.Emote);
    // a gesture ends with its clip; a pose's way down goes on to the loop by itself as theirs does (their kEmoteHold comes
    // a little later), its way up ends with its clip
    const float startDuration = ClipOf(emote, aPlaying.Female, 0).Duration;
    if (aPlaying.Now == Stage::kStart && IsPose(emote) && aPlaying.Time >= startDuration - kHandOverEarly)
    {
        aPlaying.Now = Stage::kHold;
        aPlaying.Time = 0.f;
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(aPlaying.ActorId)))
            PlayClip(pActor, aPlaying, Stage::kHold, true);
    }
    else if ((aPlaying.Now == Stage::kStart && !IsPose(emote) && aPlaying.Time >= startDuration) ||
             (aPlaying.Now == Stage::kStop && aPlaying.Time >= emote.Stop.Duration - kHandOverEarly))
        Finish(aPlaying, true);
}

// --- the game's idles

bool EmoteService::PlayClip(Actor* apActor, Playing& aPlaying, Stage aStage, bool aRemote) noexcept
{
    // the dynamic state the last clip isn't in (kEvents)
    const uint8_t state = aPlaying.State == 0 ? 1 : 0;
    const Clip& clip = ClipOf(EmoteAt(aPlaying.Emote), aPlaying.Female, static_cast<int>(aStage));
    const bool firstPerson = !aRemote && aPlaying.FirstPerson && !aPlaying.BodyPose;
    const bool gesture = !IsPose(EmoteAt(aPlaying.Emote));
    TESIdleForm* pIdle = clip.File ? IdleFor(clip.File, state, firstPerson, gesture) : nullptr;
    if (!pIdle || !apActor->currentProcess)
        return false;

    bool played = false;
    if (aRemote)
    {
        // The animation sync blocks actions on remote characters that it doesn't replay itself
        AllowRemoteIdle(apActor->formID, pIdle->formID);
        g_forceAnimation = true;
        // (a gesture in place of another one: its single state left first)
        if (gesture && aPlaying.State >= 0)
            StopIdle(apActor);
        played = apActor->currentProcess->PlayIdle(apActor, pIdle, nullptr, false);
        g_forceAnimation = false;
    }
    else if (firstPerson)
    {
        // on the first person graph; a gesture's state is a single one, so a clip in place of another one goes out of it
        // first (IdleStop) and in again
        OnGraph(apActor, 1, [&] {
            if (aPlaying.State >= 0)
                StopFirstPersonIdle(apActor);
            played = apActor->currentProcess->PlayIdle(apActor, pIdle, nullptr, false);
        });
    }
    else
    {
        // the local player's on the third person graph, the body's, also while first person's is the active one (seen in
        // game 2026-10-26: played on first person's, the emote did nothing)
        OnBodyGraph(apActor, [&] {
            if (gesture && aPlaying.State >= 0)
                StopIdle(apActor);
            played = apActor->currentProcess->PlayIdle(apActor, pIdle, nullptr, false);
        });
    }
    if (played)
        aPlaying.State = static_cast<int8_t>(state);
    return played;
}

void EmoteService::Finish(Playing& aPlaying, bool aRemote) noexcept
{
    if (Actor* pActor = Cast<Actor>(TESForm::GetById(aPlaying.ActorId)))
    {
        if (aRemote)
        {
            g_forceAnimation = true;
            StopIdle(pActor);
            g_forceAnimation = false;
            SetFootIk(pActor, true);
        }
        else if (!aPlaying.FirstPerson || aPlaying.BodyPose)
        {
            OnBodyGraph(pActor, [pActor] {
                StopIdle(pActor);
                SetFootIk(pActor, true);
            });
            // (after the IdleStop: the body's graph takes it as the active one)
            if (aPlaying.BodyPose)
                ReleaseBodyGraph(pActor, IsFirstPerson());
        }
        // Started in first person (a gesture's clip or a pose): the first person graph out of its dynamic state, whatever
        // the view is now, and again a moment later (m_stopAgainIn); its flags read and bAnimationDriven off a moment
        // after (FreeFirstPersonGraph). After first person emotes the player couldn't walk while that graph was the
        // active one (seen in game 2026-10-27: a clap cut short 0.5 s in, a first person sit that ended in third person,
        // the bird)
        if (!aRemote && aPlaying.FirstPerson)
        {
            StopFirstPersonIdle(pActor);
            m_stopAgainIn = 0.2f;
            m_stopsAgain = 2;
            m_freeIn = 0.8f;
            m_freeWhen = "after a first person emote";
        }
        pActor->CullWeapons(false);
    }

    // the body's weapon back, and the view back to first person if it went to third person for the emote
    if (!aRemote)
    {
        if (s_heldManager)
            ReleaseBodyGraph(nullptr, false);
        s_holdingBody = false;
        LogMotion(aPlaying);
        ShowBodyWeapon();
        EndFirstPersonPose();
        if (m_freeCamera)
            SetCameraFree(false);
        m_freeCamera = false;
        s_playerEmoting = false;
        if (m_restoreFirstPerson)
            SetCameraState(kFirstPersonState);
        m_restoreFirstPerson = false;

        // the weapon put away for the emote drawn again, unless the player is gone, loading, or has it out already
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (std::exchange(m_redraw, false) && pPlayer && !pPlayer->IsDead() && !IsMenuOpen("LoadingMenu") &&
            pPlayer->actorState.GetWeaponState() == kWeaponAway)
            pPlayer->SetWeaponDrawn(true);
    }
    aPlaying.ActorId = 0;
}

TESIdleForm* EmoteService::IdleFor(const char* acpFile, uint8_t aState, bool aFirstPerson, bool aGesture) noexcept
{
    const std::string key = fmt::format("{}|{}", acpFile, aFirstPerson ? "1p" : aGesture ? "3p" : aState ? "3p1" : "3p0");
    if (const auto it = m_idles.find(key); it != m_idles.end())
        return it->second;

    auto* pIdle = IFormFactory::Create<TESIdleForm>();
    if (!pIdle)
        return nullptr;
    // first person: the graph the vanilla first person landing idle plays on
    const char* pGraph = kThirdPersonGraph;
    if (aFirstPerson)
    {
        const auto* pVanilla = Cast<TESIdleForm>(TESForm::GetById(kIdle1stPJumpImpactLand));
        if (!pVanilla || !pVanilla->behaviorGraphName.AsAscii() || !*pVanilla->behaviorGraphName.AsAscii())
        {
            spdlog::warn("Emotes: no first person graph to play the emotes on");
            return nullptr;
        }
        pGraph = pVanilla->behaviorGraphName.AsAscii();
    }
    const char* pEvent = aFirstPerson || aGesture ? kGestureEvent : kEvents[aState];
    pIdle->behaviorGraphName.Set(pGraph);
    pIdle->animEventName.Set(pEvent);
    pIdle->animFileName.Set(fmt::format("{}{}", aFirstPerson ? kFirstPersonAnimations : kAnimations, acpFile).c_str());
    pIdle->idleFlags = 1;
    m_idles[key] = pIdle;
    spdlog::info("Emotes: idle {:X} for {} ({}, {})", pIdle->formID, acpFile, aFirstPerson ? "first person" : "third person", pEvent);
    return pIdle;
}

#endif

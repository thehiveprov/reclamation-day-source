#include <Services/ParkourService.h>

#if TP_FALLOUT4

#include <Services/EmoteService.h>
#include <Services/GrappleService.h>
#include <Services/MovementService.h>
#include <Events/UpdateEvent.h>
#include <World.h>
#include <Components.h>
#include <Services/TransportService.h>

#include <Messages/RequestMovementAction.h>
#include <Messages/NotifyMovementAction.h>

#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESIdleForm.h>
#include <Forms/BGSAction.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>
#include <Havok/bhkCharacterController.h>
#include <BSAnimationGraphManager.h>
#include <Games/Fallout4/BikeLink.h>
#include <Games/IFormFactory.h>
#include <Games/ActorExtension.h>
#include <Games/TES.h>
#include <Forms/TESNPC.h>
#include <Forms/TESRace.h>
#include <Components/BGSKeywordForm.h>
#include <Audio/FileSound.h>
#include <Interface/HudMarker.h>
#include <Services/PapyrusService.h>
#include <Services/Vats76Service.h>

#include <mutex>
#include <random>
#include <variant>
#include <PerfProbe.h>

namespace
{
using ParkourMoves::Move;

constexpr float kPi = 3.14159265f;
// A standing character's height, as the original's ledge rules have it (120) and ours (the capsule, 128)
constexpr float kPlayerHeight = 128.f;
constexpr float kRuleHeight = 120.f;

// The original's ledge rules (game units over the feet, times the character's scale)
constexpr float kClimbMin = 30.f;
constexpr float kClimbMax = 250.f;
constexpr float kHighestFrom = 220.f;
constexpr float kHighFrom = 170.f;
constexpr float kMediumFrom = 130.f;
constexpr float kLowFrom = 80.f;
constexpr float kStepHighFrom = 60.f;
constexpr Move ClimbFor(float aRise) noexcept
{
    return aRise >= kHighestFrom ? Move::kClimbHighest : aRise >= kHighFrom ? Move::kClimbHigh : aRise >= kMediumFrom ? Move::kClimbMedium : Move::kClimbLow;
}
// a step needs this much rise standing still, up to twice as much at a run (otherwise it's walked onto)
constexpr float kStepMin = 50.f;
// a step's or a grab's ledge is at most this far ahead
constexpr float kStepReach = 70.f;
constexpr float kVaultMin = 42.f;
constexpr float kVaultMax = 120.f;
constexpr float kGrabMax = 125.f;
constexpr float kGrabHighFrom = 75.f;
// nothing this far under the feet for a grab (otherwise it's a climb from the ground)
constexpr float kGrabClearBelow = 35.f;
// a climb as high as the head or higher, while running with nothing at the face, is running past it, not a climb
constexpr float kSmartClimbShare = 0.2f;
constexpr float kSmartClimbLook = 150.f;
// in the air at least this long (s): a grab, not the jump itself
constexpr float kGrabAfterFall = 0.17f;
constexpr float kMaxGrabFall = 450.f;

// Where each move's animation expects to start: this far back from the ledge (the point just over its edge), and
// this far under its top (the animation's rise less 5). From the original.
struct Approach
{
    float Back;
    float Rise;
};
constexpr Approach kApproach[] = {
    /* kFail */ {0.f, 0.f},          /* kGrab */ {45.f, 50.f},     /* kGrabHigh */ {45.f, 95.f},
    /* kVault */ {55.f, 0.f},        /* kStepLowLeft */ {15.f, 45.f}, /* kStepLowRight */ {15.f, 45.f},
    /* kStepHighLeft */ {15.f, 65.f}, /* kStepHighRight */ {15.f, 65.f}, /* kClimbLow */ {55.f, 105.f},
    /* kClimbMedium */ {55.f, 148.f}, /* kClimbHigh */ {55.f, 195.f},    /* kClimbHighest */ {62.f, 245.f},
    /* kLandRoll (on from the landing, no ledge) */ {0.f, 0.f},
};
static_assert(std::size(kApproach) == static_cast<size_t>(Move::kCount));
// how far the vault animation travels past where it expects the obstacle's near side
constexpr float kVaultOver = 69.f;
// the vault animation clears an obstacle this high; a higher one gets the difference as a bump
constexpr float kVaultClears = 55.f;

// The way into the animation's start is eased over this long (or 40% of a short move); the way onto the ledge's real
// height and standing spot over the move's last 40%
constexpr float kEaseIn = 0.35f;
constexpr float kEaseShare = 0.4f;
// never eased backwards more than this (a ledge closer than the animation expects): the move starts where it is
constexpr float kMaxBackEase = 20.f;

// Ledge search (from our mantling): straight down onto what's ahead, nearest first
constexpr float kFirstSample = 8.f;
constexpr float kSampleStep = 5.f;
constexpr float kBelowFeet = 48.f;
constexpr float kMinStep = 12.f;
constexpr float kMinTopNormalZ = 0.6f;
constexpr float kMaxLandingRise = 44.f;
// where the character ends up standing past the edge
constexpr float kLandingDepth = 28.f;
constexpr float kBodyRadius = 20.f;
constexpr float kClearance = 8.f;
constexpr float kMaxDropBehind = 48.f;
constexpr float kGroundSearch = 300.f;

// Speeds (game units/s): a run, and the least a vault needs
constexpr float kRunSpeed = 350.f;
constexpr float kMinVaultSpeed = 100.f;
// The character can turn this far from the move's heading during it (the view moves, the move doesn't)
constexpr float kYawFreedom = 0.5f;
// A move is stopped if its character gets this far from its way (a load, a teleport)
constexpr float kMaxOffWay = 200.f;
// Momentum handed back after a move, for this long, when it ends moving (a vault, a step)
constexpr float kHandBackTime = 0.12f;
constexpr float kMinHandBackSpeed = 60.f;
// A move's body is up once its clip's travel stays this close to where it ends (a share of the whole travel, at least
// kExitNear units): from then on a held direction hurries the rest of the move along at kHurry times its pace and ends
// its idle, into walking or running, rather than the player standing through the clip's last fraction of a second
constexpr float kExitShare = 0.02f;
constexpr float kExitNear = 3.f;
constexpr float kHurry = 3.f;
// Fallout4.esm: the action that ends an idle into the graph's normal state, blended (ActionIdleStop)
constexpr uint32_t kActionIdleStop = 0x18BA8;
constexpr float kHavokScale = 0.0142875f;

// Sounds: the vanilla player jump and landing sounds (wood), and our climb sounds for the hands (STRADAT's, from
// Mantle for New Vegas, used with permission), played the way the vanilla wood jump sound plays
constexpr uint32_t kSoundTemplate = 0x002313C8;
const std::vector<std::string> kSoundTemplateFiles{"FST_Wood_JumpUp_Player_01", "FST_Wood_JumpUp_Player_02"};
constexpr char kSoundTemplateFolder[] = "Sound\\FX\\FST\\Player\\Wood";
constexpr const char* kJumpSounds[] = {"Sound\\FX\\FST\\Player\\Wood\\FST_Wood_JumpUp_Player_01", "Sound\\FX\\FST\\Player\\Wood\\FST_Wood_JumpUp_Player_02"};
constexpr const char* kLandSounds[] = {"Sound\\FX\\FST\\Player\\Wood\\FST_Wood_JumpDown_Player_01", "Sound\\FX\\FST\\Player\\Wood\\FST_Wood_JumpDown_Player_02"};
constexpr char kTouchSoundPrefix[] = "Sound\\FX\\ReclamationDay\\MantleClimb";
constexpr int kTouchSoundCount = 10;

// The moves play as dynamic idles. Third person on the human graph, with the event that also tucks a weapon away for
// the animation (it plays in every weapon state; the plain one doesn't with the weapon away). First person on the
// first person graph (its path taken from the vanilla first person landing idle), with the plain event: the one every
// vanilla first person dynamic idle has, and the only one of the two that plays there (seen in game).
constexpr char kThirdPersonGraph[] = "Actors\\Character\\Behaviors\\RaiderRootBehavior.hkx";
constexpr const char* kIdleEvents[] = {"dyn_ActivationCullWeapons", "dyn_Activation"};
constexpr uint32_t kIdle1stPJumpImpactLand = 0x0005B339;
// The idles' animation driven movement puts the character back where the move started once more as it ends (a frame
// after the move): for this long after a move, the game's placing of its player that would move it this far at once
// is skipped (far more than a frame of running; the move's own end stays)
constexpr auto kSettleTime = std::chrono::milliseconds(300);
constexpr float kMaxSettleJump = 40.f;
// The HUD marker looks for a move this often (s)
constexpr float kMarkerInterval = 0.1f;

// The landing roll (the original's): crouch pressed after this long in the air, with the ground at most this far under
// the feet, gets it ready; it stays ready this long for the landing
constexpr float kRollMinFall = 0.5f;
constexpr float kRollReach = 100.f;
constexpr float kRollReadyTime = 1.f;
// as in the original, the roll takes this much off the height a fall's damage is for, for a fall of more than this
constexpr float kRollFallEase = 100.f;
constexpr float kRollEasesFallOver = 200.f;
// The momentum carried through the roll (its animation rolls in place): this share of the speed it landed with, at
// most this fast, the way it was moving when that's at least this fast (else the way the character faces)
constexpr float kRollCarry = 0.85f;
constexpr float kRollMaxSpeed = 520.f;
constexpr float kRollMinTurnSpeed = 60.f;
// the way on is looked along in steps this long at knee height, and the ground has to stay this close to the landing's
constexpr float kRollCheckStep = 20.f;
constexpr float kRollKneeHeight = 35.f;
constexpr float kRollGroundBelow = 40.f;
constexpr float kRollGroundAbove = 30.f;
// the player's landing isn't a stumble while the roll is ready, nor for this long after it starts
constexpr auto kRollHoldsLanding = std::chrono::milliseconds(500);

// ActorState bits (as MovementService reads them)
constexpr uint32_t kMoveModeSwimming = 1u << 10;
constexpr uint32_t kStanceSneaking = 1;
// L_LOS collision layer (hits statics, terrain, props; and characters, told apart by their layers)
constexpr uint32_t kLayerLineOfSight = 41;
// L_LOS goes through what can be seen through but not walked through (chain-link fences, grates, glass: layers
// L_TRANSPARENT, L_TRANSPARENT_SMALL, L_TRANSPARENT_SMALL_ANIM, which the character controller collides with). L_CAMERA
// hits those too (and statics, terrain, characters; not props or clutter, not triggers, water or invisible walls): every
// ray is also cast with it, and kept when it hits one of those first (COLL records in Fallout4.esm)
constexpr uint32_t kLayerCamera = 39;
constexpr uint32_t kTransparentLayers[] = {3, 26, 28};
constexpr size_t kPickDataSize = 0xE0;

// Human NPCs' moves (docs/NPC_PARKOUR.md): only the human race, as the moves are animated on its skeleton
constexpr uint32_t kHumanRace = 0x13746;
// They're looked at this often (s, the game's own time: not while it's paused), at most this many move at once, with
// at most one move planned a look (a plan is a few dozen rays)
constexpr float kNpcInterval = 0.25f;
constexpr size_t kMaxNpcMoves = 4;
// An NPC that made a move, or that had a look at one and couldn't, isn't considered again for this long (s)
constexpr float kNpcCooldown = 5.f;
constexpr float kNpcRetry = 2.f;
// Slower than this it's standing (game units/s)
constexpr float kNpcStillSpeed = 30.f;
// Standing this long (s): told to walk a path and in combat, with no path to its target, and told to walk a path
// otherwise
constexpr float kNpcStuckInCombat = 0.5f;
constexpr float kNpcNoPath = 1.f;
constexpr float kNpcStuck = 2.f;
// Who it's after is close enough at this distance and height (nothing to climb for)
constexpr float kNpcCloseEnough = 120.f;
constexpr float kNpcCloseEnoughHeight = 60.f;
// On the run, it has to be going about the way its target is (radians)
constexpr float kNpcOnTheWay = 1.05f;
// A follower this far from the player is behind
constexpr float kNpcFollowerBehind = 400.f;
// What's looked for at knee height before the plan's rays: nothing this far ahead, nothing to climb or vault
constexpr float kNpcKneeHeight = 35.f;
constexpr float kNpcLookAhead = 110.f;
// NPC slides (docs/ENEMY_TACTICS.md): fast enough to slide, how long before the same NPC slides again, how many at once
constexpr float kNpcSlideMinSpeed = 280.f;
constexpr float kNpcSlideCooldown = 8.f;
constexpr size_t kMaxNpcSlides = 2;
// into cover: from this far from the spot the game picked, stopping this short of it
constexpr float kCoverSlideMin = 250.f;
constexpr float kCoverSlideMax = 650.f;
constexpr float kCoverSlideStopShort = 60.f;
// at its target: from this far, over this much clear ground, this likely at each look (melee fighters more)
constexpr float kChargeSlideMin = 300.f;
constexpr float kChargeSlideMax = 800.f;
constexpr float kChargeSlideLane = 420.f;
constexpr float kChargeSlideChance = 0.12f;
constexpr float kChargeSlideChanceUpClose = 0.35f;
// the way it's going against the way there (cos, 35 degrees), and how much higher or lower that may be
constexpr float kSlideOnTheWayCos = 0.82f;
constexpr float kSlideMaxRise = 50.f;
// The navmesh search found this close to a point is under it
constexpr float kNavmeshNear = 48.f;
// A landing with no navmesh is only for going right after someone on it: this close, and this close in height
constexpr float kChaseOffNavmesh = 300.f;
constexpr float kChaseOffNavmeshHeight = 64.f;
// What the game's own "move to nearest navmesh location" looks for: triangles this far over the point at most
constexpr uint8_t kNavmeshFilterFlags = 1;
constexpr float kNavmeshFilterBelow = 64.f;
// BSPathingLocation, and the pathing cell in it
constexpr size_t kPathingLocationSize = 0x30;
constexpr size_t kPathingLocationSpace = 0x20;
constexpr size_t kNavmeshFilterSize = 0x18;
// ActorState in the actor, whose IMovementState slot 2 is DoGetPathingLocation; and what Actor::IsPathing asks
constexpr size_t kActorMovementState = 0x128;
constexpr size_t kMovementStateGetLocation = 2;
constexpr size_t kActorMovementController = 0x308;
// How long everyone but the local player takes to turn to a move's heading (s)
constexpr float kTurnInTime = 0.15f;
// Fighting up close: the melee and unarmed weapons' keywords (Fallout4.esm), and the game's UnarmedDefault
constexpr uint32_t kWeaponTypeMelee1H = 0x4A0A4;
constexpr uint32_t kWeaponTypeMelee2H = 0x4A0A5;
constexpr uint32_t kWeaponTypeUnarmed = 0x5240E;
constexpr uint32_t kUnarmedDefault = 0x1F4;
constexpr uintptr_t kWeaponKeywordsOffset = 0x150;

using TAvfSetVelocity = void (*)(std::monostate, Actor*, float, float, float, float, float, float, float, bool);

ParkourService* s_pParkourService = nullptr;

// The references a move has the collision of (the local player's, the NPCs' this game runs, and other games' copies)
std::array<std::atomic<const void*>, 16> s_moving{};
// set while the service places a moving reference itself (the game's own placing of one is skipped meanwhile)
thread_local bool s_placing = false;
// the characters whose move just ended, each until when (steady clock ticks)
struct Settling
{
    std::atomic<const void*> Reference{nullptr};
    std::atomic<int64_t> Until{0};
};
std::array<Settling, 8> s_settling{};
// the player's landing is a roll's until then (steady clock ticks)
std::atomic<int64_t> s_rollsLandingUntil{0};

// A place in the moving list for a reference; false when they're all taken
bool TakeMovingSlot(const void* apReference) noexcept
{
    for (auto& slot : s_moving)
    {
        const void* pFree = nullptr;
        if (slot.compare_exchange_strong(pFree, apReference))
            return true;
    }
    return false;
}

void ReleaseMovingSlot(const void* apReference) noexcept
{
    for (auto& slot : s_moving)
    {
        if (slot.load() == apReference)
            slot = nullptr;
    }
}

void MarkSettling(const void* apReference) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : s_settling)
    {
        if (entry.Reference.load() == apReference || entry.Until.load() < now.time_since_epoch().count())
        {
            entry.Until = (now + kSettleTime).time_since_epoch().count();
            entry.Reference = apReference;
            return;
        }
    }
}

struct RayHit
{
    bool Hit{false};
    NiPoint3 Point{};
    float NormalZ{0.f};
    uint32_t Layer{0};
};

// The character's own collision group with the line of sight layer, as the game looks along a line from a character
uint32_t GetRayFilter(Actor* apActor) noexcept
{
    using TGetCollisionFilter = uint32_t*(Actor*, uint32_t*);
    POINTER_FALLOUT4(TGetCollisionFilter, s_getCollisionFilter, 1474996);

    uint32_t filter = 0;
    s_getCollisionFilter.Get()(apActor, &filter);
    return (filter & 0xFFFF0000u) | kLayerLineOfSight;
}

// The closest thing on the line for one collision layer, as the game picks (bhkPickData, TESObjectCELL::Pick)
RayHit PickOnce(TESObjectCELL* apCell, uint32_t aFilter, const NiPoint3& acFrom, const NiPoint3& acTo) noexcept
{
    using TConstruct = void*(void*);
    using TSetStartEnd = void(void*, const NiPoint3*, const NiPoint3*);
    using TPick = void*(TESObjectCELL*, void*);
    using THasHit = bool(void*);
    using TGetHitFraction = float(void*);
    using TDestruct = void(void*);
    POINTER_FALLOUT4(TConstruct, s_construct, 526784);
    POINTER_FALLOUT4(TSetStartEnd, s_setStartEnd, 747471);
    POINTER_FALLOUT4(TPick, s_pick, 434718);
    POINTER_FALLOUT4(THasHit, s_hasHit, 1181585);
    POINTER_FALLOUT4(TGetHitFraction, s_getHitFraction, 476688);
    // bhkPickData's destructor (true id 109425): the pick keeps a reference to the physics world it looked in (at 0xC0,
    // taken by bhkWorld::PickObject, id 547249), which only this lets go of. Havok counts references in 16 bits: left
    // out, every ray leaked one and after 65536 of them the exterior's world was freed while still in use (the crashes
    // after "Havok: world ... destroyed", 2026-10-13 to 16)
    POINTER_FALLOUT4(TDestruct, s_destruct, 109426);

    alignas(16) uint8_t pickData[kPickDataSize]{};
    s_construct.Get()(pickData);
    *reinterpret_cast<uint32_t*>(pickData + 0x0C) = aFilter;
    s_setStartEnd.Get()(pickData, &acFrom, &acTo);
    s_pick.Get()(apCell, pickData);

    RayHit hit{};
    if (s_hasHit.Get()(pickData))
    {
        const float fraction = std::clamp(s_getHitFraction.Get()(pickData), 0.f, 1.f);
        hit.Hit = true;
        hit.Point = NiPoint3(glm::vec3(acFrom) + (glm::vec3(acTo) - glm::vec3(acFrom)) * fraction);
        hit.NormalZ = *reinterpret_cast<const float*>(pickData + 0x78);
        hit.Layer = *reinterpret_cast<const uint32_t*>(pickData + 0xAC) & 0x7F;
    }
    s_destruct.Get()(pickData);
    return hit;
}

// The closest thing on the line the character couldn't walk through: the line of sight's hit, or a see-through
// obstacle's (a chain-link fence) nearer than it
RayHit CastRay(TESObjectCELL* apCell, uint32_t aFilter, const NiPoint3& acFrom, const NiPoint3& acTo) noexcept
{
    const RayHit sight = PickOnce(apCell, aFilter, acFrom, acTo);
    const RayHit camera = PickOnce(apCell, (aFilter & ~0x7Fu) | kLayerCamera, acFrom, acTo);
    if (!camera.Hit || std::find(std::begin(kTransparentLayers), std::end(kTransparentLayers), camera.Layer) == std::end(kTransparentLayers))
        return sight;
    if (sight.Hit && glm::distance(glm::vec3(sight.Point), glm::vec3(acFrom)) <= glm::distance(glm::vec3(camera.Point), glm::vec3(acFrom)))
        return sight;
    return camera;
}

bool IsCharacterLayer(uint32_t aLayer) noexcept
{
    return aLayer == 8 || aLayer == 30 || aLayer == 32;
}

// PlayerCamera (CommonLibF4 id 1171980): TESCamera::currentState at 0x28, TESCameraState::id at 0x20, 0 = first person
bool IsFirstPerson() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    const uint8_t* pCamera = *s_playerCamera.Get();
    if (!pCamera)
        return false;

    const uint8_t* pState = *reinterpret_cast<const uint8_t* const*>(pCamera + 0x28);
    return pState && *reinterpret_cast<const uint32_t*>(pState + 0x20) == 0;
}

// The human graph's foot IK (bHumanoidFootIKDisable): left on during a move it pulls the feet down to the ground under
// the character, straightening the legs the animation tucks up (seen in game: a knee the clip bends to 30 degrees held
// at 115-157, jittering every frame)
void SetFootIk(Actor* apActor, bool aOn) noexcept
{
    static BSFixedString s_footIkDisable("bHumanoidFootIKDisable");
    apActor->animationGraphHolder.SetVariableBool(&s_footIkDisable, !aOn);
}

// Full Body First Person (a bundled mod) shows the third person body in first person, run by the player's third person
// graph: the first of the two, first person's being the second and the active one then. A move plays on the active one
// only, so with that mod the body plays it too, or it stands upright through it while the view climbs (seen in game).
bool IsBodyShownInFirstPerson() noexcept
{
    static const bool s_loaded = GetModuleHandleW(L"FullBodyFirstPerson.dll") != nullptr;
    return s_loaded;
}

// Runs aFunction with the player's third person graph as the active one, while first person's is
template <class T> void OnBodyGraph(Actor* apPlayer, T&& aFunction) noexcept
{
    BSAnimationGraphManager* pManager = nullptr;
    apPlayer->animationGraphHolder.GetBSAnimationGraph(&pManager);
    if (!pManager)
        return;

    if (pManager->animationGraphs.size >= 2 && pManager->animationGraphIndex == 1)
    {
        pManager->animationGraphIndex = 0;
        aFunction();
        pManager->animationGraphIndex = 1;
    }
    pManager->Release();
}


TAvfSetVelocity GetAvfSetVelocity() noexcept
{
    static TAvfSetVelocity s_pSetVelocity = nullptr;
    if (!s_pSetVelocity)
    {
        if (const HMODULE hAvf = GetModuleHandleA("ActorVelocityFramework.dll"))
            s_pSetVelocity = reinterpret_cast<TAvfSetVelocity>(GetProcAddress(hAvf, "SetVelocity"));
    }
    return s_pSetVelocity;
}

float Smoothstep(float aX) noexcept
{
    aX = std::clamp(aX, 0.f, 1.f);
    return aX * aX * (3.f - 2.f * aX);
}

// The character's horizontal velocity from its character controller (game units/s; 0 without one)
void GetGroundVelocity(PlayerCharacter* apPlayer, float& aX, float& aY) noexcept
{
    aX = 0.f;
    aY = 0.f;
    bhkCharacterController* pController = apPlayer->GetCharController();
    if (!pController)
        return;
    bhkCharacterController::Vector4 velocity{};
    pController->GetLinearVelocity(velocity);
    if (std::isfinite(velocity.x) && std::isfinite(velocity.y))
    {
        aX = velocity.x / kHavokScale;
        aY = velocity.y / kHavokScale;
    }
}

// The horizontal speed the character is moving at (game units/s) and how much of it is along the way it faces
void GetGroundSpeed(Actor* apActor, float& aSpeed, float& aForward) noexcept
{
    aSpeed = 0.f;
    aForward = 0.f;
    bhkCharacterController* pController = apActor->GetCharController();
    if (!pController)
        return;

    bhkCharacterController::Vector4 velocity{};
    pController->GetLinearVelocity(velocity);
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y))
        return;

    const float x = velocity.x / kHavokScale;
    const float y = velocity.y / kHavokScale;
    aSpeed = std::hypot(x, y);
    aForward = x * std::sin(apActor->rotation.z) + y * std::cos(apActor->rotation.z);
}

// The game's frame time, what it updates actors by: slowed with the world, none while it's paused
float GameFrameTime() noexcept
{
    POINTER_FALLOUT4(float, s_frameTime, 112551);
    return *s_frameTime.Get();
}

// Not crouched (that's also every slide), swimming, knocked down or using furniture
bool IsBodyFree(Actor* apActor) noexcept
{
    const auto& state = apActor->actorState;
    if (state.GetStance() == kStanceSneaking || (state.GetMoveMode() & kMoveModeSwimming))
        return false;
    return ((state.flags1 >> 21) & 0xF) == 0 && ((state.flags2 >> 18) & 0x3) == 0;
}

// A human NPC this game runs, on its feet and free to make a move
bool IsFreeHumanNpc(Actor* apActor) noexcept
{
    if (!apActor || apActor == PlayerCharacter::Get() || !apActor->parentCell || !apActor->currentProcess || apActor->IsDead() || apActor->IsInPowerArmor())
        return false;
    // Riding a raider's bike, or sitting on a caravan truck (the game's restrained life state, flags1 bits 17-20)
    BikeLink::Rider rider{};
    if (((apActor->actorState.flags1 >> 17) & 0xF) != 0 || BikeLink::GetRider(apActor->formID, rider))
        return false;
    const auto* pExtension = apActor->GetExtension();
    if (pExtension->IsPlayer() || pExtension->IsRemote())
        return false;
    const auto* pNpc = Cast<TESNPC>(apActor->baseForm);
    if (!pNpc || !pNpc->raceForm.race || pNpc->raceForm.race->formID != kHumanRace)
        return false;
    if (apActor->actorState.IsBleedingOut() || !IsBodyFree(apActor))
        return false;
    const bhkCharacterController* pController = apActor->GetCharController();
    return pController && pController->GetFallTime() < kGrabAfterFall;
}

// Whether the game has the actor walking a path (Actor::IsPathing, CommonLibF4 id 989661): the controller it asks has to
// be there, as it doesn't look
bool IsPathing(Actor* apActor) noexcept
{
    using TIsPathing = bool(Actor*);
    POINTER_FALLOUT4(TIsPathing, s_isPathing, 989662);

    const auto* pBytes = reinterpret_cast<const uint8_t*>(apActor);
    if (!*reinterpret_cast<const void* const*>(pBytes + kActorMovementController))
        return false;
    return s_isPathing.Get()(apActor);
}

// In a fight it has to get up close for: the hands, or a melee weapon, in the main hand
bool FightsUpClose(Actor* apActor) noexcept
{
    const TESForm* pWeapon = apActor->GetEquippedWeapon(0);
    if (!pWeapon || pWeapon->formID == kUnarmedDefault)
        return true;
    if (pWeapon->formType != FormType::Weapon)
        return false;
    const auto* pKeywords = reinterpret_cast<const BGSKeywordForm*>(reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponKeywordsOffset);
    for (const uint32_t keyword : {kWeaponTypeMelee1H, kWeaponTypeMelee2H, kWeaponTypeUnarmed})
    {
        if (const TESForm* pKeyword = TESForm::GetById(keyword); pKeyword && pKeywords->HasKeyword(pKeyword))
            return true;
    }
    return false;
}

// Whether there's navmesh at a point, by the game's own search, the one its "move to nearest navmesh location" makes
// (BSPathingLocation at the point, in the actor's pathing cell, and its FindTriangleForLocationFilter). With no triangle
// under the point the search moves it to the nearest there is, so where it ends up says whether it found one here.
bool HasNavmeshAt(Actor* apActor, const NiPoint3& acPoint) noexcept
{
    using TConstruct = void*(void*);
    using TConstructAt = void*(void*, const NiPoint3*, void* const*);
    using TDestruct = void(void*);
    using TConstructFilter = void*(void*, uint8_t, float, float);
    using TFind = bool(void*, const void*);
    using TGetLocation = void(void*, void*);
    POINTER_FALLOUT4(TConstruct, s_construct, 506342);
    POINTER_FALLOUT4(TConstructAt, s_constructAt, 39877);
    POINTER_FALLOUT4(TDestruct, s_destruct, 983102);
    POINTER_FALLOUT4(TConstructFilter, s_constructFilter, 523394);
    POINTER_FALLOUT4(TFind, s_find, 725365);

    alignas(16) uint8_t here[kPathingLocationSize]{};
    s_construct.Get()(here);
    void* pState = reinterpret_cast<uint8_t*>(apActor) + kActorMovementState;
    reinterpret_cast<TGetLocation*>((*reinterpret_cast<void***>(pState))[kMovementStateGetLocation])(pState, here);

    bool found = false;
    void* const* ppSpace = reinterpret_cast<void* const*>(here + kPathingLocationSpace);
    if (*ppSpace)
    {
        alignas(16) uint8_t there[kPathingLocationSize]{};
        s_constructAt.Get()(there, &acPoint, ppSpace);
        alignas(16) uint8_t filter[kNavmeshFilterSize]{};
        s_constructFilter.Get()(filter, kNavmeshFilterFlags, 0.f, kNavmeshFilterBelow);
        if (s_find.Get()(there, filter))
        {
            const auto* pFound = reinterpret_cast<const float*>(there);
            found = std::hypot(pFound[0] - acPoint.x, pFound[1] - acPoint.y) <= kNavmeshNear && std::abs(pFound[2] - acPoint.z) <= kNavmeshNear;
        }
        s_destruct.Get()(there);
    }
    s_destruct.Get()(here);
    return found;
}

void PlayRandom(std::vector<std::unique_ptr<FileSound>>& aSounds, Actor* apActor) noexcept
{
    if (aSounds.empty())
        return;

    static std::minstd_rand s_random(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    aSounds[std::uniform_int_distribution<size_t>(0, aSounds.size() - 1)(s_random)]->Play(apActor);
}

// The game's switch for a reference's collision (what console "tcl" uses, true id 660095): sets or clears bit 0x800
// of the flags at its loaded data (+0xF0) + 0x20. With it set, the actor movement update (957046) goes around the
// character controller: no collision, no gravity. The player's own update (263152) clears it every frame unless the
// game's no clip is on, so the hook keeps it set for a reference a move is playing.
using TSetCollisionDisabled = void(TESObjectREFR*, bool);
TSetCollisionDisabled* RealSetCollisionDisabled = nullptr;

void HookSetCollisionDisabled(TESObjectREFR* apReference, bool aDisabled)
{
    if (!aDisabled && apReference && ParkourService::IsMoving(apReference))
        return;
    RealSetCollisionDisabled(apReference, aDisabled);
}

// The game's jump control (JumpHandler::OnButtonEvent, client id 882499): it jumps on the press itself and gets the
// same event every frame jump stays down. A press that starts a parkour move doesn't jump.
struct JumpHandler;
TP_THIS_FUNCTION(TJumpButtonEvent, void, JumpHandler, const void* apEvent);
TJumpButtonEvent* RealJumpButtonEvent = nullptr;

void TP_MAKE_THISCALL(HookJumpButtonEvent, JumpHandler, const void* apEvent)
{
    if (s_pParkourService && apEvent)
    {
        // ButtonEvent (CommonLibF4): value at 0x38, heldDownSecs at 0x3C
        const auto* pBytes = static_cast<const uint8_t*>(apEvent);
        const float value = *reinterpret_cast<const float*>(pBytes + 0x38);
        const float heldDownSecs = *reinterpret_cast<const float*>(pBytes + 0x3C);
        if (s_pParkourService->OnJumpButton(value != 0.f, value != 0.f && heldDownSecs == 0.f))
            return;
    }

    TiltedPhoques::ThisCall(RealJumpButtonEvent, apThis, apEvent);
}

// The game's sneak control (SneakHandler::OnButtonEvent: slot 8 of its vtable, as the jump control is of its own;
// true id 399406): it toggles sneaking when the button goes up. A press that gets the landing roll ready, and its
// release, don't sneak.
struct SneakHandler;
TP_THIS_FUNCTION(TSneakButtonEvent, void, SneakHandler, const void* apEvent);
TSneakButtonEvent* RealSneakButtonEvent = nullptr;

void TP_MAKE_THISCALL(HookSneakButtonEvent, SneakHandler, const void* apEvent)
{
    if (s_pParkourService && apEvent)
    {
        const auto* pBytes = static_cast<const uint8_t*>(apEvent);
        const float value = *reinterpret_cast<const float*>(pBytes + 0x38);
        const float heldDownSecs = *reinterpret_cast<const float*>(pBytes + 0x3C);
        if (s_pParkourService->OnSneakButton(value != 0.f, value != 0.f && heldDownSecs == 0.f))
            return;
    }

    TiltedPhoques::ThisCall(RealSneakButtonEvent, apThis, apEvent);
}

// The game's combat AI taking an NPC into cover: CombatEnterCoverPathController's update (its vtable's slot 6, id
// 356879) with the path it's on. The path has the actor's handle at 0x10; the controller has the cover location at
// 0x18, whose spot is at 0x8C while its state (0xA8) is 2 to 5, else at 0x80 (read from this function). The AI may run
// it off the main thread: it only notes where each NPC is going, for UpdateNpcs.
struct CoverWish
{
    uint32_t Handle;
    float At[3];
};
std::mutex s_coverLock;
std::vector<CoverWish> s_coverWishes;

using TEnterCoverUpdate = void(void*, void*);
TEnterCoverUpdate* RealEnterCoverUpdate = nullptr;

void HookEnterCoverUpdate(void* apController, void* apPath)
{
    const auto* pController = static_cast<const uint8_t*>(apController);
    const auto* pCover = pController && apPath ? *reinterpret_cast<const uint8_t* const*>(pController + 0x18) : nullptr;
    if (pCover)
    {
        const uint32_t state = *reinterpret_cast<const uint32_t*>(pCover + 0xA8);
        const auto* pAt = reinterpret_cast<const float*>(pCover + (state - 2u <= 3u ? 0x8C : 0x80));
        const CoverWish wish{*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(apPath) + 0x10), {pAt[0], pAt[1], pAt[2]}};

        std::scoped_lock lock(s_coverLock);
        const auto it = std::find_if(s_coverWishes.begin(), s_coverWishes.end(), [&wish](const CoverWish& acOther) { return acOther.Handle == wish.Handle; });
        if (it != s_coverWishes.end())
            *it = wish;
        else if (s_coverWishes.size() < 64)
            s_coverWishes.push_back(wish);
    }

    RealEnterCoverUpdate(apController, apPath);
}
} // namespace

// Animation.cpp: lets an action through on a remote character, and lets one idle through once when the game performs
// it later
extern thread_local bool g_forceAnimation;
void AllowRemoteIdle(uint32_t aActorId, uint32_t aIdleId) noexcept;

static TiltedPhoques::Initializer s_parkourHooks(
    []()
    {
        POINTER_FALLOUT4(TJumpButtonEvent, s_jumpButtonEvent, 882499);
        RealJumpButtonEvent = s_jumpButtonEvent.Get();
        TP_HOOK(&RealJumpButtonEvent, HookJumpButtonEvent);

        POINTER_FALLOUT4(TSneakButtonEvent, s_sneakButtonEvent, 399407);
        RealSneakButtonEvent = s_sneakButtonEvent.Get();
        TP_HOOK(&RealSneakButtonEvent, HookSneakButtonEvent);

        POINTER_FALLOUT4(TSetCollisionDisabled, s_setCollisionDisabled, 660096);
        RealSetCollisionDisabled = s_setCollisionDisabled.Get();
        TP_HOOK(&RealSetCollisionDisabled, HookSetCollisionDisabled);

        POINTER_FALLOUT4(TEnterCoverUpdate, s_enterCoverUpdate, 356880);
        RealEnterCoverUpdate = s_enterCoverUpdate.Get();
        TP_HOOK(&RealEnterCoverUpdate, HookEnterCoverUpdate);
    });

struct ParkourService::Ledge
{
    // the height of its top (world)
    float Top{0.f};
    // how far in front of the character its edge is
    float EdgeDistance{0.f};
    // the height where the character ends up standing on it: the top's own on a flat ledge, higher up a slanted one
    float LandingZ{0.f};
};

ParkourService::ParkourService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ParkourService::OnUpdate>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&ParkourService::OnNotifyMovementAction>(this);
    s_pParkourService = this;

    for (const char* pFile : kJumpSounds)
        m_jumpSounds.push_back(std::make_unique<FileSound>(kSoundTemplate, kSoundTemplateFiles, kSoundTemplateFolder, pFile));
    for (const char* pFile : kLandSounds)
        m_landSounds.push_back(std::make_unique<FileSound>(kSoundTemplate, kSoundTemplateFiles, kSoundTemplateFolder, pFile));
    for (int i = 1; i <= kTouchSoundCount; ++i)
        m_touchSounds.push_back(std::make_unique<FileSound>(kSoundTemplate, kSoundTemplateFiles, kSoundTemplateFolder, fmt::format("{}{:02}", kTouchSoundPrefix, i)));
}

ParkourService::~ParkourService() noexcept
{
    s_pParkourService = nullptr;
}

bool ParkourService::IsMoving(const void* apReference) noexcept
{
    return std::any_of(s_moving.begin(), s_moving.end(), [apReference](const std::atomic<const void*>& aSlot) { return aSlot.load(std::memory_order_relaxed) == apReference; });
}

bool ParkourService::HoldsPosition(const void* apReference, float aDistance) noexcept
{
    if (s_placing)
        return false;
    if (IsMoving(apReference))
        return true;
    if (aDistance <= kMaxSettleJump)
        return false;
    return std::any_of(s_settling.begin(), s_settling.end(),
                       [apReference](const Settling& acEntry)
                       {
                           return acEntry.Reference.load(std::memory_order_relaxed) == apReference &&
                                  std::chrono::steady_clock::now().time_since_epoch().count() < acEntry.Until.load(std::memory_order_relaxed);
                       });
}

std::optional<ParkourService::GrabSpot> ParkourService::FindGrabSpot(PlayerCharacter* apPlayer, const NiPoint3& acFeet, float aHeading) const noexcept
{
    if (!apPlayer || !apPlayer->parentCell)
        return std::nullopt;
    const auto ledge = FindLedgeFrom(apPlayer, acFeet, aHeading, true, false);
    if (!ledge)
        return std::nullopt;

    const float forwardX = std::sin(aHeading);
    const float forwardY = std::cos(aHeading);
    const float scale = apPlayer->GetScale();
    const Approach& approach = kApproach[static_cast<size_t>(Move::kGrabHigh)];
    const float back = ledge->EdgeDistance - approach.Back * scale;
    GrabSpot spot{};
    spot.Mark = NiPoint3(glm::vec3(acFeet.x + forwardX * ledge->EdgeDistance, acFeet.y + forwardY * ledge->EdgeDistance, ledge->Top));
    spot.Start = NiPoint3(glm::vec3(acFeet.x + forwardX * back, acFeet.y + forwardY * back, ledge->Top - approach.Rise * scale));
    return spot;
}

bool ParkourService::GrabFromAir(PlayerCharacter* apPlayer) noexcept
{
    if (!m_rules.Parkour || !apPlayer || !CanMove(apPlayer) || IsPlayerMoving())
        return false;
    if (const auto path = Plan(apPlayer, true, true))
    {
        Start(apPlayer, *path);
        m_holdUsed = true;
        return true;
    }
    return false;
}

bool ParkourService::IsPlayerMoving() const noexcept
{
    return std::any_of(m_playing.begin(), m_playing.end(), [](const Playing& acMove) { return acMove.Local; });
}

void ParkourService::HoldPlayer(PlayerCharacter* apPlayer) noexcept
{
    if (!apPlayer)
        return;
    if (!IsMoving(apPlayer))
        TakeMovingSlot(apPlayer);
    RealSetCollisionDisabled(apPlayer, true);
}

void ParkourService::PlacePlayer(PlayerCharacter* apPlayer, const NiPoint3& acPosition) noexcept
{
    if (!apPlayer)
        return;
    s_placing = true;
    apPlayer->SetPosition(acPosition, true);
    s_placing = false;
    // a fall counts from where the hold lets go, not from where it started
    if (bhkCharacterController* pController = apPlayer->GetCharController())
        pController->SetFallStartHeight(acPosition.z);
}

void ParkourService::ReleasePlayer(PlayerCharacter* apPlayer, const NiPoint3& acVelocity) noexcept
{
    if (!apPlayer)
        return;
    ReleaseMovingSlot(apPlayer);
    RealSetCollisionDisabled(apPlayer, false);
    if (bhkCharacterController* pController = apPlayer->GetCharController())
        pController->SetFallStartHeight(apPlayer->position.z);
    if (const auto pSetVelocity = GetAvfSetVelocity(); pSetVelocity && std::hypot(acVelocity.x, acVelocity.y, acVelocity.z) > 1.f)
        pSetVelocity({}, apPlayer, acVelocity.x, acVelocity.y, acVelocity.z, kHandBackTime, acVelocity.x, acVelocity.y, acVelocity.z, true);
}

bool ParkourService::OnJumpButton(bool aDown, bool aJustPressed) noexcept
{
    // RD VATS 76' first: with a critical strike there to use, the jump key readies it (Fallout 76's Space)
    if (Vats76Service::TakeJump(aDown, aJustPressed))
        return true;
    // the grapple's next: a press lets go of its cable (or pulls up onto a ledge in reach)
    if (GrappleService::TakeJump(aJustPressed))
        return true;

    m_jumpDown = aDown;
    if (!aJustPressed || !m_rules.Parkour)
        return false;

    m_holdUsed = false;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !CanMove(pPlayer) || std::any_of(m_playing.begin(), m_playing.end(), [](const Playing& acMove) { return acMove.Local; }))
        return false;

    const bhkCharacterController* pController = pPlayer->GetCharController();
    const bool airborne = pController->GetFallTime() >= kGrabAfterFall;
    if (const auto path = Plan(pPlayer, airborne, true))
    {
        Start(pPlayer, *path);
        m_holdUsed = true;
        return true;
    }
    return false;
}

bool ParkourService::RollsLanding() noexcept
{
    return std::chrono::steady_clock::now().time_since_epoch().count() < s_rollsLandingUntil.load(std::memory_order_relaxed);
}

bool ParkourService::OnSneakButton(bool aDown, bool aJustPressed) noexcept
{
    // the release of the press that got the roll ready doesn't sneak either
    if (!aDown)
    {
        const bool swallow = m_swallowSneakRelease;
        m_swallowSneakRelease = false;
        return swallow;
    }
    if (!aJustPressed || !m_rules.Parkour || !m_rules.LandingRoll || m_rollReady)
        return false;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !CanMove(pPlayer) || std::any_of(m_playing.begin(), m_playing.end(), [](const Playing& acMove) { return acMove.Local; }))
        return false;
    bhkCharacterController* pController = pPlayer->GetCharController();
    if (pController->GetFallTime() < kRollMinFall)
        return false;

    // The original's: the ground close under the feet, and enough AP (its stamina)
    const NiPoint3 feet = pPlayer->position;
    const RayHit ground = CastRay(pPlayer->parentCell, GetRayFilter(pPlayer), NiPoint3(glm::vec3(feet.x, feet.y, feet.z + 4.f)),
                                  NiPoint3(glm::vec3(feet.x, feet.y, feet.z - kRollReach * pPlayer->GetScale())));
    if (!ground.Hit)
    {
        spdlog::info("Fluid Movement: no landing roll, the ground is too far down yet");
        return false;
    }
    const float cost = m_rules.ApCost * pPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints) * 0.5f;
    if (pPlayer->GetActorValue(ActorValueInfo::kActionPoints) < cost)
    {
        spdlog::info("Fluid Movement: no landing roll, not enough AP");
        return false;
    }

    // as in the original, the roll softens the landing of a big fall
    const float fallStart = pController->GetFallStartHeight();
    if (fallStart - ground.Point.z > kRollEasesFallOver)
        pController->SetFallStartHeight(fallStart - kRollFallEase);

    m_rollReady = true;
    m_rollReadyTime = kRollReadyTime;
    m_swallowSneakRelease = true;
    m_rollHasPosition = false;
    GetGroundVelocity(pPlayer, m_rollVelocity[0], m_rollVelocity[1]);
    s_rollsLandingUntil = (std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(kRollReadyTime)) +
                           kRollHoldsLanding).time_since_epoch().count();
    spdlog::info("Fluid Movement: landing roll ready, {:.0f} over the ground after a fall of {:.0f}", feet.z - ground.Point.z, fallStart - ground.Point.z);
    return true;
}

void ParkourService::UpdateRoll(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    if (!m_rollReady || !apPlayer)
        return;
    const bhkCharacterController* pController = apPlayer->GetCharController();
    if (!pController)
    {
        m_rollReady = false;
        return;
    }

    // The horizontal velocity coming down, from the positions while falling (the landing takes it away)
    const NiPoint3 position = apPlayer->position;
    const bool falling = pController->GetFallTime() > 0.f;
    if (falling && m_rollHasPosition && aDelta > 0.f)
    {
        m_rollVelocity[0] = (position.x - m_rollLastPosition[0]) / aDelta;
        m_rollVelocity[1] = (position.y - m_rollLastPosition[1]) / aDelta;
    }
    m_rollLastPosition[0] = position.x;
    m_rollLastPosition[1] = position.y;
    m_rollLastPosition[2] = position.z;
    m_rollHasPosition = true;

    if (falling)
    {
        if ((m_rollReadyTime -= aDelta) <= 0.f)
        {
            m_rollReady = false;
            spdlog::info("Fluid Movement: the landing roll wasn't landed");
        }
        return;
    }

    m_rollReady = false;
    if (!CanMove(apPlayer) || std::any_of(m_playing.begin(), m_playing.end(), [](const Playing& acMove) { return acMove.Local; }))
        return;
    s_rollsLandingUntil = (std::chrono::steady_clock::now() + kRollHoldsLanding).time_since_epoch().count();
    Start(apPlayer, PlanRoll(apPlayer));
}

ParkourService::Path ParkourService::PlanRoll(PlayerCharacter* apPlayer) const noexcept
{
    const NiPoint3 feet = apPlayer->position;
    const float speed = std::hypot(m_rollVelocity[0], m_rollVelocity[1]);
    const float heading = speed >= kRollMinTurnSpeed ? std::atan2(m_rollVelocity[0], m_rollVelocity[1]) : apPlayer->rotation.z;
    const float forwardX = std::sin(heading);
    const float forwardY = std::cos(heading);
    const float duration = ParkourMoves::kViews[static_cast<size_t>(Move::kLandRoll)][IsFirstPerson() ? 1 : 0].Duration;
    const float want = std::min(speed, kRollMaxSpeed) * kRollCarry * duration;

    // On along the ground: short of anything at knee height in front of the body, and only while the ground stays
    // about level with the landing (collision is off for the move)
    TESObjectCELL* pCell = apPlayer->parentCell;
    const uint32_t filter = GetRayFilter(apPlayer);
    const float scale = apPlayer->GetScale();
    float reached = 0.f;
    float endZ = feet.z;
    while (reached < want)
    {
        const float distance = std::min(reached + kRollCheckStep, want);
        const float x = feet.x + forwardX * distance;
        const float y = feet.y + forwardY * distance;
        const NiPoint3 from(glm::vec3(feet.x + forwardX * reached, feet.y + forwardY * reached, endZ + kRollKneeHeight * scale));
        const NiPoint3 front(glm::vec3(feet.x + forwardX * (distance + kBodyRadius * scale), feet.y + forwardY * (distance + kBodyRadius * scale), endZ + kRollKneeHeight * scale));
        if (CastRay(pCell, filter, from, front).Hit)
            break;
        const RayHit ground = CastRay(pCell, filter, NiPoint3(glm::vec3(x, y, endZ + kRollGroundAbove * scale)), NiPoint3(glm::vec3(x, y, feet.z - kRollGroundBelow * scale)));
        if (!ground.Hit || ground.NormalZ < kMinTopNormalZ || IsCharacterLayer(ground.Layer))
            break;
        reached = distance;
        endZ = ground.Point.z;
    }

    Path path{};
    path.Kind = Move::kLandRoll;
    path.Heading = heading;
    path.Start[0] = path.Ideal[0] = feet.x;
    path.Start[1] = path.Ideal[1] = feet.y;
    path.Start[2] = path.Ideal[2] = feet.z;
    path.End[0] = feet.x + forwardX * reached;
    path.End[1] = feet.y + forwardY * reached;
    path.End[2] = endZ;
    std::copy(std::begin(path.End), std::end(path.End), path.Mark);
    return path;
}

void ParkourService::UpdateMarker(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    // the grappling gun out: the marker is the grapple's (a ledge it would pull up onto)
    if (GrappleService::OwnsMarker())
    {
        m_marker.reset();
        m_markerTimer = 0.f;
        return;
    }

    // On the ground and free to move (not in a move): the original's indicators, shown while a jump would make a move
    const bhkCharacterController* pController = apPlayer ? apPlayer->GetCharController() : nullptr;
    const bool looking = pController && m_rules.Parkour && m_rules.Markers && pController->GetFallTime() < kGrabAfterFall &&
                         std::none_of(m_playing.begin(), m_playing.end(), [](const Playing& acMove) { return acMove.Local; }) && CanMove(apPlayer);
    if (!looking)
    {
        m_marker.reset();
        m_markerTimer = 0.f;
        HudMarker::Hide();
        return;
    }

    if ((m_markerTimer -= aDelta) <= 0.f)
    {
        m_markerTimer = kMarkerInterval;
        // the same planning as a jump's, without its side effect (which leg steps next)
        const bool lastStepLeft = m_lastStepLeft;
        m_marker = Plan(apPlayer, false, false);
        m_lastStepLeft = lastStepLeft;
    }
    if (!m_marker)
    {
        HudMarker::Hide();
        return;
    }

    HudMarker::Icon icon = HudMarker::Icon::kClimb;
    switch (m_marker->Kind)
    {
    case Move::kVault: icon = HudMarker::Icon::kVault; break;
    case Move::kStepLowLeft:
    case Move::kStepLowRight:
    case Move::kStepHighLeft:
    case Move::kStepHighRight: icon = HudMarker::Icon::kStep; break;
    case Move::kFail: icon = HudMarker::Icon::kNoAp; break;
    default: break;
    }
    HudMarker::Show(icon, NiPoint3(glm::vec3(m_marker->Mark[0], m_marker->Mark[1], m_marker->Mark[2])), aDelta);
}

void ParkourService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Parkour");
    const float delta = static_cast<float>(acEvent.Delta);
    UpdateMarker(PlayerCharacter::Get(), delta);
    UpdateRoll(PlayerCharacter::Get(), delta);

    for (auto it = m_playing.begin(); it != m_playing.end();)
    {
        if (Advance(*it, delta))
            ++it;
        else
            it = m_playing.erase(it);
    }

    UpdateNpcs();

    // Holding jump in the air grabs a ledge as soon as the hands can reach one, once per hold
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    m_retryTimer -= delta;
    if (!pPlayer || !m_rules.Parkour || !m_jumpDown || m_holdUsed || m_retryTimer > 0.f)
        return;
    if (std::any_of(m_playing.begin(), m_playing.end(), [](const Playing& acMove) { return acMove.Local; }) || !CanMove(pPlayer))
        return;

    const bhkCharacterController* pController = pPlayer->GetCharController();
    if (pController->GetFallTime() < kGrabAfterFall)
        return;

    m_retryTimer = 0.05f;
    if (const auto path = Plan(pPlayer, true, false))
    {
        Start(pPlayer, *path);
        m_holdUsed = true;
    }
}

bool ParkourService::CanMove(PlayerCharacter* apPlayer) noexcept
{
    const auto& state = apPlayer->actorState;
    if (apPlayer->IsDead() || state.IsBleedingOut() || apPlayer->IsInPowerArmor() || !apPlayer->parentCell)
        return false;
    // on the grappling gun's cable: its winch has the player (it hands over to a grab itself); in an emote, whose
    // movement keys get the player up (EmoteService)
    if (GrappleService::HoldsPlayer() || EmoteService::IsPlayerEmoting())
        return false;

    return IsBodyFree(apPlayer) && apPlayer->GetCharController() != nullptr;
}

std::optional<ParkourService::Path> ParkourService::Plan(PlayerCharacter* apPlayer, bool aAirborne, bool aExplain) noexcept
{
    const float scale = apPlayer->GetScale();
    const NiPoint3 feet = apPlayer->position;
    const float heading = apPlayer->rotation.z;
    const float forwardX = std::sin(heading);
    const float forwardY = std::cos(heading);

    float speed = 0.f;
    float forwardSpeed = 0.f;
    GetGroundSpeed(apPlayer, speed, forwardSpeed);
    const float running = std::clamp(speed / kRunSpeed, 0.f, 1.f);

    if (!aAirborne)
    {
        if (auto vault = PlanVault(apPlayer, heading, false, aExplain))
            return vault;
    }
    else if (const bhkCharacterController* pController = apPlayer->GetCharController(); pController->GetFallStartHeight() - feet.z > kMaxGrabFall)
    {
        if (aExplain)
            spdlog::info("Fluid Movement: no grab, falling too fast");
        return std::nullopt;
    }

    const auto ledge = FindLedge(apPlayer, aAirborne, aExplain);
    if (!ledge)
        return std::nullopt;

    const float rise = (ledge->Top - feet.z) / scale;
    const auto refuse = [&](const char* acpWhy) -> std::optional<Path>
    {
        if (aExplain)
            spdlog::info("Fluid Movement: no move, a ledge {:.0f} up and {:.0f} ahead but {}", rise, ledge->EdgeDistance, acpWhy);
        return std::nullopt;
    };

    Move kind = Move::kCount;
    if (aAirborne)
    {
        if (rise > kGrabMax)
            return refuse("it's out of reach from the air");
        if (ledge->EdgeDistance > kStepReach * scale)
            return refuse("it's too far ahead to grab");
        TESObjectCELL* pCell = apPlayer->parentCell;
        if (CastRay(pCell, GetRayFilter(apPlayer), feet, NiPoint3(glm::vec3(feet.x, feet.y, feet.z - kGrabClearBelow * scale))).Hit)
            return refuse("the feet are nearly on the ground");
        kind = rise > kGrabHighFrom ? Move::kGrabHigh : Move::kGrab;
    }
    else if (rise >= kLowFrom)
    {
        kind = ClimbFor(rise);

        if (kind == Move::kClimbHigh || kind == Move::kClimbHighest)
        {
            // Running along with nothing at the face isn't a climb (the original's smart climb)
            const float faceZ = feet.z + kRuleHeight * scale;
            const NiPoint3 face(glm::vec3(feet.x, feet.y, faceZ));
            const NiPoint3 ahead(glm::vec3(feet.x + forwardX * kSmartClimbLook, feet.y + forwardY * kSmartClimbLook, faceZ));
            if (running > kSmartClimbShare && !CastRay(apPlayer->parentCell, GetRayFilter(apPlayer), face, ahead).Hit)
                return refuse("it's run past, nothing at the face");

            // Not enough AP: the climb fails (the original's out of stamina)
            const float cost = m_rules.ApCost * apPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints);
            if (apPlayer->GetActorValue(ActorValueInfo::kActionPoints) < cost)
                kind = Move::kFail;
        }
    }
    else
    {
        if (forwardSpeed < -10.f)
            return refuse("moving backwards");
        if (rise < kStepMin * (1.f + running))
            return refuse("it's low enough to walk onto");
        if (ledge->EdgeDistance > kStepReach * scale)
            return refuse("it's too far ahead to step onto");
        m_lastStepLeft = !m_lastStepLeft;
        kind = rise >= kStepHighFrom ? (m_lastStepLeft ? Move::kStepHighLeft : Move::kStepHighRight) : (m_lastStepLeft ? Move::kStepLowLeft : Move::kStepLowRight);
    }

    return PathOntoLedge(kind, feet, heading, scale, *ledge);
}

ParkourService::Path ParkourService::PathOntoLedge(Move aKind, const NiPoint3& acFeet, float aHeading, float aScale, const Ledge& acLedge) noexcept
{
    const float forwardX = std::sin(aHeading);
    const float forwardY = std::cos(aHeading);

    Path path{};
    path.Kind = aKind;
    path.Heading = aHeading;
    path.Start[0] = acFeet.x;
    path.Start[1] = acFeet.y;
    path.Start[2] = acFeet.z;
    path.Mark[0] = acFeet.x + forwardX * acLedge.EdgeDistance;
    path.Mark[1] = acFeet.y + forwardY * acLedge.EdgeDistance;
    path.Mark[2] = acLedge.Top;
    if (aKind == Move::kFail)
    {
        std::copy(std::begin(path.Start), std::end(path.Start), path.Ideal);
        std::copy(std::begin(path.Start), std::end(path.Start), path.End);
        return path;
    }

    // Into the animation's start (from the point just over the edge), never eased far backwards; out onto the ledge's
    // standing spot at its real height
    const Approach& approach = kApproach[static_cast<size_t>(aKind)];
    const float back = std::max(acLedge.EdgeDistance - approach.Back * aScale, -kMaxBackEase);
    path.Ideal[0] = acFeet.x + forwardX * back;
    path.Ideal[1] = acFeet.y + forwardY * back;
    path.Ideal[2] = acLedge.Top - approach.Rise * aScale;
    const float along = acLedge.EdgeDistance + kLandingDepth;
    path.End[0] = acFeet.x + forwardX * along;
    path.End[1] = acFeet.y + forwardY * along;
    path.End[2] = acLedge.LandingZ + 1.f;
    return path;
}

std::optional<ParkourService::Path> ParkourService::PlanVault(Actor* apActor, float aHeading, bool aStanding, bool aExplain) const noexcept
{
    float speed = 0.f;
    float forwardSpeed = 0.f;
    GetGroundSpeed(apActor, speed, forwardSpeed);
    if (!aStanding && forwardSpeed < kMinVaultSpeed)
        return std::nullopt;

    TESObjectCELL* pCell = apActor->parentCell;
    const uint32_t filter = GetRayFilter(apActor);
    const float scale = apActor->GetScale();
    const NiPoint3 feet = apActor->position;
    const float forwardX = std::sin(aHeading);
    const float forwardY = std::cos(aHeading);
    const auto ahead = [&](float aDistance, float aZ) { return NiPoint3(glm::vec3(feet.x + forwardX * aDistance, feet.y + forwardY * aDistance, aZ)); };
    const float head = feet.z + kRuleHeight * scale;
    const float running = std::clamp(speed / kRunSpeed, 0.f, 1.f);
    const float reach = (75.f + 25.f * running) * scale;

    // The obstacle: the first top in reach between waist and head height (higher is a wall: the climbs' business)
    float nearDistance = -1.f;
    float top = 0.f;
    for (float distance = kFirstSample; distance <= reach; distance += kSampleStep)
    {
        const RayHit down = CastRay(pCell, filter, ahead(distance, head), ahead(distance, feet.z - 10.f));
        if (!down.Hit)
            continue;
        const float height = down.Point.z - feet.z;
        if (height > kVaultMax * scale)
            return std::nullopt;
        if (height <= kVaultMin * scale || down.NormalZ < kMinTopNormalZ)
            continue;
        if (IsCharacterLayer(down.Layer))
            return std::nullopt;
        nearDistance = distance;
        top = down.Point.z;
        break;
    }
    if (nearDistance < 0.f)
        return std::nullopt;

    const auto refuse = [&](const char* acpWhy) -> std::optional<Path>
    {
        if (aExplain)
            spdlog::info("Fluid Movement: no vault, something {:.0f} high {:.0f} ahead but {}", top - feet.z, nearDistance, acpWhy);
        return std::nullopt;
    };

    // Room over it, nothing in the way at its height, and it's a rail or a low wall rather than a platform: past it
    // the ground is lower
    if (CastRay(pCell, filter, ahead(nearDistance, top + 5.f), ahead(nearDistance, top + 60.f * scale)).Hit)
        return refuse("there's no room over it");
    if (CastRay(pCell, filter, ahead(0.f, top + 10.f), ahead(nearDistance + kVaultClears, top + 10.f)).Hit)
        return refuse("something's in the way over it");
    const RayHit beyond = CastRay(pCell, filter, ahead(nearDistance + kVaultClears, head), ahead(nearDistance + kVaultClears, feet.z + kVaultMin * scale));
    if (beyond.Hit && beyond.Point.z >= top - 10.f)
        return refuse("it goes on past it (a platform to climb, not to vault)");

    // Ground to land on, with room to stand
    const float landAt = nearDistance + kVaultOver;
    const RayHit ground = CastRay(pCell, filter, ahead(landAt, top + 10.f), ahead(landAt, feet.z - kMaxDropBehind));
    if (!ground.Hit || ground.Point.z > top - 10.f || ground.NormalZ < kMinTopNormalZ)
        return refuse("there's nowhere to land past it");
    if (CastRay(pCell, filter, ahead(landAt, ground.Point.z + 2.f), ahead(landAt, ground.Point.z + kPlayerHeight * scale)).Hit)
        return refuse("there's no room to stand past it");

    Path path{};
    path.Kind = Move::kVault;
    path.Heading = aHeading;
    path.Start[0] = feet.x;
    path.Start[1] = feet.y;
    path.Start[2] = feet.z;
    // running into it: never eased backwards, it starts where the character is
    const float back = std::max(nearDistance - kApproach[static_cast<size_t>(Move::kVault)].Back * scale, 0.f);
    path.Ideal[0] = feet.x + forwardX * back;
    path.Ideal[1] = feet.y + forwardY * back;
    path.Ideal[2] = feet.z;
    path.End[0] = feet.x + forwardX * landAt;
    path.End[1] = feet.y + forwardY * landAt;
    path.End[2] = ground.Point.z + 1.f;
    path.Bump = std::max(0.f, top - feet.z - kVaultClears * scale);
    path.Mark[0] = feet.x + forwardX * nearDistance;
    path.Mark[1] = feet.y + forwardY * nearDistance;
    path.Mark[2] = top;
    return path;
}

std::optional<ParkourService::Ledge> ParkourService::FindLedge(PlayerCharacter* apPlayer, bool aAirborne, bool aExplain) const noexcept
{
    return FindLedgeFrom(apPlayer, apPlayer->position, apPlayer->rotation.z, aAirborne, aExplain);
}

std::optional<ParkourService::Ledge> ParkourService::FindLedgeFrom(Actor* apActor, const NiPoint3& acFeet, float aHeading, bool aAirborne,
                                                                   bool aExplain) const noexcept
{
    TESObjectCELL* pCell = apActor->parentCell;
    const uint32_t filter = GetRayFilter(apActor);

    const float scale = apActor->GetScale();
    const float height = kPlayerHeight * scale;
    const float maxLedge = (aAirborne ? kGrabMax : kClimbMax) * scale;
    const float minLedge = (aAirborne ? 10.f : kClimbMin) * scale;
    float speed = 0.f;
    float forwardSpeed = 0.f;
    GetGroundSpeed(apActor, speed, forwardSpeed);
    const float reach = (aAirborne ? kStepReach : 75.f + 35.f * std::clamp(speed / kRunSpeed, 0.f, 1.f)) * scale;

    const NiPoint3 feet = acFeet;
    const float directionX = std::sin(aHeading);
    const float directionY = std::cos(aHeading);
    const auto ahead = [&](float aDistance, float aZ) { return NiPoint3(glm::vec3(feet.x + directionX * aDistance, feet.y + directionY * aDistance, aZ)); };

    // How far ahead it's open just above the highest ledge in reach: past that is a wall too tall to climb
    const float probeTop = feet.z + maxLedge + 4.f;
    const RayHit wall = CastRay(pCell, filter, ahead(0.f, probeTop), ahead(reach, probeTop));
    const float open = wall.Hit ? std::hypot(wall.Point.x - feet.x, wall.Point.y - feet.y) : reach;

    // Straight down onto what's ahead, nearest first, until the top of a step in reach turns up
    Ledge ledge{};
    bool found = false;
    bool lastHit = false;
    float lastRise = 0.f;
    for (float distance = kFirstSample; distance <= open; distance += kSampleStep)
    {
        const RayHit down = CastRay(pCell, filter, ahead(distance, probeTop), ahead(distance, feet.z - kBelowFeet));
        if (!down.Hit)
        {
            lastHit = false;
            continue;
        }

        const float rise = down.Point.z - feet.z;
        const bool step = !lastHit || rise - lastRise >= std::max(kMinStep, rise * 0.5f);
        lastHit = true;
        lastRise = rise;
        if (rise < minLedge || !step)
            continue;

        if (IsCharacterLayer(down.Layer))
        {
            if (aExplain)
                spdlog::info("Fluid Movement: no move, the top {:.0f} up and {:.0f} ahead is someone", rise, distance);
            return std::nullopt;
        }

        // The edge or the face itself: the flat top, if there is one, is a little further in
        if (down.NormalZ < kMinTopNormalZ)
        {
            lastHit = false;
            continue;
        }

        ledge.Top = down.Point.z;
        ledge.EdgeDistance = distance;
        found = true;
        break;
    }

    if (!found)
        return std::nullopt;

    const auto refuse = [&](const char* acpWhy) -> std::optional<Ledge>
    {
        if (aExplain)
            spdlog::info("Fluid Movement: no move, a ledge {:.0f} up and {:.0f} ahead but {}", ledge.Top - feet.z, ledge.EdgeDistance, acpWhy);
        return std::nullopt;
    };

    // Where the feet end up past the edge: on the top (higher up a slanted one), not a drop and not the foot of a wall
    const NiPoint3 landing = ahead(ledge.EdgeDistance + kLandingDepth, ledge.Top);
    const RayHit below = CastRay(pCell, filter, NiPoint3(glm::vec3(feet.x, feet.y, feet.z + 4.f)), NiPoint3(glm::vec3(feet.x, feet.y, feet.z - kGroundSearch)));
    const float ground = below.Hit ? below.Point.z : feet.z;
    const RayHit stand = CastRay(pCell, filter, NiPoint3(glm::vec3(landing.x, landing.y, ledge.Top + kMaxLandingRise + 4.f)),
                                 NiPoint3(glm::vec3(landing.x, landing.y, ground - kMaxDropBehind)));
    if (!stand.Hit)
        return refuse("there's a drop behind it");
    if (stand.Point.z > ledge.Top + kMaxLandingRise)
        return refuse("it's the foot of a wall");
    if (stand.Point.z > ledge.Top - kClearance && (stand.NormalZ < kMinTopNormalZ || IsCharacterLayer(stand.Layer)))
        return refuse(IsCharacterLayer(stand.Layer) ? "someone's standing on it" : "it's too steep to stand on past the edge");
    ledge.LandingZ = std::max(stand.Point.z, ledge.Top);

    // Room to stand up there, to rise on the way, and nothing on top between here and a body's width past the spot
    if (CastRay(pCell, filter, NiPoint3(glm::vec3(landing.x, landing.y, ledge.LandingZ + 2.f)), NiPoint3(glm::vec3(landing.x, landing.y, ledge.LandingZ + height))).Hit)
        return refuse("there's no room to stand on it");
    if (CastRay(pCell, filter, NiPoint3(glm::vec3(feet.x, feet.y, feet.z + height * 0.9f)), NiPoint3(glm::vec3(feet.x, feet.y, ledge.Top + height))).Hit)
        return refuse("there's no room to rise");
    const float beyond = ledge.EdgeDistance + kLandingDepth + kBodyRadius;
    const float beyondZ = ledge.Top + (ledge.LandingZ - ledge.Top) * (kLandingDepth + kBodyRadius) / kLandingDepth;
    for (const float above : {kClearance + 8.f, height * 0.6f})
    {
        if (CastRay(pCell, filter, ahead(0.f, ledge.Top + above), ahead(ledge.EdgeDistance, ledge.Top + above)).Hit ||
            CastRay(pCell, filter, ahead(ledge.EdgeDistance, ledge.Top + above), ahead(beyond, beyondZ + above)).Hit)
            return refuse("something's in the way on top");
    }

    return ledge;
}

void ParkourService::Start(PlayerCharacter* apPlayer, const Path& acPath) noexcept
{
    const size_t index = static_cast<size_t>(acPath.Kind);
    const int view = IsFirstPerson() ? 1 : 0;

    // AP: a full price climb (as high as the head or higher) or half (the original's low effort), none for a fail
    const bool bigClimb = acPath.Kind == Move::kClimbHigh || acPath.Kind == Move::kClimbHighest;
    if (acPath.Kind != Move::kFail)
    {
        const float cost = m_rules.ApCost * apPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints) * (bigClimb ? 1.f : 0.5f);
        const float actionPoints = apPlayer->GetActorValue(ActorValueInfo::kActionPoints);
        apPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kActionPoints, std::max(0.f, actionPoints - cost));
    }

    Playing move{};
    move.Way = acPath;
    move.ActorId = apPlayer->formID;
    move.Reference = apPlayer;
    move.Local = true;
    move.View = view;

    // Collision off for the move (the hook keeps it off), on the way from this frame
    TakeMovingSlot(apPlayer);
    RealSetCollisionDisabled(apPlayer, true);
    // a fall doesn't carry on through a move: what counts after it is from where it ends
    if (bhkCharacterController* pController = apPlayer->GetCharController())
        pController->SetFallStartHeight(apPlayer->position.z);

    // The weapon out of the hands for the move, in both views (the third person event only hides it in third person,
    // and a weapon held through a first person move breaks it)
    move.WeaponsCulled = true;
    apPlayer->CullWeapons(true);

    TESIdleForm* pIdle = GetIdle(acPath.Kind, view);
    const bool animated = pIdle && apPlayer->currentProcess && apPlayer->currentProcess->PlayIdle(apPlayer, pIdle, nullptr, false);
    if (view == 1 && IsBodyShownInFirstPerson())
    {
        bool bodyAnimated = false;
        if (TESIdleForm* pBodyIdle = GetIdle(acPath.Kind, 0); pBodyIdle && apPlayer->currentProcess)
            OnBodyGraph(apPlayer, [&] { bodyAnimated = apPlayer->currentProcess->PlayIdle(apPlayer, pBodyIdle, nullptr, false); });
        spdlog::info("Fluid Movement: the body Full Body First Person shows {}", bodyAnimated ? "plays the move too" : "didn't take the move");
    }
    m_playing.push_back(move);
    // a landing roll starts on the landing itself
    if (acPath.Kind == Move::kLandRoll)
        PlayRandom(m_landSounds, apPlayer);
    Share(acPath);

    const float rise = acPath.End[2] - acPath.Start[2];
    const float along = std::hypot(acPath.End[0] - acPath.Start[0], acPath.End[1] - acPath.Start[1]);
    spdlog::info("Fluid Movement: {} ({} person{}), {:.0f} up and {:.0f} along, {:.2f} s, eased {:.0f} into it", ParkourMoves::kNames[index],
                 view ? "first" : "third", animated ? "" : ", the animation didn't play", rise, along, ParkourMoves::kViews[index][view].Duration,
                 std::hypot(acPath.Ideal[0] - acPath.Start[0], acPath.Ideal[1] - acPath.Start[1]));
}

namespace
{
// A clip's per frame table (N values a frame) at a time
template <size_t N> void SampleFrames(const ParkourMoves::View& acClip, const float (*apTable)[N], float aTime, float (&aOut)[N]) noexcept
{
    const float frame = std::clamp(aTime / ParkourMoves::kFrameTime, 0.f, static_cast<float>(acClip.FrameCount - 1));
    const int a = static_cast<int>(frame);
    const int b = std::min(a + 1, acClip.FrameCount - 1);
    const float u = frame - static_cast<float>(a);
    for (size_t i = 0; i < N; ++i)
        aOut[i] = apTable[a][i] + (apTable[b][i] - apTable[a][i]) * u;
}

// When a move's body is up: the first frame from which its travel stays near where it ends (kExitShare, kExitNear)
float ExitTime(const ParkourMoves::View& acClip) noexcept
{
    const auto& last = acClip.Curve[acClip.FrameCount - 1];
    const float closeEnough = std::max(kExitNear, kExitShare * std::hypot(last[0], last[1], last[2]));
    int frame = acClip.FrameCount - 1;
    while (frame > 0)
    {
        const auto& at = acClip.Curve[frame - 1];
        if (std::hypot(at[0] - last[0], at[1] - last[1], at[2] - last[2]) > closeEnough)
            break;
        --frame;
    }
    return std::min(static_cast<float>(frame) * ParkourMoves::kFrameTime, acClip.Duration);
}

// The moves a held direction can leave once the body is up: not a failed climb (it falls back) nor the landing roll
// (it carries its own momentum)
bool CanLeaveEarly(ParkourMoves::Move aMove) noexcept
{
    return aMove != ParkourMoves::Move::kFail && aMove != ParkourMoves::Move::kLandRoll;
}

// Whether the player is pushing a direction, on keys or a stick (PlayerControls, CommonLibF4 id 544871: its data at
// 0x44 starts with the movement input, x and y from -1 to 1)
bool IsMoveInputHeld() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerControls, 544872);
    const uint8_t* pControls = *s_playerControls.Get();
    if (!pControls)
        return false;
    const float x = *reinterpret_cast<const float*>(pControls + 0x44);
    const float y = *reinterpret_cast<const float*>(pControls + 0x48);
    return std::isfinite(x) && std::isfinite(y) && std::hypot(x, y) > 0.3f;
}

// Out of a move's idle into the graph's normal state, blended as the game ends idles; also on the body Full Body
// First Person shows in first person, which plays the move too. Through the game's action where the graph takes it
// (third person), so the others' copy of the character ends its idle as well; the first person graph refuses the
// action and takes its event instead (seen in game).
void StopIdle(Actor* apActor, bool aBodyToo) noexcept
{
    auto* pAction = Cast<BGSAction>(TESForm::GetById(kActionIdleStop));

    const auto stop = [apActor, pAction]
    {
        if (pAction)
        {
            TESActionData actionData(2, apActor, pAction, nullptr);
            if (ActorMediator::Get()->PerformAction(&actionData))
                return;
        }
        BSFixedString idleStop("IdleStop");
        apActor->animationGraphHolder.SendAnimationEvent(&idleStop);
        idleStop.Release();
    };
    stop();
    if (aBodyToo)
        OnBodyGraph(apActor, stop);
}

// How much of the way into a move's start is left at a time (1 -> 0), and how much of the way onto its end is done
// (0 -> 1): eased over kEaseIn (or kEaseShare of a short move) and the move's last kEaseShare
void Eases(const ParkourMoves::View& acClip, float aTime, float& aInto, float& aOnto) noexcept
{
    const float easeIn = std::min(kEaseIn, kEaseShare * acClip.Duration);
    const float out = kEaseShare * acClip.Duration;
    aInto = 1.f - Smoothstep(aTime / std::max(easeIn, 1e-3f));
    aOnto = Smoothstep((aTime - (acClip.Duration - out)) / std::max(out, 1e-3f));
}

// A move's own frame (x right, y forward, z up) in the world, facing aHeading
void ToWorld(float aHeading, const float acLocal[3], float aWorld[3]) noexcept
{
    const float forwardX = std::sin(aHeading);
    const float forwardY = std::cos(aHeading);
    aWorld[0] = forwardY * acLocal[0] + forwardX * acLocal[1];
    aWorld[1] = -forwardX * acLocal[0] + forwardY * acLocal[1];
    aWorld[2] = acLocal[2];
}
} // namespace

void ParkourService::PointAt(const Path& acPath, int aView, float aTime, float aOut[3]) noexcept
{
    const auto& clip = ParkourMoves::kViews[static_cast<size_t>(acPath.Kind)][aView];
    const float time = std::clamp(aTime, 0.f, clip.Duration);

    // The landing roll's animation rolls in place: the momentum it landed with carries it on, at an even pace
    if (acPath.Kind == Move::kLandRoll)
    {
        const float share = time / std::max(clip.Duration, 1e-3f);
        for (int axis = 0; axis < 3; ++axis)
            aOut[axis] = acPath.Start[axis] + (acPath.End[axis] - acPath.Start[axis]) * share;
        return;
    }

    // the clip's travel at this time, and at its end, in the world
    float travel[3];
    float now[3];
    float last[3];
    SampleFrames(clip, clip.Curve, time, travel);
    ToWorld(acPath.Heading, travel, now);
    ToWorld(acPath.Heading, clip.Curve[clip.FrameCount - 1], last);

    // From where the character was into the animation's start, then the animation's travel, and onto the real ledge
    float into;
    float onto;
    Eases(clip, time, into, onto);
    const float bump = acPath.Bump * std::sin(kPi * time / std::max(clip.Duration, 1e-3f));
    for (int axis = 0; axis < 3; ++axis)
    {
        const float fromIdeal = acPath.Ideal[axis] + now[axis];
        const float endMiss = acPath.End[axis] - (acPath.Ideal[axis] + last[axis]);
        aOut[axis] = fromIdeal + (acPath.Start[axis] - acPath.Ideal[axis]) * into + endMiss * onto;
    }
    aOut[2] += bump;
}

void ParkourService::EyeAt(const Path& acPath, int aView, float aTime, float aOut[5]) noexcept
{
    std::fill(aOut, aOut + 5, 0.f);
    const auto& clip = ParkourMoves::kViews[static_cast<size_t>(acPath.Kind)][aView];
    if (!clip.Eye)
        return;
    const float time = std::clamp(aTime, 0.f, clip.Duration);

    // the clip camera's motion, less where it starts (eased away like the way into the move) and less where it ends
    // (eased in like the way onto the ledge): from the view at rest, back to it
    float first[5];
    float now[5];
    float last[5];
    SampleFrames(clip, clip.Eye, 0.f, first);
    SampleFrames(clip, clip.Eye, time, now);
    SampleFrames(clip, clip.Eye, clip.Duration, last);
    float into;
    float onto;
    Eases(clip, time, into, onto);
    float eye[5];
    for (int i = 0; i < 5; ++i)
        eye[i] = now[i] - first[i] * into - last[i] * onto;
    ToWorld(acPath.Heading, eye, aOut);
    aOut[3] = eye[3];
    aOut[4] = eye[4];
}

bool ParkourService::Advance(Playing& aMove, float aDelta) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aMove.ActorId));
    if (!pActor || pActor->IsDead() || !pActor->parentCell)
    {
        Finish(aMove, "the character went away or died");
        return false;
    }

    const auto& clip = ParkourMoves::kViews[static_cast<size_t>(aMove.Way.Kind)][aMove.View];
    // Where the character is put: on the way, and in first person where the clip's camera takes the view (the
    // character itself isn't seen, the view is placed by it)
    const auto placeAt = [&](float aTime, float aPoint[3], float aEye[5])
    {
        PointAt(aMove.Way, aMove.View, aTime, aPoint);
        EyeAt(aMove.Way, aMove.View, aTime, aEye);
        for (int axis = 0; axis < 3; ++axis)
            aPoint[axis] += aEye[axis];
    };

    // far from the way: a load or a teleport took the character
    float expected[3];
    float eye[5];
    placeAt(aMove.Time, expected, eye);
    if (std::hypot(pActor->position.x - expected[0], pActor->position.y - expected[1], pActor->position.z - expected[2]) > kMaxOffWay)
    {
        Finish(aMove, "the character was moved away");
        return false;
    }

    // An NPC knocked over or down in the middle of a move: the game has it from there
    if (aMove.Owned && (!IsBodyFree(pActor) || pActor->actorState.IsBleedingOut()))
    {
        Finish(aMove, "the character was knocked out of it");
        return false;
    }

    // A player going on once the body is up: the rest of the move at kHurry times its pace, then out into walking or
    // running. It still ends on the ledge: left right there instead, the body was short of it and fell back down.
    if (aMove.Local && !aMove.Hurried && CanLeaveEarly(aMove.Way.Kind) && aMove.Time >= ExitTime(clip) && IsMoveInputHeld())
        aMove.Hurried = true;
    // an NPC's goes by the game's time as its animation does: in VATS they crawl along together
    aMove.Time += aMove.Owned ? GameFrameTime() : aMove.Hurried ? aDelta * kHurry : aDelta;
    float point[3];
    placeAt(aMove.Time, point, eye);
    const NiPoint3 position(glm::vec3(point[0], point[1], point[2]));
    if (aMove.Local)
    {
        s_placing = true;
        pActor->SetPosition(position, true);
        s_placing = false;
        // first person: the view turns as the clip's camera does, on top of the player's own turning
        pActor->rotation.x += eye[3] - aMove.EyePitch;
        pActor->rotation.z += eye[4] - aMove.EyeYaw;
        aMove.EyePitch = eye[3];
        aMove.EyeYaw = eye[4];
        // the view can turn a little more, the move keeps its heading
        const float facing = aMove.Way.Heading + aMove.EyeYaw;
        const float turn = std::remainder(pActor->rotation.z - facing, 2.f * kPi);
        if (std::abs(turn) > kYawFreedom)
            pActor->rotation.z = facing + std::copysign(kYawFreedom, turn);
    }
    else
    {
        s_placing = true;
        pActor->ForcePosition(position);
        s_placing = false;
        // an NPC can start a move facing elsewhere: turned to it quickly, not at once
        const float turn = std::remainder(aMove.StartYaw - aMove.Way.Heading, 2.f * kPi) * (1.f - Smoothstep(aMove.Time / kTurnInTime));
        pActor->SetRotation(0.f, 0.f, aMove.Way.Heading + turn);
    }

    SetFootIk(pActor, false);
    if (aMove.Local && aMove.View == 1 && IsBodyShownInFirstPerson())
        OnBodyGraph(pActor, [pActor] { SetFootIk(pActor, false); });
    PlayCues(aMove, pActor);

    if (aMove.Time < clip.Duration)
    {
        // Another game's copy is handed back to the movement sync once the body is up: its owner may be hurrying
        // through the rest, and the sync has it where the owner is either way
        if (!aMove.Local && !aMove.Owned && CanLeaveEarly(aMove.Way.Kind) && aMove.Time >= ExitTime(clip))
        {
            Finish(aMove, "handed back");
            return false;
        }
        return true;
    }

    // Hurried through: the idle still has a moment to play, and the player going on doesn't wait for it
    if (aMove.Hurried)
        StopIdle(pActor, aMove.View == 1 && IsBodyShownInFirstPerson());
    Finish(aMove, aMove.Hurried ? "done, hurried to go on" : "done");
    return false;
}

void ParkourService::Finish(Playing& aMove, const char* acpReason) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aMove.ActorId));
    ReleaseMovingSlot(aMove.Reference);
    if (!pActor)
        return;

    RealSetCollisionDisabled(pActor, false);
    SetFootIk(pActor, true);
    if (aMove.WeaponsCulled)
    {
        pActor->CullWeapons(false);
        aMove.WeaponsCulled = false;
    }
    if (aMove.Local && aMove.View == 1 && IsBodyShownInFirstPerson())
        OnBodyGraph(pActor, [pActor] { SetFootIk(pActor, true); });

    if (aMove.Owned)
    {
        // its AI has it again, from where the move ended, with no fall to count
        MarkSettling(pActor);
        if (bhkCharacterController* pController = pActor->GetCharController())
            pController->SetFallStartHeight(pActor->position.z);
        spdlog::info("Fluid Movement: NPC {:X} {} {} after {:.2f} s", pActor->formID, ParkourMoves::kNames[static_cast<size_t>(aMove.Way.Kind)], acpReason, aMove.Time);
        return;
    }

    if (!aMove.Local)
    {
        // the movement sync places the character again
        auto view = m_world.view<FormIdComponent, InterpolationComponent>();
        for (const auto entity : view)
        {
            if (view.get<FormIdComponent>(entity).Id == aMove.ActorId)
                view.get<InterpolationComponent>(entity).Scripted = false;
        }
        return;
    }

    auto* pPlayer = static_cast<PlayerCharacter*>(pActor);
    // what's left of the move's turning of the view (nothing, unless it was cut short)
    pPlayer->rotation.x -= aMove.EyePitch;
    pPlayer->rotation.z -= aMove.EyeYaw;
    aMove.EyePitch = 0.f;
    aMove.EyeYaw = 0.f;
    MarkSettling(pPlayer);
    if (bhkCharacterController* pController = pPlayer->GetCharController())
        pController->SetFallStartHeight(pPlayer->position.z);

    // Out of a move still going (a vault, a step) at its own speed, where it ended or was left: the run carries on
    // rather than starting again
    const auto& clip = ParkourMoves::kViews[static_cast<size_t>(aMove.Way.Kind)][aMove.View];
    const float at = std::min(aMove.Time, clip.Duration);
    float last[3];
    float before[3];
    PointAt(aMove.Way, aMove.View, at, last);
    PointAt(aMove.Way, aMove.View, at - ParkourMoves::kFrameTime, before);
    const float x = (last[0] - before[0]) / ParkourMoves::kFrameTime;
    const float y = (last[1] - before[1]) / ParkourMoves::kFrameTime;
    if (std::hypot(x, y) >= kMinHandBackSpeed && aMove.Time >= ExitTime(clip))
    {
        if (const auto pSetVelocity = GetAvfSetVelocity())
            pSetVelocity({}, pPlayer, x, y, 0.f, kHandBackTime, x, y, 0.f, true);
    }

    spdlog::info("Fluid Movement: {} {} after {:.2f} s, feet {:.0f} from where it was to end", ParkourMoves::kNames[static_cast<size_t>(aMove.Way.Kind)], acpReason,
                 aMove.Time, std::hypot(pPlayer->position.x - aMove.Way.End[0], pPlayer->position.y - aMove.Way.End[1], pPlayer->position.z - aMove.Way.End[2]));
}

TESIdleForm* ParkourService::GetIdle(Move aMove, int aView) noexcept
{
    TESIdleForm*& pIdle = m_idles[static_cast<size_t>(aMove)][aView];
    if (pIdle)
        return pIdle;

    // first person: the graph the vanilla first person landing idle plays on
    const char* pGraph = kThirdPersonGraph;
    if (aView == 1)
    {
        const auto* pVanilla = Cast<TESIdleForm>(TESForm::GetById(kIdle1stPJumpImpactLand));
        if (!pVanilla || !pVanilla->behaviorGraphName.AsAscii() || !*pVanilla->behaviorGraphName.AsAscii())
        {
            spdlog::warn("Fluid Movement: no first person graph to play the moves on");
            return nullptr;
        }
        pGraph = pVanilla->behaviorGraphName.AsAscii();
    }

    auto* pNew = IFormFactory::Create<TESIdleForm>();
    if (!pNew)
        return nullptr;

    pNew->behaviorGraphName.Set(pGraph);
    pNew->animEventName.Set(kIdleEvents[aView]);
    pNew->animFileName.Set(ParkourMoves::kViews[static_cast<size_t>(aMove)][aView].File);
    pNew->idleFlags = 1;
    spdlog::info("Fluid Movement: idle {:X} for {} ({} person, {})", pNew->formID, ParkourMoves::kNames[static_cast<size_t>(aMove)], aView ? "first" : "third", pGraph);
    pIdle = pNew;
    return pNew;
}

void ParkourService::PlayCues(Playing& aMove, Actor* apActor) noexcept
{
    const auto& clip = ParkourMoves::kViews[static_cast<size_t>(aMove.Way.Kind)][aMove.View];
    while (aMove.NextCue < clip.CueCount && clip.Cues[aMove.NextCue].Time >= 0.f && aMove.Time >= clip.Cues[aMove.NextCue].Time)
    {
        switch (clip.Cues[aMove.NextCue].Kind)
        {
        case ParkourMoves::Sound::kJump: PlayRandom(m_jumpSounds, apActor); break;
        case ParkourMoves::Sound::kLand: PlayRandom(m_landSounds, apActor); break;
        case ParkourMoves::Sound::kTouch: PlayRandom(m_touchSounds, apActor); break;
        }
        ++aMove.NextCue;
    }
}

void ParkourService::Share(const Path& acPath, const Actor* apNpc) noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;

    RequestMovementAction request{};
    request.Type = RequestMovementAction::kParkour;
    if (apNpc)
    {
        // the server's id of the NPC: this game runs it (not known to the server yet, the others don't know it either)
        auto view = m_world.view<FormIdComponent, LocalComponent>();
        const auto it = std::find_if(view.begin(), view.end(), [&view, apNpc](const auto aEntity) { return view.template get<FormIdComponent>(aEntity).Id == apNpc->formID; });
        if (it == view.end())
            return;
        request.Parkour.IsNpc = true;
        request.Parkour.Npc = view.get<LocalComponent>(*it).Id;
    }
    request.Parkour.Move = static_cast<uint8_t>(acPath.Kind);
    std::copy(std::begin(acPath.Start), std::end(acPath.Start), request.Parkour.Start);
    request.Parkour.Heading = acPath.Heading;
    std::copy(std::begin(acPath.Ideal), std::end(acPath.Ideal), request.Parkour.Ideal);
    std::copy(std::begin(acPath.End), std::end(acPath.End), request.Parkour.End);
    request.Parkour.Bump = acPath.Bump;
    transport.Send(request);
}

void ParkourService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    if (acMessage.Type != RequestMovementAction::kParkour)
        return;

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor || pActor->IsDead() || !pActor->parentCell)
        return;

    // An NPC's move from the game that ran it just before this one did: this game has it now
    if (acMessage.Parkour.IsNpc && !pActor->GetExtension()->IsRemote())
        return;

    // What a message says is never taken on trust: a move this character could be making, near it
    const auto& in = acMessage.Parkour;
    Path path{};
    path.Kind = static_cast<Move>(in.Move);
    std::copy(std::begin(in.Start), std::end(in.Start), path.Start);
    path.Heading = in.Heading;
    std::copy(std::begin(in.Ideal), std::end(in.Ideal), path.Ideal);
    std::copy(std::begin(in.End), std::end(in.End), path.End);
    path.Bump = in.Bump;
    const float height = kPlayerHeight * pActor->GetScale();
    const float* pValues[] = {path.Start, path.Ideal, path.End};
    bool finite = std::isfinite(path.Heading) && std::isfinite(path.Bump);
    for (const float* pValue : pValues)
        finite = finite && std::all_of(pValue, pValue + 3, [](float aValue) { return std::isfinite(aValue); });
    const bool sane = in.Move < static_cast<uint8_t>(Move::kCount) && finite && path.Bump >= 0.f && path.Bump < height &&
                      std::hypot(path.Start[0] - pActor->position.x, path.Start[1] - pActor->position.y, path.Start[2] - pActor->position.z) < height * 2.f &&
                      std::hypot(path.End[0] - path.Start[0], path.End[1] - path.Start[1], path.End[2] - path.Start[2]) < height * 4.f;
    if (!sane)
    {
        spdlog::warn("Fluid Movement: {:X} sent a move that doesn't fit their character here, ignored", pActor->formID);
        return;
    }

    // Already playing one for them: that one gives way
    for (auto it = m_playing.begin(); it != m_playing.end(); ++it)
    {
        if (!it->Local && it->ActorId == pActor->formID)
        {
            Finish(*it, "replaced");
            m_playing.erase(it);
            break;
        }
    }

    // Their character here plays the third person move and travels it itself (the movement sync waits meanwhile); with
    // every place taken, the sync's positions alone show it
    if (!TakeMovingSlot(pActor))
        return;
    Playing move{};
    move.Way = path;
    move.ActorId = pActor->formID;
    move.Reference = pActor;
    move.StartYaw = pActor->rotation.z;
    move.View = 0;
    RealSetCollisionDisabled(pActor, true);
    move.WeaponsCulled = true;
    pActor->CullWeapons(true);

    auto view = m_world.view<FormIdComponent, InterpolationComponent>();
    for (const auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == pActor->formID)
            view.get<InterpolationComponent>(entity).Scripted = true;
    }

    bool animated = false;
    if (TESIdleForm* pIdle = GetIdle(path.Kind, 0); pIdle && pActor->currentProcess)
    {
        AllowRemoteIdle(pActor->formID, pIdle->formID);
        g_forceAnimation = true;
        animated = pActor->currentProcess->PlayIdle(pActor, pIdle, nullptr, false);
        g_forceAnimation = false;
    }
    m_playing.push_back(move);
    // a landing roll starts on the landing itself
    if (path.Kind == Move::kLandRoll)
        PlayRandom(m_landSounds, pActor);
    spdlog::info("Fluid Movement: {:X} does {}{}", pActor->formID, ParkourMoves::kNames[in.Move], animated ? "" : " (the animation didn't play)");
}

void ParkourService::UpdateNpcs() noexcept
{
    const bool climbs = m_rules.Parkour && m_rules.NpcParkour;
    if (!climbs && !m_rules.NpcSlides)
        return;

    const float delta = GameFrameTime();
    m_npcSince += delta;
    if ((m_npcTimer -= delta) > 0.f || m_npcSince <= 0.f)
        return;
    const float since = m_npcSince;
    m_npcSince = 0.f;
    m_npcTimer = kNpcInterval;
    m_npcClock += since;

    ProcessLists* pLists = ProcessLists::Get();
    if (!pLists)
        return;

    // Where the game's combat AI is sending NPCs into cover, noted by the hook since the last look
    m_coverWishes.clear();
    if (m_rules.NpcSlides)
    {
        std::vector<CoverWish> wishes;
        {
            std::scoped_lock lock(s_coverLock);
            wishes.swap(s_coverWishes);
        }
        for (const CoverWish& wish : wishes)
            if (const TESObjectREFR* pReference = TESObjectREFR::GetByHandle(wish.Handle))
                m_coverWishes[pReference->formID] = {wish.At[0], wish.At[1], wish.At[2]};
    }

    size_t moving = std::count_if(m_playing.begin(), m_playing.end(), [](const Playing& acMove) { return acMove.Owned; });
    bool planned = false;
    for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
    {
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(pLists->highActorHandleArray[i]));
        if (!IsFreeHumanNpc(pActor) || IsMoving(pActor))
            continue;

        NpcWatch& watch = m_npcWatch[pActor->formID];
        const NiPoint3 feet = pActor->position;
        const float moved = watch.HasPosition ? std::hypot(feet.x - watch.Position[0], feet.y - watch.Position[1]) : 0.f;
        const bool going = moved / since >= kNpcStillSpeed;
        const float wayX = going ? (feet.x - watch.Position[0]) / moved : 0.f;
        const float wayY = going ? (feet.y - watch.Position[1]) / moved : 0.f;
        watch.Still = going ? 0.f : watch.Still + since;
        watch.Position[0] = feet.x;
        watch.Position[1] = feet.y;
        watch.Position[2] = feet.z;
        watch.HasPosition = true;
        watch.LastSeen = m_npcClock;

        if (m_rules.NpcSlides && going && m_npcClock >= watch.NextSlide && TryNpcSlide(pActor, moved / since, wayX, wayY))
        {
            watch.NextSlide = m_npcClock + kNpcSlideCooldown;
            watch.NextTry = std::max(watch.NextTry, m_npcClock + kNpcRetry);
            continue;
        }

        if (!climbs || planned || moving >= kMaxNpcMoves || m_npcClock < watch.NextTry)
            continue;
        const auto wish = GetNpcWish(pActor, watch, going);
        if (!wish)
            continue;

        // nothing at knee height that way: nothing to climb or vault, and no need for the plan's rays
        const float scale = pActor->GetScale();
        const float z = feet.z + kNpcKneeHeight * scale;
        const float reach = kNpcLookAhead * scale;
        const RayHit knee = CastRay(pActor->parentCell, GetRayFilter(pActor), NiPoint3(glm::vec3(feet.x, feet.y, z)),
                                    NiPoint3(glm::vec3(feet.x + std::sin(wish->Heading) * reach, feet.y + std::cos(wish->Heading) * reach, z)));
        if (!knee.Hit || IsCharacterLayer(knee.Layer))
            continue;

        planned = true;
        const auto path = PlanNpc(pActor, *wish);
        if (path && StartNpc(pActor, *path))
        {
            ++moving;
            watch.Still = 0.f;
            watch.NextTry = m_npcClock + kNpcCooldown;
        }
        else
            watch.NextTry = m_npcClock + kNpcRetry;
    }

    // those not looked at for a while are gone, or far away
    std::erase_if(m_npcWatch, [this](const auto& acEntry) { return m_npcClock - acEntry.second.LastSeen > kNpcCooldown * 2.f; });
}

bool ParkourService::TryNpcSlide(Actor* apActor, float aSpeed, float aDirectionX, float aDirectionY) noexcept
{
    if (aSpeed < kNpcSlideMinSpeed || !apActor->IsInCombat())
        return false;
    auto& movement = m_world.ctx().at<MovementService>();
    if (movement.NpcSlideCount() >= kMaxNpcSlides)
        return false;

    const NiPoint3 feet = apActor->position;
    const float scale = apActor->GetScale();
    const uint32_t filter = GetRayFilter(apActor);
    // Clear ground that way: nothing at knee height for aLength (or nothing nearer than aClearFor), and the ground
    // where it ends up about as high as here
    const auto clear = [&](float aX, float aY, float aLength, float aClearFor)
    {
        const float knee = feet.z + kNpcKneeHeight * scale;
        const RayHit hit = CastRay(apActor->parentCell, filter, NiPoint3(glm::vec3(feet.x, feet.y, knee)), NiPoint3(glm::vec3(feet.x + aX * aLength, feet.y + aY * aLength, knee)));
        if (hit.Hit && std::hypot(hit.Point.x - feet.x, hit.Point.y - feet.y) < aClearFor)
            return false;
        const float endX = feet.x + aX * aLength, endY = feet.y + aY * aLength;
        const RayHit ground = CastRay(apActor->parentCell, filter, NiPoint3(glm::vec3(endX, endY, feet.z + 60.f * scale)), NiPoint3(glm::vec3(endX, endY, feet.z - 80.f * scale)));
        return ground.Hit && ground.NormalZ >= kMinTopNormalZ && std::abs(ground.Point.z - feet.z) < kSlideMaxRise;
    };

    // Into the cover the game's combat AI is taking it to, stopping short of it
    if (const auto it = m_coverWishes.find(apActor->formID); it != m_coverWishes.end())
    {
        const auto& at = it->second;
        const float dx = at[0] - feet.x, dy = at[1] - feet.y;
        const float distance = std::hypot(dx, dy);
        if (distance >= kCoverSlideMin && distance <= kCoverSlideMax && std::abs(at[2] - feet.z) < kSlideMaxRise &&
            (dx * aDirectionX + dy * aDirectionY) / distance >= kSlideOnTheWayCos)
        {
            const float length = distance - kCoverSlideStopShort;
            if (clear(dx / distance, dy / distance, length, length - 30.f))
                return movement.StartNpcSlide(apActor, dx / distance, dy / distance, aSpeed, "into cover");
        }
    }

    // At its target, now and then, over open ground
    Actor* pTarget = apActor->GetCombatTarget();
    if (!pTarget || pTarget == apActor || !pTarget->parentCell)
        return false;
    const float dx = pTarget->position.x - feet.x, dy = pTarget->position.y - feet.y;
    const float distance = std::hypot(dx, dy);
    if (distance < kChargeSlideMin || distance > kChargeSlideMax || std::abs(pTarget->position.z - feet.z) > kSlideMaxRise ||
        (dx * aDirectionX + dy * aDirectionY) / distance < kSlideOnTheWayCos)
        return false;
    static std::mt19937 s_random{std::random_device{}()};
    if (std::uniform_real_distribution<float>(0.f, 1.f)(s_random) >= (FightsUpClose(apActor) ? kChargeSlideChanceUpClose : kChargeSlideChance))
        return false;
    const float lane = std::min(kChargeSlideLane, distance - 150.f);
    if (!clear(dx / distance, dy / distance, lane, lane))
        return false;
    return movement.StartNpcSlide(apActor, dx / distance, dy / distance, aSpeed, "at its target");
}

std::optional<ParkourService::NpcWish> ParkourService::GetNpcWish(Actor* apActor, const NpcWatch& acWatch, bool aMoving) const noexcept
{
    const NiPoint3 feet = apActor->position;
    const float facing = apActor->rotation.z;
    const auto wayTo = [&feet](const NiPoint3& acTo) { return std::atan2(acTo.x - feet.x, acTo.y - feet.y); };
    const auto onTheWay = [facing](float aHeading) { return std::abs(std::remainder(aHeading - facing, 2.f * kPi)) <= kNpcOnTheWay; };
    const bool pathing = IsPathing(apActor);

    // In a fight: after its target, over what's in the way rather than around it, or with no way around at all. A gunner
    // that can see its target stays and shoots.
    if (Actor* pTarget = apActor->IsInCombat() ? apActor->GetCombatTarget() : nullptr; pTarget && pTarget != apActor && pTarget->parentCell)
    {
        const NiPoint3 at = pTarget->position;
        if (std::hypot(at.x - feet.x, at.y - feet.y) < kNpcCloseEnough && std::abs(at.z - feet.z) < kNpcCloseEnoughHeight)
            return std::nullopt;
        const float heading = wayTo(at);
        if (aMoving ? !onTheWay(heading) : acWatch.Still < (pathing ? kNpcStuckInCombat : kNpcNoPath))
            return std::nullopt;
        if (!FightsUpClose(apActor))
        {
            const float scale = apActor->GetScale();
            const RayHit sight = CastRay(apActor->parentCell, GetRayFilter(apActor), NiPoint3(glm::vec3(feet.x, feet.y, feet.z + kRuleHeight * scale)),
                                         NiPoint3(glm::vec3(at.x, at.y, at.z + kRuleHeight * 0.7f * pTarget->GetScale())));
            if (!sight.Hit || IsCharacterLayer(sight.Layer))
                return std::nullopt;
        }
        return NpcWish{heading, pTarget};
    }

    // A follower left behind: after the player the same way
    PAPYRUS_FUNCTION(bool, Actor, IsPlayerTeammate);
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get(); pPlayer && s_pIsPlayerTeammate(apActor))
    {
        const NiPoint3 at = pPlayer->position;
        if (std::hypot(at.x - feet.x, at.y - feet.y, at.z - feet.z) < kNpcFollowerBehind)
            return std::nullopt;
        const float heading = wayTo(at);
        if (aMoving ? !onTheWay(heading) : acWatch.Still < kNpcNoPath)
            return std::nullopt;
        return NpcWish{heading, nullptr};
    }

    // Anyone else, stuck on its way somewhere for a good while: whatever's in front of it
    if (pathing && !aMoving && acWatch.Still >= kNpcStuck)
        return NpcWish{facing, nullptr};
    return std::nullopt;
}

std::optional<ParkourService::Path> ParkourService::PlanNpc(Actor* apActor, const NpcWish& acWish) noexcept
{
    const float scale = apActor->GetScale();
    const NiPoint3 feet = apActor->position;

    std::optional<Path> path = PlanVault(apActor, acWish.Heading, true, false);
    if (!path)
    {
        const auto ledge = FindLedgeFrom(apActor, feet, acWish.Heading, false, false);
        if (!ledge)
            return std::nullopt;

        const float rise = (ledge->Top - feet.z) / scale;
        Move kind = Move::kCount;
        if (rise >= kLowFrom)
            kind = ClimbFor(rise);
        else if (rise >= kStepMin && ledge->EdgeDistance <= kStepReach * scale)
        {
            m_lastStepLeft = !m_lastStepLeft;
            kind = rise >= kStepHighFrom ? (m_lastStepLeft ? Move::kStepHighLeft : Move::kStepHighRight) : (m_lastStepLeft ? Move::kStepLowLeft : Move::kStepLowRight);
        }
        else
            return std::nullopt;
        path = PathOntoLedge(kind, feet, acWish.Heading, scale, *ledge);
    }

    // It has to be able to go on from where it lands. Navmesh there says so; without it, only someone being gone after
    // right there is worth it (the game walks it back to navmesh from there, or warps it).
    const NiPoint3 end(glm::vec3(path->End[0], path->End[1], path->End[2]));
    if (HasNavmeshAt(apActor, end))
        return path;
    if (acWish.pChasing)
    {
        const NiPoint3 at = acWish.pChasing->position;
        if (std::hypot(at.x - end.x, at.y - end.y) < kChaseOffNavmesh && std::abs(at.z - end.z) < kChaseOffNavmeshHeight)
            return path;
    }
    spdlog::info("Fluid Movement: NPC {:X} no {}, no navmesh where it would land", apActor->formID, ParkourMoves::kNames[static_cast<size_t>(path->Kind)]);
    return std::nullopt;
}

bool ParkourService::StartNpc(Actor* apActor, const Path& acPath) noexcept
{
    TESIdleForm* pIdle = GetIdle(acPath.Kind, 0);
    if (!pIdle || !TakeMovingSlot(apActor))
        return false;

    g_forceAnimation = true;
    const bool animated = apActor->currentProcess->PlayIdle(apActor, pIdle, nullptr, false);
    g_forceAnimation = false;
    if (!animated)
    {
        // busy with something the game won't break off: it stays with that
        ReleaseMovingSlot(apActor);
        spdlog::info("Fluid Movement: NPC {:X} no {}, the animation didn't play", apActor->formID, ParkourMoves::kNames[static_cast<size_t>(acPath.Kind)]);
        return false;
    }

    Playing move{};
    move.Way = acPath;
    move.ActorId = apActor->formID;
    move.Reference = apActor;
    move.Owned = true;
    move.StartYaw = apActor->rotation.z;
    move.View = 0;
    RealSetCollisionDisabled(apActor, true);
    if (bhkCharacterController* pController = apActor->GetCharController())
        pController->SetFallStartHeight(apActor->position.z);
    move.WeaponsCulled = true;
    apActor->CullWeapons(true);
    m_playing.push_back(move);
    Share(acPath, apActor);

    spdlog::info("Fluid Movement: NPC {:X} does {}, {:.0f} up and {:.0f} along", apActor->formID, ParkourMoves::kNames[static_cast<size_t>(acPath.Kind)], acPath.End[2] - acPath.Start[2],
                 std::hypot(acPath.End[0] - acPath.Start[0], acPath.End[1] - acPath.Start[1]));
    return true;
}

#endif

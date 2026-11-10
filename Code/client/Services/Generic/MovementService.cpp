#include <Services/MovementService.h>

#if TP_FALLOUT4

#include <Events/UpdateEvent.h>
#include <World.h>
#include <Components.h>
#include <Services/TransportService.h>
#include <Services/ParkourService.h>
#include <Services/TacticsService.h>

#include <Messages/RequestMovementAction.h>
#include <Messages/NotifyMovementAction.h>
#include <Messages/RequestMovementRules.h>
#include <Interface/MainMenuIntegration.h>

#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESIdleForm.h>
#include <Havok/bhkCharacterController.h>
#include <Games/IFormFactory.h>
#include <Games/ActorExtension.h>
#include <Audio/FileSound.h>

#include <variant>
#include <PerfProbe.h>

namespace
{
// ActorState flags1 (move mode) bits, read from the game while moving/swimming
constexpr uint32_t kMoveModeMoving = 1u << 7;
constexpr uint32_t kMoveModeSwimming = 1u << 10;
constexpr uint32_t kStanceSneaking = 1;

// RD's MCM page Movement: the values MCM saved over the defaults (MovementRules, the Rules structs)
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr auto kRulesInterval = std::chrono::seconds(1);

// Drops below this are normal jumps and steps down; about 3.5 m
constexpr float kStumbleDrop = 250.f;
// Stepping off small bumps counts as falling for a few frames
constexpr float kMinFallTime = 0.3f;

// Vanilla landing idles in Fallout4.esm: the heavy impact landing, third and first person
constexpr uint32_t kIdleJumpLandImpactToRun = 0x00017C6D;
constexpr uint32_t kIdle1stPJumpImpactLand = 0x0005B339;

// Havok units are metres; game units per Havok unit (bhkWorld scale 0.0142875)
constexpr float kGameUnitsPerHavokUnit = 69.99125f;

// Slide: keeps the momentum we had going in, slowed down by sliding friction (a constant deceleration of mu * g, what
// Actor Velocity Framework's linear interpolation from start to end velocity gives) to the tail speed, then a longer,
// gentler tail down to crouch-walking speed. Game units: sprinting is about 490 per second, a sprint slide goes about
// 300 (4.3 m) in the first 0.85 s and 70 more in the tail.
constexpr float kSlideFriction = 0.5f;
constexpr float kGravity = 9.81f * kGameUnitsPerHavokUnit;
constexpr float kSlideDeceleration = kSlideFriction * kGravity;
constexpr float kSlideTailSpeed = 200.f;
constexpr float kSlideEndSpeed = 120.f;
constexpr float kSlideTailDuration = 0.42f;
constexpr float kSlideMinStartSpeed = 250.f;
// Share of the momentum going in that the slide starts with
constexpr float kSlideMomentum = 0.85f;
// The slide animation moves the body forward itself (a slide with it went twice as far), so with it our push is smaller
// (0.4 went 625 units at 500 a second; this is a little slower and shorter)
constexpr float kSlidePushWithAnimation = 0.3f;
// The tail is a second push: started this long before the first one runs out, so no frame goes without one
constexpr float kSlideTailLead = 0.03f;
// How far back the momentum going in is taken from: the game starts braking as soon as crouch is pressed
constexpr float kMomentumWindow = 0.25f;
// Faster than anything moves on foot: a jump in position this big is a teleport or a load
constexpr float kMaxMoveSpeed = 2000.f;

// Toounx's slide sounds (Enhanced Movement's, used with permission), played like the player's own footstep and jump
// sounds: the vanilla FSTPlayerWoodJumpUp (Fallout4.esm) is the template, its files are what the calibration finds
constexpr uint32_t kSlideSoundTemplate = 0x002313C8;
const std::vector<std::string> kSlideSoundTemplateFiles{"FST_Wood_JumpUp_Player_01", "FST_Wood_JumpUp_Player_02"};
constexpr char kSlideSoundTemplateFolder[] = "Sound\\FX\\FST\\Player\\Wood";
constexpr char kSlideHitGroundSound[] = "Sound\\FX\\ReclamationDay\\SlideHitGround";
constexpr char kSlideMovingSound[] = "Sound\\FX\\ReclamationDay\\SlideMoving";

// First person slide: the left arm swings down to the floor and a little out, blended in and out (our own pose)
constexpr float kArmSwingDown = 70.f * 3.14159265f / 180.f;
constexpr float kArmSwingOut = 20.f * 3.14159265f / 180.f;
constexpr float kArmBlendInTime = 0.15f;
constexpr float kArmBlendOutTime = 0.3f;
// PlayerCharacter::firstPerson3D (CommonLibF4, 1.10.163)
constexpr uintptr_t kFirstPerson3DOffset = 0xB78;

// Toounx's slide animation (made for Enhanced Movement, used with permission), played as a dynamic idle on the third
// person graph: what an idle record with the event dyn_Activation and an animation file does. With the weapon away
// the game takes that event from someone sprinting into a crouch and plays nothing (seen in trials of every weapon
// state: the body only crouched, whenever the event was sent and however often). The event that also puts weapons
// away for the animation plays in every state, but hides a weapon that's out, so it's only for when none is.
constexpr char kSlideBehaviorGraph[] = "Actors\\Character\\Behaviors\\RaiderRootBehavior.hkx";
constexpr char kSlideAnimEvent[] = "dyn_Activation";
constexpr char kSlideAnimEventWeaponAway[] = "dyn_ActivationCullWeapons";
constexpr char kSlideAnimation[] = "Actors\\Character\\Animations\\ReclamationDay\\Slide.hkx";

// Actor Velocity Framework (by Bingle) through explicit linking, as its mod page documents: SetVelocity(actor, initial
// x, y, z, duration, terminal x, y, z, gravity) applies a velocity every frame for the duration, interpolated
// linearly, in game units per second
using TAvfSetVelocity = void (*)(std::monostate, Actor*, float, float, float, float, float, float, float, bool);

MovementService* s_pMovementService = nullptr;
} // namespace

// Animation.cpp: lets an action through on a remote character (otherwise only the animation sync's own replays are)
extern thread_local bool g_forceAnimation;
// Lets this idle through the block on remote characters' animations once, when the game performs it later (Animation.cpp)
void AllowRemoteIdle(uint32_t aActorId, uint32_t aIdleId) noexcept;

namespace
{

// Actor's IPostAnimationChannelUpdateFunctor (at Actor+0x168): the engine calls it after animating the actor, before
// the frame is drawn, which is where a procedural pose can change bones (id 1226709, found in a running game)
struct PostAnimationFunctor;
TP_THIS_FUNCTION(TPostAnimationUpdate, void, PostAnimationFunctor);
TPostAnimationUpdate* RealPostAnimationUpdate = nullptr;

void TP_MAKE_THISCALL(HookPostAnimationUpdate, PostAnimationFunctor)
{
    static PerfProbe::HookStat s_perf("PostAnimation");
    PerfProbe::Hook _perf(s_perf);
    {
        PerfProbe::GameCall _game;
        TiltedPhoques::ThisCall(RealPostAnimationUpdate, apThis);
    }

    if (!s_pMovementService)
        return;

    auto* pActor = reinterpret_cast<Actor*>(reinterpret_cast<uint8_t*>(apThis) - 0x168);
    if (pActor == PlayerCharacter::Get())
        s_pMovementService->AfterPlayerAnimation(static_cast<PlayerCharacter*>(pActor));
}

// NiAVObject (CommonLibF4, 1.10.163): parent at 0x28, local transform at 0x30, world at 0x70; a rotation is 3 rows of
// 4 floats. The game works local first, world = local * parent world as stored (its multiply, id 739335): stored
// rotations are the transposes of the usual ones that turn column vectors.
struct NiRotation
{
    float m[3][4];
};

NiRotation Multiply(const NiRotation& acLhs, const NiRotation& acRhs) noexcept
{
    NiRotation result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            result.m[row][column] = acLhs.m[row][0] * acRhs.m[0][column] + acLhs.m[row][1] * acRhs.m[1][column] + acLhs.m[row][2] * acRhs.m[2][column];
    return result;
}

NiRotation Transpose(const NiRotation& acRotation) noexcept
{
    NiRotation result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            result.m[row][column] = acRotation.m[column][row];
    return result;
}

// Rotation of aAngle radians about a unit axis
NiRotation AxisAngle(float aX, float aY, float aZ, float aAngle) noexcept
{
    const float c = std::cos(aAngle);
    const float s = std::sin(aAngle);
    const float t = 1.f - c;

    NiRotation result{};
    result.m[0][0] = t * aX * aX + c;
    result.m[0][1] = t * aX * aY - s * aZ;
    result.m[0][2] = t * aX * aZ + s * aY;
    result.m[1][0] = t * aX * aY + s * aZ;
    result.m[1][1] = t * aY * aY + c;
    result.m[1][2] = t * aY * aZ - s * aX;
    result.m[2][0] = t * aX * aZ - s * aY;
    result.m[2][1] = t * aY * aZ + s * aX;
    result.m[2][2] = t * aZ * aZ + c;
    return result;
}

uint8_t* GetObjectByName(uint8_t* apNode, const BSFixedString& acName) noexcept
{
    using TGetObjectByName = uint8_t* (*)(uint8_t*, const BSFixedString&);
    return (*reinterpret_cast<TGetObjectByName* const*>(apNode))[0x2E](apNode, acName);
}

// The engine's camera shake, what Papyrus Game.ShakeCamera calls after clamping the strength to 0-1 (found by
// disassembling that native on 1.10.163; id 758209)
void ShakeCamera(float aStrength, const NiPoint3& acPosition, float aDuration) noexcept
{
    using TShakeCamera = void(float, const NiPoint3*, float);
    POINTER_FALLOUT4(TShakeCamera, s_shakeCamera, 758210);
    s_shakeCamera.Get()(std::clamp(aStrength, 0.f, 1.f), &acPosition, aDuration);
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

TAvfSetVelocity GetAvfSetVelocity() noexcept
{
    static TAvfSetVelocity s_pSetVelocity = nullptr;
    static bool s_reportedMissing = false;
    if (s_pSetVelocity)
        return s_pSetVelocity;

    if (const HMODULE hAvf = GetModuleHandleA("ActorVelocityFramework.dll"))
        s_pSetVelocity = reinterpret_cast<TAvfSetVelocity>(GetProcAddress(hAvf, "SetVelocity"));

    if (!s_pSetVelocity && !s_reportedMissing)
    {
        s_reportedMissing = true;
        spdlog::warn("Movement: Actor Velocity Framework isn't loaded, no slides or dodges");
    }
    return s_pSetVelocity;
}
} // namespace

static TiltedPhoques::Initializer s_movementHooks(
    []()
    {
        POINTER_FALLOUT4(TPostAnimationUpdate, s_postAnimationUpdate, 1226710);
        RealPostAnimationUpdate = s_postAnimationUpdate.Get();
        TP_HOOK(&RealPostAnimationUpdate, HookPostAnimationUpdate);
    });

MovementService::MovementService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&MovementService::OnUpdate>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&MovementService::OnNotifyMovementAction>(this);
    s_pMovementService = this;

    m_pSlideHitGroundSound = std::make_unique<FileSound>(kSlideSoundTemplate, kSlideSoundTemplateFiles, kSlideSoundTemplateFolder, kSlideHitGroundSound);
    m_pSlideMovingSound = std::make_unique<FileSound>(kSlideSoundTemplate, kSlideSoundTemplateFiles, kSlideSoundTemplateFolder, kSlideMovingSound);
}

MovementService::~MovementService() noexcept
{
    s_pMovementService = nullptr;
}

// The first person slide pose, on the first person skeleton once the game has animated it (maybe on an animation
// thread, so only atomics are shared with the rest)
void MovementService::AfterPlayerAnimation(PlayerCharacter* apPlayer) noexcept
{
    const auto now = Clock::now();
    const float delta = m_lastArmUpdate == Clock::time_point{} ? 0.f : std::min(std::chrono::duration<float>(now - m_lastArmUpdate).count(), 0.1f);
    m_lastArmUpdate = now;

    if (m_firstPersonSlideArm.load(std::memory_order_relaxed))
        m_armBlend = std::min(1.f, m_armBlend + delta / kArmBlendInTime);
    else
        m_armBlend = std::max(0.f, m_armBlend - delta / kArmBlendOutTime);

    if (m_armBlend <= 0.f)
        return;

    uint8_t* pRoot = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(apPlayer) + kFirstPerson3DOffset);
    if (!pRoot)
        return;

    static BSFixedString s_upperArmName("LArm_UpperArm");
    uint8_t* pUpperArm = GetObjectByName(pRoot, s_upperArmName);
    uint8_t* pParent = pUpperArm ? *reinterpret_cast<uint8_t**>(pUpperArm + 0x28) : nullptr;
    if (!m_loggedArm)
    {
        m_loggedArm = true;
        spdlog::info("Movement: first person left upper arm {}, parent {}", fmt::ptr(pUpperArm), fmt::ptr(pParent));
    }
    if (!pParent)
        return;

    auto& local = *reinterpret_cast<NiRotation*>(pUpperArm + 0x30);
    const auto& parentWorld = *reinterpret_cast<const NiRotation*>(pParent + 0x70);

    // TEMP: are world transforms already computed by now (then a changed local shows a frame late, or not at all)?
    if (!m_loggedWorldOrder)
    {
        m_loggedWorldOrder = true;
        const auto& world = *reinterpret_cast<const NiRotation*>(pUpperArm + 0x70);
        const NiRotation computed = Multiply(local, parentWorld);
        float difference = 0.f;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                difference = std::max(difference, std::abs(computed.m[row][column] - world.m[row][column]));
        spdlog::info("Movement: after animation, upper arm world {} local * parent world (largest difference {:.4f})", difference < 0.001f ? "already matches" : "doesn't match yet",
                     difference);
    }

    // About the body's sideways axis (down) and the up axis (out to the left), in world space: whatever way the bone's
    // own axes point. The swing turns the world rotation (swing * world for column vectors, world * swing^T as stored),
    // so the new local is local * parent * swing^T * parent^T. The other way round mixed the player's heading into it.
    const float heading = apPlayer->rotation.z;
    const float rightX = std::cos(heading);
    const float rightY = -std::sin(heading);
    const NiRotation swing = Multiply(AxisAngle(0.f, 0.f, 1.f, kArmSwingOut * m_armBlend), AxisAngle(rightX, rightY, 0.f, -kArmSwingDown * m_armBlend));
    local = Multiply(local, Multiply(parentWorld, Multiply(Transpose(swing), Transpose(parentWorld))));
}

void MovementService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Movement");
    UpdateRules();
    UpdateNpcSlides();

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->IsDead())
        return;

    const float delta = static_cast<float>(acEvent.Delta);

    // How we've actually been moving over the last frames, in world space (the physics velocity is already braking
    // by the time crouch is pressed)
    if (delta > 0.f && m_hasLastPosition)
    {
        const float vx = (pPlayer->position.x - m_lastPosition[0]) / delta;
        const float vy = (pPlayer->position.y - m_lastPosition[1]) / delta;
        if (vx * vx + vy * vy > kMaxMoveSpeed * kMaxMoveSpeed)
        {
            // a teleport or load, not movement
            m_groundVelocity[0] = m_groundVelocity[1] = 0.f;
        }
        else
        {
            m_groundVelocity[0] += (vx - m_groundVelocity[0]) * 0.3f;
            m_groundVelocity[1] += (vy - m_groundVelocity[1]) * 0.3f;
        }

        m_speedSamples[m_nextSpeedSample] = {Clock::now(), std::sqrt(m_groundVelocity[0] * m_groundVelocity[0] + m_groundVelocity[1] * m_groundVelocity[1])};
        m_nextSpeedSample = (m_nextSpeedSample + 1) % m_speedSamples.size();
    }
    m_lastPosition[0] = pPlayer->position.x;
    m_lastPosition[1] = pPlayer->position.y;
    m_hasLastPosition = true;

    UpdateSprintCost(pPlayer, delta);
    UpdateApDrains(pPlayer, delta);

    if (bhkCharacterController* pController = pPlayer->GetCharController())
    {
        UpdateSlide(pPlayer, pController);
        UpdateLanding(pPlayer, pController);
    }

    m_lastActionPoints = pPlayer->GetActorValue(ActorValueInfo::kActionPoints);
}

float MovementService::GetRecentPeakSpeed() const noexcept
{
    const auto since = Clock::now() - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kMomentumWindow));
    float peak = 0.f;
    for (const auto& sample : m_speedSamples)
    {
        if (sample.Time >= since)
            peak = std::max(peak, sample.Speed);
    }
    return peak;
}

// Out of combat, gives back part of the AP the game takes for sprinting (whatever it took since the last frame)
void MovementService::UpdateSprintCost(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    m_combatCheckTimer -= aDelta;
    if (m_combatCheckTimer <= 0.f)
    {
        m_combatCheckTimer = 0.5f;
        m_inCombat = apPlayer->IsInCombat();
    }

    if (!m_rules.CheaperSprintOutOfCombat || m_inCombat || m_lastActionPoints < 0.f || !apPlayer->actorState.IsSprinting())
        return;

    const float actionPoints = apPlayer->GetActorValue(ActorValueInfo::kActionPoints);
    const float spent = m_lastActionPoints - actionPoints;
    if (spent <= 0.f)
        return;

    const float refund = spent * (1.f - m_rules.SprintOutOfCombatCost);
    apPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kActionPoints, actionPoints + refund);
}

// Swimming and sneaking while moving cost AP over time; Endurance and Agility make them cheaper
void MovementService::UpdateApDrains(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const auto& state = apPlayer->actorState;
    const uint32_t moveMode = state.GetMoveMode();
    if (!(moveMode & kMoveModeMoving))
        return;

    float perSecond = 0.f;
    if (moveMode & kMoveModeSwimming)
    {
        if (m_rules.SwimmingApDrain)
            perSecond = std::clamp(7.f - 0.6f * apPlayer->GetActorValue(ActorValueInfo::kEndurance), 1.f, 6.f);
    }
    else if (state.GetStance() == kStanceSneaking)
    {
        if (m_rules.SneakingApDrain && !m_sliding)
            perSecond = std::clamp(3.5f - 0.3f * apPlayer->GetActorValue(ActorValueInfo::kAgility), 0.5f, 3.f);
    }

    if (perSecond <= 0.f)
        return;

    const float actionPoints = apPlayer->GetActorValue(ActorValueInfo::kActionPoints);
    if (actionPoints > 0.f)
        apPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kActionPoints, std::max(0.f, actionPoints - perSecond * aDelta));
}

// Crouching while sprinting starts a slide: the game keeps both set for a few frames, then drops the sprint
void MovementService::UpdateSlide(PlayerCharacter* apPlayer, bhkCharacterController* apController) noexcept
{
    const auto& state = apPlayer->actorState;
    const uint32_t stance = state.GetStance();
    const bool sprinting = state.IsSprinting();

    if (m_sliding)
    {
        // Actor Velocity Framework pushes for the whole duration, off ledges too
        const float seconds = std::chrono::duration<float>(Clock::now() - m_slideStart).count();
        if (!m_slideInTail && seconds >= m_slideMainDuration - kSlideTailLead)
        {
            // from the speed the main push has slowed down to by now
            StartSlideTail(apPlayer, std::max(m_slideStartSpeed - kSlideDeceleration * seconds, kSlideEndSpeed));
        }
        else if (m_slideInTail && std::chrono::duration<float>(Clock::now() - m_slideTailStart).count() >= kSlideTailDuration)
        {
            m_sliding = false;
            m_firstPersonSlideArm = false;
            const float dx = apPlayer->position.x - m_slideStartPosition[0];
            const float dy = apPlayer->position.y - m_slideStartPosition[1];
            const float distance = std::sqrt(dx * dx + dy * dy);
            spdlog::info("Movement: slide over, moved {:.0f} units = {:.0f} per second on average, towards ({:.2f} {:.2f})", distance, distance / seconds, distance > 0.f ? dx / distance : 0.f,
                         distance > 0.f ? dy / distance : 0.f);
        }
    }
    else if (m_rules.Sliding && stance == kStanceSneaking && m_lastStance != kStanceSneaking && (sprinting || m_wasSprinting) &&
             apController->GetFallTime() <= 0.f && !apPlayer->IsInPowerArmor())
    {
        m_sliding = StartSlide(apPlayer, apController);
    }

    m_wasSprinting = sprinting;
    m_lastStance = stance;
}

bool MovementService::StartSlide(PlayerCharacter* apPlayer, bhkCharacterController* apController) noexcept
{
    const auto pSetVelocity = GetAvfSetVelocity();
    if (!pSetVelocity)
        return false;

    const float maxActionPoints = apPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints);
    const float cost = m_rules.SlideApCost * maxActionPoints;
    const float actionPoints = apPlayer->GetActorValue(ActorValueInfo::kActionPoints);
    if (actionPoints < cost)
    {
        spdlog::info("Movement: no slide, {:.0f} AP of {:.0f} needed", actionPoints, cost);
        return false;
    }

    // The momentum we had going in, along the way we've been moving (facing when we've barely moved)
    const float momentum = GetRecentPeakSpeed();
    if (momentum < kSlideMinStartSpeed)
    {
        spdlog::info("Movement: no slide, moving at {:.0f}", momentum);
        return false;
    }
    const float startSpeed = momentum * kSlideMomentum;

    const float currentSpeed = std::sqrt(m_groundVelocity[0] * m_groundVelocity[0] + m_groundVelocity[1] * m_groundVelocity[1]);
    float directionX = std::sin(apPlayer->rotation.z);
    float directionY = std::cos(apPlayer->rotation.z);
    if (currentSpeed > 50.f)
    {
        directionX = m_groundVelocity[0] / currentSpeed;
        directionY = m_groundVelocity[1] / currentSpeed;
    }

    apPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kActionPoints, actionPoints - cost);

    m_slideStart = Clock::now();
    m_slideStartPosition[0] = apPlayer->position.x;
    m_slideStartPosition[1] = apPlayer->position.y;
    m_slideDirection[0] = directionX;
    m_slideDirection[1] = directionY;
    m_slideStartSpeed = startSpeed;
    m_slideInTail = false;

    // Only for the third person body: in first person the game plays it on the hidden body too, which breaks the
    // first person view
    bool animated = false;
    const bool firstPerson = IsFirstPerson();
    if (firstPerson)
    {
        // our own pose on the first person arms instead
        m_firstPersonSlideArm = true;
    }
    else
    {
        animated = PlaySlideAnimation(apPlayer);
    }
    m_slidePushScale = animated ? kSlidePushWithAnimation : 1.f;

    // The others see the third person body slide either way
    ShareAction(RequestMovementAction::kSlide);

    const float mainEndSpeed = std::min(startSpeed, kSlideTailSpeed);
    m_slideMainDuration = (startSpeed - mainEndSpeed) / kSlideDeceleration;
    spdlog::info("Movement: slide from {:.0f} at {:.0f} units per second, {:.2f} s then the tail, {} ({:.0f} AP)", momentum, startSpeed, m_slideMainDuration,
                 animated ? "animated" : firstPerson ? "the arm pose (first person)" : "the animation didn't play", cost);

    if (m_rules.SlideCameraShake > 0.f)
        ShakeCamera(m_rules.SlideCameraShake, apPlayer->position, m_slideMainDuration + kSlideTailDuration);

    const bool hitGround = m_pSlideHitGroundSound->Play(apPlayer);
    const bool moving = m_pSlideMovingSound->Play(apPlayer);
    if (!m_loggedSlideSounds)
    {
        m_loggedSlideSounds = true;
        spdlog::info("Movement: slide sounds playing: hit ground {}, moving {}", hitGround, moving);
    }

    const float push = m_slidePushScale;
    if (m_slideMainDuration > kSlideTailLead)
        pSetVelocity({}, apPlayer, directionX * startSpeed * push, directionY * startSpeed * push, 0.f, m_slideMainDuration, directionX * mainEndSpeed * push, directionY * mainEndSpeed * push, 0.f, true);
    else
        StartSlideTail(apPlayer, startSpeed);

    return true;
}

TESIdleForm* MovementService::GetSlideIdle(bool aWeaponDrawn) noexcept
{
    TESIdleForm*& pSlideIdle = m_pSlideIdles[aWeaponDrawn ? 1 : 0];
    if (pSlideIdle)
        return pSlideIdle;

    auto* pIdle = IFormFactory::Create<TESIdleForm>();
    if (!pIdle)
    {
        spdlog::warn("Movement: couldn't create the slide idle");
        return nullptr;
    }

    pIdle->behaviorGraphName.Set(kSlideBehaviorGraph);
    pIdle->animEventName.Set(aWeaponDrawn ? kSlideAnimEvent : kSlideAnimEventWeaponAway);
    pIdle->animFileName.Set(kSlideAnimation);
    // as Enhanced Movement's slide idle record has it
    pIdle->idleFlags = 1;

    spdlog::info("Movement: slide idle created ({:X}) for {}, the weapon {}", pIdle->formID, kSlideAnimation, aWeaponDrawn ? "out" : "away");
    pSlideIdle = pIdle;
    return pIdle;
}

void MovementService::StartSlideTail(PlayerCharacter* apPlayer, float aSpeed) noexcept
{
    m_slideInTail = true;
    m_slideTailStart = Clock::now();

    if (const auto pSetVelocity = GetAvfSetVelocity())
    {
        const float startSpeed = aSpeed * m_slidePushScale;
        const float endSpeed = std::min(aSpeed, kSlideEndSpeed) * m_slidePushScale;
        pSetVelocity({}, apPlayer, m_slideDirection[0] * startSpeed, m_slideDirection[1] * startSpeed, 0.f, kSlideTailDuration, m_slideDirection[0] * endSpeed, m_slideDirection[1] * endSpeed, 0.f, true);
    }
}

bool MovementService::StartNpcSlide(Actor* apActor, float aDirectionX, float aDirectionY, float aSpeed, const char* acpWhy) noexcept
{
    const auto pSetVelocity = GetAvfSetVelocity();
    TESIdleForm* pIdle = GetSlideIdle(apActor->actorState.IsWeaponDrawn());
    if (!pSetVelocity || !pIdle || !apActor->currentProcess)
        return false;

    // The animation first: without it, no push either
    AllowRemoteIdle(apActor->formID, pIdle->formID);
    g_forceAnimation = true;
    const bool animated = apActor->currentProcess->PlayIdle(apActor, pIdle, nullptr, false);
    g_forceAnimation = false;
    if (!animated)
    {
        spdlog::info("Movement: NPC {:X} no slide ({}), the animation didn't play", apActor->formID, acpWhy);
        return false;
    }

    // As the player's: the momentum it had, slowing by friction to the tail, the animation's own travel with ours on top
    const float startSpeed = std::max(aSpeed, kSlideMinStartSpeed) * kSlideMomentum;
    const float mainEndSpeed = std::min(startSpeed, kSlideTailSpeed);
    const float mainDuration = std::max((startSpeed - mainEndSpeed) / kSlideDeceleration, kSlideTailLead);
    const float push = kSlidePushWithAnimation;
    pSetVelocity({}, apActor, aDirectionX * startSpeed * push, aDirectionY * startSpeed * push, 0.f, mainDuration, aDirectionX * mainEndSpeed * push,
                 aDirectionY * mainEndSpeed * push, 0.f, true);
    m_npcSlides.push_back({apActor->formID, Clock::now(), mainDuration, {aDirectionX, aDirectionY}, mainEndSpeed, {apActor->position.x, apActor->position.y}, false});

    m_pSlideHitGroundSound->Play(apActor);
    m_pSlideMovingSound->Play(apActor);

    // The others play it on their copy of the NPC (its server id: this game runs it)
    if (auto& transport = m_world.GetTransport(); transport.IsOnline())
    {
        auto view = m_world.view<FormIdComponent, LocalComponent>();
        const auto it = std::find_if(view.begin(), view.end(), [&view, apActor](const auto aEntity) { return view.template get<FormIdComponent>(aEntity).Id == apActor->formID; });
        if (it != view.end())
        {
            RequestMovementAction request{};
            request.Type = RequestMovementAction::kNpcSlide;
            request.Npc = view.get<LocalComponent>(*it).Id;
            transport.Send(request);
        }
    }

    spdlog::info("Movement: NPC {:X} slides {} from {:.0f}, {:.2f} s then the tail", apActor->formID, acpWhy, startSpeed, mainDuration);
    return true;
}

void MovementService::UpdateNpcSlides() noexcept
{
    const auto now = Clock::now();
    for (auto it = m_npcSlides.begin(); it != m_npcSlides.end();)
    {
        NpcSlide& slide = *it;
        const float seconds = std::chrono::duration<float>(now - slide.Start).count();
        Actor* pActor = Cast<Actor>(TESForm::GetById(slide.ActorId));
        if (!pActor || pActor->IsDead())
        {
            it = m_npcSlides.erase(it);
            continue;
        }
        if (!slide.InTail && seconds >= slide.MainDuration - kSlideTailLead)
        {
            slide.InTail = true;
            if (const auto pSetVelocity = GetAvfSetVelocity())
            {
                const float startSpeed = slide.TailSpeed * kSlidePushWithAnimation;
                const float endSpeed = std::min(slide.TailSpeed, kSlideEndSpeed) * kSlidePushWithAnimation;
                pSetVelocity({}, pActor, slide.Direction[0] * startSpeed, slide.Direction[1] * startSpeed, 0.f, kSlideTailDuration, slide.Direction[0] * endSpeed,
                             slide.Direction[1] * endSpeed, 0.f, true);
            }
        }
        if (seconds >= slide.MainDuration + kSlideTailDuration)
        {
            spdlog::info("Movement: NPC {:X} slide over, moved {:.0f} units", slide.ActorId,
                         std::hypot(pActor->position.x - slide.From[0], pActor->position.y - slide.From[1]));
            it = m_npcSlides.erase(it);
            continue;
        }
        ++it;
    }
}

void MovementService::UpdateLanding(PlayerCharacter* apPlayer, bhkCharacterController* apController) noexcept
{
    const float fallTime = apController->GetFallTime();
    if (fallTime > 0.f)
    {
        m_falling = true;
        m_lastFallTime = fallTime;
        m_fallApex = apController->GetFallStartHeight();
        return;
    }

    if (!m_falling)
        return;

    m_falling = false;
    const float drop = m_fallApex - apPlayer->position.z;
    if (m_lastFallTime < kMinFallTime)
        return;

    spdlog::info("Movement: landed after {:.2f} s, dropped {:.0f}", m_lastFallTime, drop);

    // a landing roll takes the landing instead (crouch pressed just before it); power armor has its own heavy landing
    if (m_rules.LandingStumble && drop >= kStumbleDrop && !ParkourService::RollsLanding() && !apPlayer->IsInPowerArmor())
        PlayLandingStumble(apPlayer, drop);
}

void MovementService::PlayLandingStumble(PlayerCharacter* apPlayer, float aDrop) noexcept
{
    const bool played = PlayLandingIdle(apPlayer);
    spdlog::info("Movement: landing stumble {}after a drop of {:.0f}", played ? "" : "not played ", aDrop);

    // The others see the third person body stumble, whatever this view played
    ShareAction(RequestMovementAction::kLandingStumble);
}

bool MovementService::PlayLandingIdle(Actor* apActor) noexcept
{
    AIProcess* pProcess = apActor->currentProcess;
    if (!pProcess)
        return false;

    // The third person idle only plays on the third person graph and the other way around, so try both. Our own
    // conditions decide (the idles' own test the vertical speed, which is gone by the time we see the landing).
    for (const uint32_t idleId : {kIdleJumpLandImpactToRun, kIdle1stPJumpImpactLand})
    {
        auto* pIdle = Cast<TESIdleForm>(TESForm::GetById(idleId));
        if (!pIdle)
            continue;
        if (apActor->GetExtension()->IsRemote())
            AllowRemoteIdle(apActor->formID, pIdle->formID);
        if (pProcess->PlayIdle(apActor, pIdle, nullptr, false))
            return true;
    }

    return false;
}

bool MovementService::PlaySlideAnimation(Actor* apActor) noexcept
{
    TESIdleForm* pIdle = GetSlideIdle(apActor->actorState.IsWeaponDrawn());
    return pIdle && apActor->currentProcess && apActor->currentProcess->PlayIdle(apActor, pIdle, nullptr, false);
}

void MovementService::LoadLocalRules() noexcept
{
    const auto path = std::filesystem::current_path() / L"Data" / kSettingsFile;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    const auto stamp = ec ? std::filesystem::file_time_type{} : time;
    if (m_settingsLoaded && stamp == m_settingsTime)
        return;
    m_settingsLoaded = true;
    m_settingsTime = stamp;

    const std::wstring file = path.wstring();
    const auto number = [&file](const wchar_t* acpSection, const wchar_t* acpKey, float aDefault)
    {
        wchar_t text[32]{};
        GetPrivateProfileStringW(acpSection, acpKey, L"", text, static_cast<DWORD>(std::size(text)), file.c_str());
        wchar_t* pEnd = nullptr;
        const float value = std::wcstof(text, &pEnd);
        return pEnd != text && std::isfinite(value) ? value : aDefault;
    };
    const auto flag = [&number](const wchar_t* acpSection, const wchar_t* acpKey, bool aDefault) { return number(acpSection, acpKey, aDefault ? 1.f : 0.f) != 0.f; };
    // costs are percentages on the page
    const auto share = [&number](const wchar_t* acpSection, const wchar_t* acpKey, float aDefault) { return std::clamp(number(acpSection, acpKey, aDefault * 100.f), 0.f, 100.f) / 100.f; };

    const MovementRules defaults{};
    MovementRules rules;
    rules.Sliding = flag(L"Movement", L"bSliding", defaults.Sliding);
    rules.SlideApCost = share(L"Movement", L"iSlideApCost", defaults.SlideApCost);
    rules.CheaperSprintOutOfCombat = flag(L"Movement", L"bCheaperSprintOutOfCombat", defaults.CheaperSprintOutOfCombat);
    rules.SprintOutOfCombatCost = share(L"Movement", L"iSprintOutOfCombatCost", defaults.SprintOutOfCombatCost);
    rules.SwimmingApDrain = flag(L"Movement", L"bSwimmingApDrain", defaults.SwimmingApDrain);
    rules.SneakingApDrain = flag(L"Movement", L"bSneakingApDrain", defaults.SneakingApDrain);
    rules.LandingStumble = flag(L"Movement", L"bLandingStumble", defaults.LandingStumble);
    rules.Parkour = flag(L"Parkour", L"bEnabled", defaults.Parkour);
    rules.ParkourApCost = share(L"Parkour", L"iClimbApCost", defaults.ParkourApCost);
    rules.LandingRoll = flag(L"Parkour", L"bLandingRoll", defaults.LandingRoll);
    rules.NpcParkour = flag(L"Parkour", L"bNpcParkour", defaults.NpcParkour);
    rules.NpcTactics = flag(L"Tactics", L"bNpcTactics", defaults.NpcTactics);
    rules.NpcSlides = flag(L"Tactics", L"bNpcSlides", defaults.NpcSlides);
    m_localRules = rules;
    m_localCameraShake = std::clamp(number(L"Movement", L"fSlideCameraShake", Rules{}.SlideCameraShake), 0.f, 1.f);
    m_localMarkers = flag(L"Parkour", L"bLedgeMarkers", true);
}

void MovementService::UpdateRules() noexcept
{
    const auto now = Clock::now();
    if (m_settingsLoaded && now - m_lastRulesCheck < kRulesInterval)
        return;
    m_lastRulesCheck = now;

    LoadLocalRules();

    auto& transport = m_world.GetTransport();
    const bool online = transport.IsOnline();
    const bool hosting = online && IsHostingWorld();
    if (!online)
        m_sentRules.reset();

    // The host's rules are the server's, as its difficulty is: sent when they change (and on every connection), and
    // never taken back from the server
    if (hosting && (!m_sentRules || *m_sentRules != m_localRules))
    {
        RequestMovementRules request{};
        request.Rules = m_localRules;
        transport.Send(request);
        m_sentRules = m_localRules;
        spdlog::info("Movement: the host's rules sent to the server");
    }

    const ServerSettings& server = m_world.GetServerSettings();
    const bool hostRules = online && !hosting && server.HasMovementRules;
    if (hostRules != m_usingHostRules)
    {
        m_usingHostRules = hostRules;
        spdlog::info("Movement: {} rules", hostRules ? "the host's" : "this game's own");
    }
    const MovementRules& rules = hostRules ? server.Movement : m_localRules;

    m_rules.Sliding = rules.Sliding;
    m_rules.SlideApCost = rules.SlideApCost;
    m_rules.SlideCameraShake = m_localCameraShake;
    m_rules.CheaperSprintOutOfCombat = rules.CheaperSprintOutOfCombat;
    m_rules.SprintOutOfCombatCost = rules.SprintOutOfCombatCost;
    m_rules.SwimmingApDrain = rules.SwimmingApDrain;
    m_rules.SneakingApDrain = rules.SneakingApDrain;
    m_rules.LandingStumble = rules.LandingStumble;

    auto& parkour = m_world.ctx().at<ParkourService>();
    ParkourService::Rules parkourRules = parkour.GetRules();
    parkourRules.Parkour = rules.Parkour;
    parkourRules.ApCost = rules.ParkourApCost;
    parkourRules.LandingRoll = rules.LandingRoll;
    parkourRules.NpcParkour = rules.NpcParkour;
    parkourRules.NpcSlides = rules.NpcSlides;
    parkourRules.Markers = m_localMarkers;
    parkour.SetRules(parkourRules);

    m_world.ctx().at<TacticsService>().SetEnabled(rules.NpcTactics);
}

void MovementService::ShareAction(uint8_t aAction) noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;

    RequestMovementAction request{};
    request.Type = aAction;
    transport.Send(request);
}

void MovementService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor || pActor->IsDead())
        return;

    if (acMessage.Type == RequestMovementAction::kSlide)
    {
        // The animation sync blocks actions on remote characters that it doesn't replay itself
        if (TESIdleForm* pIdle = GetSlideIdle(pActor->actorState.IsWeaponDrawn()))
            AllowRemoteIdle(pActor->formID, pIdle->formID);
        g_forceAnimation = true;
        const bool animated = PlaySlideAnimation(pActor);
        g_forceAnimation = false;

        m_pSlideHitGroundSound->Play(pActor);
        m_pSlideMovingSound->Play(pActor);
        spdlog::info("Movement: {:X} slides, {}", pActor->formID, animated ? "animated" : "the animation didn't play");
    }
    else if (acMessage.Type == RequestMovementAction::kLandingStumble)
    {
        g_forceAnimation = true;
        const bool played = PlayLandingIdle(pActor);
        g_forceAnimation = false;

        spdlog::info("Movement: {:X} stumbles landing{}", pActor->formID, played ? "" : ", the idle didn't play");
    }
}

#endif

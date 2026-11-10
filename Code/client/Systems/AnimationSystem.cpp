#include <HivemindPCH.h>

#include <Systems/AnimationSystem.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Messages/ClientReferencesMoveRequest.h>
#include <TiltedCore/Math.hpp>
#include <PerfProbe.h>

#include <array>
#include <cmath>

#include <Components.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

extern thread_local const char* g_animErrorCode;

namespace
{
// How far an actor can drift from what the others last got of it before it goes out again
constexpr float kPositionSlack = 1.f;    // game units: positions go out in whole units anyway
constexpr float kRotationSlack = 0.005f; // radians, about a third of a degree
constexpr float kDirectionSlack = 0.01f;
constexpr float kFloatSlack = 0.001f; // of the value's size, at least 1
// An actor that hasn't changed still goes out this often
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);

// Why an actor's snapshot went out, or that it didn't (the "Perf: movement" lines)
enum SendReason : uint32_t
{
    kFirst,
    kActions,
    kHeartbeat,
    kCell,
    kPosition,
    kRotation,
    kDirection,
    kBooleans,
    kIntegers,
    kFloats,
    kUnchanged,
    kReasonCount
};
constexpr const char* kReasonNames[kReasonCount] = {"first", "actions", "heartbeat", "cell", "moved", "turned", "direction", "bools", "ints", "floats", "left out"};
std::array<uint32_t, kReasonCount> s_reasons{};
// Which float variable it was, when one was
std::array<uint32_t, 64> s_floatChanges{};

bool NearAngle(float aA, float aB, float aSlack) noexcept
{
    return std::abs(std::remainder(aA - aB, 2.f * static_cast<float>(TiltedPhoques::Pi))) <= aSlack;
}

bool NearValue(float aA, float aB) noexcept
{
    if (std::isnan(aA) || std::isnan(aB))
        return std::isnan(aA) && std::isnan(aB);
    return std::abs(aA - aB) <= kFloatSlack * std::max(1.f, std::abs(aB));
}

SendReason Compare(const Movement& acNow, const Movement& acSent) noexcept
{
    if (acNow.CellId != acSent.CellId || acNow.WorldSpaceId != acSent.WorldSpaceId)
        return kCell;
    if (std::abs(acNow.Position.x - acSent.Position.x) > kPositionSlack || std::abs(acNow.Position.y - acSent.Position.y) > kPositionSlack ||
        std::abs(acNow.Position.z - acSent.Position.z) > kPositionSlack)
        return kPosition;
    if (!NearAngle(acNow.Rotation.x, acSent.Rotation.x, kRotationSlack) || !NearAngle(acNow.Rotation.y, acSent.Rotation.y, kRotationSlack))
        return kRotation;
    if (!NearAngle(acNow.Direction, acSent.Direction, kDirectionSlack))
        return kDirection;

    const auto& now = acNow.Variables;
    const auto& sent = acSent.Variables;
    if (now.Booleans != sent.Booleans)
        return kBooleans;
    if (now.Integers != sent.Integers)
        return kIntegers;
    if (now.Floats.size() != sent.Floats.size())
        return kFloats;
    for (size_t i = 0; i < now.Floats.size(); ++i)
    {
        if (!NearValue(now.Floats[i], sent.Floats[i]))
        {
            if (i < s_floatChanges.size())
                ++s_floatChanges[i];
            return kFloats;
        }
    }
    return kUnchanged;
}
} // namespace

void AnimationSystem::Update(World& aWorld, Actor* apActor, RemoteAnimationComponent& aAnimationComponent, const uint64_t aTick) noexcept
{
    auto& actions = aAnimationComponent.TimePoints;

    const auto it = std::begin(actions);
    if (it != std::end(actions) && it->Tick <= aTick)
    {
        // Its animation graph isn't loaded yet: the action waits (upstream fix #789)
        if (!apActor->animationGraphHolder.IsReady())
            return;

        if (aAnimationComponent.ReplayCount > 0 && aAnimationComponent.ResetAnimationGraphForReplay)
        {
            ResetAnimationGraph(apActor);
            aAnimationComponent.ResetAnimationGraphForReplay = false;
        }

        const auto& first = *it;

        const auto actionId = first.ActionId;
        const auto targetId = first.TargetId;

        const auto pAction = Cast<BGSAction>(TESForm::GetById(actionId));
        const auto pTarget = Cast<TESObjectREFR>(TESForm::GetById(targetId));

        apActor->actorState.flags1 = first.State1;
        apActor->actorState.flags2 = first.State2;

        apActor->LoadAnimationVariables(first.Variables);

        aAnimationComponent.LastRanAction = first;

#if TP_FALLOUT4
        // A replayed state swapped for its "instant" event by the server (g_defaultStateInstant...): no action or idle
        // goes with it, the graph takes the event as is
        if (!pAction && first.IdleId == 0 && !first.EventName.empty())
        {
            BSFixedString eventName(first.EventName.c_str());
            apActor->animationGraphHolder.SendAnimationEvent(&eventName);
            eventName.Release();
        }
        else
#endif
        {
            // Play the animation
            TESActionData actionData(first.Type & 0x3, apActor, pAction, pTarget);
            actionData.eventName = BSFixedString(first.EventName.c_str());
            actionData.idleForm = Cast<TESIdleForm>(TESForm::GetById(first.IdleId));
            actionData.someFlag = ((first.Type & 0x4) != 0) ? 1 : 0;

            ActorMediator::Get()->ForceAction(&actionData);
        }

        if (aAnimationComponent.ReplayCount > 0)
            aAnimationComponent.ReplayCount--;

        actions.pop_front();
    }
}

void AnimationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    aWorld.emplace_or_replace<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<RemoteAnimationComponent>(aEntity))
        aWorld.remove<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::AddActionsForReplay(RemoteAnimationComponent& aAnimationComponent, const ActionReplayChain& acReplay) noexcept
{
    aAnimationComponent.TimePoints.insert(aAnimationComponent.TimePoints.end(), acReplay.Actions.begin(), acReplay.Actions.end());
    aAnimationComponent.ReplayCount = static_cast<uint32_t>(acReplay.Actions.size());
    aAnimationComponent.ResetAnimationGraphForReplay = acReplay.ResetAnimationGraph;

    if (!acReplay.Actions.empty())
    {
        spdlog::info("Replaying {} animation actions (reset graph: {}), from {} to {}", acReplay.Actions.size(), acReplay.ResetAnimationGraph,
                     acReplay.Actions.front().EventName.c_str(), acReplay.Actions.back().EventName.c_str());
    }
}

void AnimationSystem::ResetAnimationGraph(Actor* apActor) noexcept
{
#if TP_FALLOUT4
    // What the game itself does to snap an actor's graph to its base state (BGSAnimationSystemUtils::InitializeActorInstant):
    // ActionInstantInitializeGraphToBaseState, which every race's graph maps to its own event
    constexpr uint32_t kActionInstantInitializeGraphToBaseState = 0x5704C;

    const auto pAction = Cast<BGSAction>(TESForm::GetById(kActionInstantInitializeGraphToBaseState));
    if (!pAction)
        return;

    TESActionData actionData(2, apActor, pAction, nullptr);
    ActorMediator::Get()->ForceAction(&actionData);
#else
    apActor->animationGraphHolder.RevertAnimationGraphManager();
#endif
}

void AnimationSystem::AddAction(RemoteAnimationComponent& aAnimationComponent, const std::string& acActionDiff) noexcept
{
    auto itor = std::begin(aAnimationComponent.TimePoints);
    const auto end = std::cend(aAnimationComponent.TimePoints);

    auto& lastProcessedAction = aAnimationComponent.LastProcessedAction;

    TiltedPhoques::ViewBuffer buffer((uint8_t*)acActionDiff.data(), acActionDiff.size());
    Buffer::Reader reader(&buffer);

    lastProcessedAction.ApplyDifferential(reader);

    aAnimationComponent.TimePoints.push_back(lastProcessedAction);
}

void AnimationSystem::Serialize(World& aWorld, ClientReferencesMoveRequest& aMovementSnapshot, LocalComponent& localComponent, LocalAnimationComponent& animationComponent, FormIdComponent& formIdComponent,
                                std::chrono::steady_clock::time_point aNow)
{
    const auto pForm = TESForm::GetById(formIdComponent.Id);
    const auto pActor = Cast<Actor>(pForm);
    if (!pActor)
        return;

    Movement movement;

    if (const auto pCell = pActor->parentCell)
        World::Get().GetModSystem().GetServerModId(pCell->formID, movement.CellId.ModId, movement.CellId.BaseId);

    if (const auto pWorldSpace = pActor->GetWorldSpace())
        World::Get().GetModSystem().GetServerModId(pWorldSpace->formID, movement.WorldSpaceId.ModId, movement.WorldSpaceId.BaseId);

    movement.Position = pActor->position;

    movement.Rotation.x = pActor->rotation.x;
    movement.Rotation.y = pActor->rotation.z;

    pActor->SaveAnimationVariables(movement.Variables);

    if (pActor->currentProcess && pActor->currentProcess->middleProcess)
    {
        movement.Direction = pActor->currentProcess->middleProcess->direction;
    }

    // An actor that hasn't changed since the others last got it (one standing, as most are most of the time) is left
    // out but for a heartbeat; the others keep it as it was meanwhile (InterpolationSystem::AddPoint)
    SendReason reason = kFirst;
    if (localComponent.LastSentTime != std::chrono::steady_clock::time_point{})
    {
        reason = animationComponent.Actions.empty() ? Compare(movement, localComponent.LastSentMovement) : kActions;
        if (reason == kUnchanged && aNow - localComponent.LastSentTime >= kHeartbeatInterval)
            reason = kHeartbeat;
    }
    ++s_reasons[reason];
    if (reason == kUnchanged)
        return;

    localComponent.LastSentMovement = movement;
    localComponent.LastSentTime = aNow;

    auto& update = aMovementSnapshot.Updates[localComponent.Id];
    update.UpdatedMovement = std::move(movement);

    for (auto& entry : animationComponent.Actions)
    {
        update.ActionEvents.push_back(entry);
    }

    auto latestAction = animationComponent.GetLatestAction();

    if (latestAction)
        localComponent.CurrentAction = latestAction.MoveResult();

    animationComponent.Actions.clear();
}

bool AnimationSystem::Serialize(World& aWorld, const ActionEvent& aActionEvent, const ActionEvent& aLastProcessedAction, std::string* apData)
{
    uint32_t actionBaseId = 0;
    uint32_t actionModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.ActionId, actionModId, actionBaseId))
        return false;

    uint32_t targetBaseId = 0;
    uint32_t targetModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.TargetId, targetModId, targetBaseId))
        return false;

    uint8_t scratch[1 << 14];
    TiltedPhoques::ViewBuffer buffer(scratch, std::size(scratch));
    Buffer::Writer writer(&buffer);
    aActionEvent.GenerateDifferential(aLastProcessedAction, writer);

    apData->assign(buffer.GetData(), buffer.GetData() + writer.Size());

    return true;
}

std::string AnimationSystem::TakeSendStats() noexcept
{
    std::string out = "Perf: movement snapshots";
    for (uint32_t i = 0; i < kReasonCount; ++i)
    {
        if (s_reasons[i])
            out += fmt::format(" {} {}", kReasonNames[i], s_reasons[i]);
    }
    s_reasons = {};

    std::vector<std::pair<uint32_t, uint32_t>> floats;
    for (uint32_t i = 0; i < s_floatChanges.size(); ++i)
    {
        if (s_floatChanges[i])
            floats.emplace_back(s_floatChanges[i], i);
    }
    s_floatChanges = {};
    std::sort(floats.rbegin(), floats.rend());
    if (!floats.empty())
        out += ";";
    for (size_t i = 0; i < floats.size() && i < 8; ++i)
        out += fmt::format(" float{} {}", floats[i].second, floats[i].first);
    return out;
}

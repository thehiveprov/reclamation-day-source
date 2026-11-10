#include <HivemindPCH.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <Forms/TESIdleForm.h>

#include <Structs/ActionEvent.h>

#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>

#include <Misc/BSFixedString.h>

#include <World.h>
#include <PerfProbe.h>
#if TP_FALLOUT4
#include <Services/ParkourService.h>
#endif

#include <map>
#include <tuple>

TP_THIS_FUNCTION(TPerformAction, uint8_t, ActorMediator, TESActionData* apAction);
static TPerformAction* RealPerformAction;

// TODO: make scoped override
thread_local bool g_forceAnimation = false;

namespace
{
// Idles our own code plays on remote characters (a slide, a landing stumble). The game can perform an idle later, from
// the character's own update, where g_forceAnimation isn't set, and the block below stopped them (no one ever saw
// another player slide), so each is also allowed once, for a moment, by the character's and the idle's ids.
struct AllowedIdle
{
    uint32_t ActorId;
    uint32_t IdleId;
    std::chrono::steady_clock::time_point Until;
};
std::mutex s_allowedIdlesLock;
std::vector<AllowedIdle> s_allowedIdles;
std::atomic<uint32_t> s_allowedIdleCount{0};

bool ConsumeAllowedIdle(uint32_t aActorId, uint32_t aIdleId) noexcept
{
    if (s_allowedIdleCount.load(std::memory_order_relaxed) == 0)
        return false;

    std::scoped_lock lock(s_allowedIdlesLock);
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(s_allowedIdles, [now](const AllowedIdle& acIdle) { return acIdle.Until < now; });
    const auto it = std::ranges::find_if(s_allowedIdles, [&](const AllowedIdle& acIdle) { return acIdle.ActorId == aActorId && acIdle.IdleId == aIdleId; });
    const bool found = it != s_allowedIdles.end();
    if (found)
        s_allowedIdles.erase(it);
    s_allowedIdleCount = static_cast<uint32_t>(s_allowedIdles.size());
    return found;
}

// The actions the local characters ran, by action, event and whether the game took them (only taken ones go to the
// others), for the "Perf: actions" lines
std::mutex s_actionStatsLock;
std::map<std::tuple<uint32_t, std::string, bool>, uint32_t> s_actionStats;

void CountAction(const ActionEvent& acAction, bool aTaken) noexcept
{
    std::scoped_lock lock(s_actionStatsLock);
    ++s_actionStats[{acAction.ActionId, acAction.EventName.c_str(), aTaken}];
}
} // namespace

std::string TakeActionStats() noexcept
{
    std::vector<std::pair<uint32_t, std::tuple<uint32_t, std::string, bool>>> entries;
    {
        std::scoped_lock lock(s_actionStatsLock);
        for (auto& [key, count] : s_actionStats)
            entries.emplace_back(count, key);
        s_actionStats.clear();
    }
    std::sort(entries.begin(), entries.end(), [](const auto& acA, const auto& acB) { return acA.first > acB.first; });
    std::string out = "Perf: actions";
    for (size_t i = 0; i < entries.size() && i < 10; ++i)
    {
        const auto& [actionId, eventName, taken] = entries[i].second;
        out += fmt::format(" {:X} '{}' {} {};", actionId, eventName, taken ? "taken" : "refused, not sent", entries[i].first);
    }
    return out;
}

void AllowRemoteIdle(uint32_t aActorId, uint32_t aIdleId) noexcept
{
    std::scoped_lock lock(s_allowedIdlesLock);
    s_allowedIdles.push_back({aActorId, aIdleId, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    s_allowedIdleCount = static_cast<uint32_t>(s_allowedIdles.size());
}

uint8_t TP_MAKE_THISCALL(HookPerformAction, ActorMediator, TESActionData* apAction)
{
    static PerfProbe::HookStat s_perf("PerformAction");
    PerfProbe::Hook _perf(s_perf);
    auto pActor = apAction->actor;
    const auto pExtension = pActor->GetExtension();

#if TP_FALLOUT4
    // An NPC in a parkour move (docs/NPC_PARKOUR.md): the move has its body until it lands, and what its AI wants done
    // waits, but for the move's own idle, which the game may perform a little later, and the graph's reset below
    if (!g_forceAnimation && !pExtension->IsPlayer() && !pExtension->IsRemote() && ParkourService::IsMoving(pActor) && apAction->action->formID != 0x5704c &&
        !(apAction->idleForm && (apAction->idleForm->formID & 0xFF000000) == 0xFF000000))
        return 0;
#endif

    const bool forced = g_forceAnimation || (pExtension->IsRemote() && apAction->idleForm && ConsumeAllowedIdle(pActor->formID, apAction->idleForm->formID));
    if (!pExtension->IsRemote() || forced)
    {
        ActionEvent action;
        action.State1 = pActor->actorState.flags1;
        action.State2 = pActor->actorState.flags2;
        action.Type = apAction->unkInput | (apAction->someFlag ? 0x4 : 0);
        action.Tick = World::Get().GetTick();
        action.ActorId = pActor->formID;
        action.ActionId = apAction->action->formID;
        action.TargetId = apAction->target ? apAction->target->formID : 0;

        pActor->SaveAnimationVariables(action.Variables);

        uint8_t res;
        {
            PerfProbe::GameCall _game;
            res = TiltedPhoques::ThisCall(RealPerformAction, apThis, apAction);
        }

        // spdlog::debug("Action event name: {}, target name: {}", apAction->eventName.AsAscii(), apAction->targetEventName.AsAscii());

        // This is a weird case where it gets spammed and doesn't do much, not sure if it still needs to be sent over the network
        if (apAction->someFlag == 1 || forced)
            return res;

        // An idle made at runtime (ours, the slide) has an id only this game knows: the others would replay the action
        // without it. Those are shared on their own (MovementService).
        if (apAction->idleForm && (apAction->idleForm->formID & 0xFF000000) == 0xFF000000)
            return res;

        action.EventName = apAction->eventName.AsAscii();
        action.TargetEventName = apAction->targetEventName.AsAscii();
        action.IdleId = apAction->idleForm ? apAction->idleForm->formID : 0;

        if (PerfProbe::Enabled())
            CountAction(action, res != 0);

        // One the game refused changed nothing here, and the others replaying it could change their copy of the
        // character. NPCs ask for some over and over while they can't be done (a sheathe, hundreds of times a second for
        // a few of them): they were most of what went out.
        if (!res)
            return res;

        // Save for later
        pExtension->LatestAnimation = action;

        World::Get().GetRunner().Trigger(action);

        return res;
    }

#if TP_FALLOUT4
    // Let the ActionInstantInitializeGraphToBaseState event go through
    if (apAction->action->formID == 0x5704c)
    {
        PerfProbe::GameCall _game;
        return TiltedPhoques::ThisCall(RealPerformAction, apThis, apAction);
    }
#endif

    return 0;
}

ActorMediator* ActorMediator::Get() noexcept
{
    POINTER_FALLOUT4(ActorMediator*, s_actorMediator, 1358859);
    POINTER_SKYRIMSE(ActorMediator*, s_actorMediator, 403567);

    return *(s_actorMediator.Get());
}

bool ActorMediator::PerformAction(TESActionData* apAction) noexcept
{
    if (apAction->actor->formID == 0x13482)
    {
        /*static Set<uint32_t> s_ids;

        spdlog::error("New frame");
        for(auto i = 0; i < action.Variables.size(); ++i)
        {
            auto& oldVars = pExtension->LatestVariables.Variables;
            auto& newVars = action.Variables;
            if(oldVars[i] != newVars[i] && s_ids.count(i) == 0)
            {
                //s_ids.insert(i);
                spdlog::info("Var {} changed from {} to {}", i, oldVars[i], newVars[i]);
            }
        }*/
        // spdlog::info("Play animation name: {} with idle {:X} and target {:X} and unk {:X}", apAction->action->keyword.AsAscii(), (apAction->idleForm ? apAction->idleForm->formID : 0), (apAction->target ? apAction->target->formID : 0), apAction->unkInput);
    }

    const auto res = TiltedPhoques::ThisCall(RealPerformAction, this, apAction);
    // const auto res = RePerformAction(apAction, aValue);

    if (res && apAction->actor->formID == 0x13482)
    {
        //    spdlog::info("Passed !");
    }

    return res != 0;
}

bool ActorMediator::ForceAction(TESActionData* apAction) noexcept
{
    TP_THIS_FUNCTION(TAnimationStep, uint8_t, ActorMediator, TESActionData*);
    using TApplyAnimationVariables = void*(void*, TESActionData*);

    POINTER_SKYRIMSE(TApplyAnimationVariables, ApplyAnimationVariables, 39004);
    POINTER_SKYRIMSE(TAnimationStep, PerformComplexAction, 38953);
    POINTER_SKYRIMSE(void*, qword_142F271B8, 403566);

    POINTER_FALLOUT4(TAnimationStep, PerformComplexAction, 1445653);
    uint8_t result = 0;

    auto pActor = static_cast<Actor*>(apAction->actor);
    if (!pActor || pActor->animationGraphHolder.IsReady())
    {
        result = TiltedPhoques::ThisCall(PerformComplexAction, this, apAction);

#if TP_SKYRIM64
        ApplyAnimationVariables(*qword_142F271B8.Get(), apAction);
#endif
    }

    return result;
}

ActionInput::ActionInput(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
{
    // skip vtable as we never use this directly
    actor = apActor;
    target = apTarget;
    action = apAction;
    unkInput = aParam1;
}

void ActionInput::Release()
{
    actor.Release();
    target.Release();
}

ActionOutput::ActionOutput()
    : eventName("")
    , targetEventName("")
{
    // skip vtable as we never use this directly

    result = 0;
    targetIdleForm = nullptr;
    idleForm = nullptr;
    unk1 = 0;
}

void ActionOutput::Release()
{
    eventName.Release();
    targetEventName.Release();
}

BGSActionData::BGSActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : ActionInput(aParam1, apActor, apAction, apTarget)
{
    // skip vtable as we never use this directly
    someFlag = 0;
}

TESActionData::TESActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : BGSActionData(aParam1, apActor, apAction, apTarget)
{
    POINTER_FALLOUT4(void*, s_vtbl, 780201);
    POINTER_SKYRIMSE(void*, s_vtbl, 188603);

    someFlag = false;

    *reinterpret_cast<void**>(this) = s_vtbl.Get();
}

TESActionData::~TESActionData()
{
    ActionOutput::Release();
    ActionInput::Release();
}

static TiltedPhoques::Initializer s_animationHook(
    []()
    {
        POINTER_FALLOUT4(TPerformAction, performAction, 502377);
        POINTER_SKYRIMSE(TPerformAction, performAction, 38949);

        RealPerformAction = performAction.Get();

        TP_HOOK(&RealPerformAction, HookPerformAction);
    });

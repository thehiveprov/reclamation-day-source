#include <Destruction.h>

#include <Games/References.h>
#include <Forms/TESForm.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>

#include <Events/DestructionStageEvent.h>

#include <World.h>

namespace
{
// Read from the exe (1.10.163, CommonLibF4 ids; RD's are + 1). The object damage the Papyrus DamageObject task runs
// (771685): the object's own destructible data takes the damage (210663), which moves the stage, swaps the model,
// spawns the stage's explosion and debris, and sends TESDestructionStageChangedEvent.
using TDamageObject = void(TESObjectREFR* apObject, float aDamage, bool aEvenIfDestroyed);
// The base form's destructible data (65291), null when it has none
using TGetDestructibleForm = void*(TESForm* apBaseForm);
// The most health an object has: its destructible data's (1181312)
using TGetMaxHealth = int32_t(void* apDestructibleForm, TESObjectREFR* apObject);
// The object's health (1025343): its ExtraHealth (extra data 0x55, +0x18), -1 while it hasn't been damaged
using TGetExtraHealth = float(ExtraDataList* apExtraData);

// The thread replaying another player's stage change right now: what it causes isn't sent back
std::atomic<uint32_t> s_applyingThread{0};

struct DestructionStageSink final : BSTEventSink<TESDestructionStageChangedEvent>
{
    BSTEventResult OnEvent(const TESDestructionStageChangedEvent* apEvent, const EventDispatcher<TESDestructionStageChangedEvent>*) override
    {
        // Only stages going up: ClearDestruction (repairs, a cell reset when the player gets there) stays the game's own
        if (!apEvent || !apEvent->pObject || apEvent->NewStage <= apEvent->OldStage || apEvent->pObject->formType == FormType::Character)
            return BSTEventResult::kOk;

        if (s_applyingThread.load(std::memory_order_relaxed) == GetCurrentThreadId())
            return BSTEventResult::kOk;

        World::Get().GetRunner().Trigger(DestructionStageEvent{apEvent->pObject->formID, apEvent->OldStage, apEvent->NewStage});
        return BSTEventResult::kOk;
    }
};

DestructionStageSink s_sink;

void* GetDestructibleForm(TESObjectREFR* apObject) noexcept
{
    POINTER_FALLOUT4(TGetDestructibleForm, s_getDestructibleForm, 65292);

    return apObject && apObject->baseForm ? s_getDestructibleForm.Get()(apObject->baseForm) : nullptr;
}
} // namespace

namespace Destruction
{
void Install() noexcept
{
    static bool s_installed = false;
    if (std::exchange(s_installed, true))
        return;

    GetEventDispatcher_TESDestructionStageChangedEvent()->RegisterSink(&s_sink);
}

float GetMaxHealth(TESObjectREFR* apObject) noexcept
{
    POINTER_FALLOUT4(TGetMaxHealth, s_getMaxHealth, 1181313);

    void* pDestructible = GetDestructibleForm(apObject);
    return pDestructible ? static_cast<float>(s_getMaxHealth.Get()(pDestructible, apObject)) : 0.f;
}

float GetHealth(TESObjectREFR* apObject) noexcept
{
    POINTER_FALLOUT4(TGetExtraHealth, s_getExtraHealth, 1025344);

    const float maxHealth = GetMaxHealth(apObject);
    if (maxHealth <= 0.f)
        return 0.f;

    ExtraDataList* pExtraData = apObject->GetExtraDataList();
    const float health = pExtraData ? s_getExtraHealth.Get()(pExtraData) : -1.f;
    return health < 0.f ? maxHealth : health;
}

bool DamageTo(TESObjectREFR* apObject, float aHealth) noexcept
{
    POINTER_FALLOUT4(TDamageObject, s_damageObject, 771686);

    if (!apObject || apObject->formType == FormType::Character || GetMaxHealth(apObject) <= 0.f)
        return false;

    const float damage = GetHealth(apObject) - aHealth;
    if (damage <= 0.f)
        return false;

    s_applyingThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    s_damageObject.Get()(apObject, damage, true);
    s_applyingThread.store(0, std::memory_order_relaxed);
    return true;
}
} // namespace Destruction

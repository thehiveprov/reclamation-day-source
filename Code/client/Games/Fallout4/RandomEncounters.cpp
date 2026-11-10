#include <RandomEncounters.h>

#include <Forms/TESForm.h>

namespace
{
// Fallout4.esm's REEnabled (GLOB)
constexpr uint32_t kREEnabledId = 0x2DFE9;

// Conditions can be evaluated off the main thread
std::atomic<bool> s_allowed{true};

// The GetGlobalValue condition function (script command 0x104A). The global is the condition's first parameter, the
// value goes to aResult.
using TGetGlobalValueCondition = bool(void* apParams, TESForm* apGlobal, void* apParam2, float& aResult);
TGetGlobalValueCondition* RealGetGlobalValueCondition = nullptr;

bool HookGetGlobalValueCondition(void* apParams, TESForm* apGlobal, void* apParam2, float& aResult)
{
    if (apGlobal && apGlobal->formID == kREEnabledId && !s_allowed.load(std::memory_order_relaxed))
    {
        aResult = 0.f;
        return true;
    }

    return RealGetGlobalValueCondition(apParams, apGlobal, apParam2, aResult);
}
} // namespace

namespace RandomEncounters
{
void SetAllowed(bool aAllowed) noexcept
{
    if (s_allowed.exchange(aAllowed, std::memory_order_relaxed) != aAllowed)
        spdlog::info("Random encounters {} on this game", aAllowed ? "enabled" : "disabled");
}

bool IsAllowed() noexcept
{
    return s_allowed.load(std::memory_order_relaxed);
}
} // namespace RandomEncounters

static TiltedPhoques::Initializer s_randomEncountersHooks(
    []()
    {
        POINTER_FALLOUT4(TGetGlobalValueCondition, s_getGlobalValueCondition, 971508);

        RealGetGlobalValueCondition = s_getGlobalValueCondition.Get();

        TP_HOOK(&RealGetGlobalValueCondition, HookGetGlobalValueCondition);
    });

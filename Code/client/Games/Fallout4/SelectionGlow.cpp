#include <SelectionGlow.h>

#include <TESObjectREFR.h>
#include <Games/Memory.h>

#include <algorithm>

// The target glow's manager (checked against 1.10.163): its targets are 3D made into a glow target, a reference counted
// object of 0xA8 that keeps the 3D's geometry. Enable's first argument picks VATS's own drawing (its menu passes true);
// everything else that glows outside VATS (the workshop, companion commands, the ViewCaster) passes false, and that's the
// one that draws in the world. Enable only switches the image space effect on (effect 12) once there are targets.

namespace
{
constexpr uint32_t kTargetConstructor = 820922;
constexpr uint32_t kTargetDestructor = 1073051;
constexpr size_t kTargetSize = 0xA8;
constexpr size_t kTargetGeometryCount = 0x30;
constexpr uint32_t kAddTarget = 126856;
constexpr uint32_t kEnable = 499746;
constexpr uint32_t kSetColour = 403513;
constexpr uint32_t kClearTargets = 1415368;
constexpr uint32_t kDisable = 845362;
// The manager's state: on (someone's glow is up)
constexpr uint32_t kGlowOn = 166379;
// TESObjectREFR's 3D
constexpr size_t kGet3D = 0x8C;

struct Glow
{
    // the target we added (one reference of ours), for which reference and 3D, in which colour
    void* pTarget = nullptr;
    uint32_t Handle = 0;
    const void* p3D = nullptr;
    float Colour[3]{};
};
Glow s_glow{};

volatile long* RefCount(void* apTarget) noexcept
{
    return static_cast<volatile long*>(apTarget);
}

void ReleaseTarget(void*& apTarget) noexcept
{
    if (!apTarget)
        return;
    if (_InterlockedDecrement(RefCount(apTarget)) == 0)
    {
        using TDestructor = void(void*);
        POINTER_FALLOUT4(TDestructor, s_destructor, kTargetDestructor);
        s_destructor.Get()(apTarget);
        Memory::Free(apTarget);
    }
    apTarget = nullptr;
}

// Whether the glow's targets still hold ours (VATS and the workshop clear them when they start)
bool StillOurs() noexcept
{
    return s_glow.pTarget && *RefCount(s_glow.pTarget) > 1;
}

// Someone else's glow is up (the workshop, companion commands): theirs, not to be touched
bool SomeoneElsesUp() noexcept
{
    POINTER_FALLOUT4(bool, s_glowOn, kGlowOn);
    return *s_glowOn.Get() && !StillOurs();
}

const void* Get3D(TESObjectREFR* apRef) noexcept
{
    using TGet3D = void*(TESObjectREFR*);
    return (*reinterpret_cast<TGet3D* const* const*>(apRef))[kGet3D](apRef);
}
} // namespace

void SelectionGlow::Set(TESObjectREFR* apRef, uint32_t aHandle, float aRed, float aGreen, float aBlue) noexcept
{
    const void* p3D = apRef ? Get3D(apRef) : nullptr;
    const float colour[3] = {aRed, aGreen, aBlue};
    if (s_glow.Handle == aHandle && s_glow.p3D == p3D && std::equal(colour, colour + 3, s_glow.Colour) && StillOurs())
        return;

    Clear();
    if (!p3D || SomeoneElsesUp())
        return;

    using TConstructor = void*(void*, const void*);
    using TAddTarget = void(void**);
    using TEnable = void(bool, uint8_t);
    using TSetColour = void(float, float, float);
    POINTER_FALLOUT4(TConstructor, s_constructor, kTargetConstructor);
    POINTER_FALLOUT4(TAddTarget, s_addTarget, kAddTarget);
    POINTER_FALLOUT4(TEnable, s_enable, kEnable);
    POINTER_FALLOUT4(TSetColour, s_setColour, kSetColour);

    void* pMemory = Memory::Allocate(kTargetSize);
    if (!pMemory)
        return;
    void* pTarget = s_constructor.Get()(pMemory, p3D);
    _InterlockedIncrement(RefCount(pTarget));
    // nothing to draw in it (no geometry): the game doesn't take it
    if (*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pTarget) + kTargetGeometryCount) == 0)
    {
        ReleaseTarget(pTarget);
        return;
    }
    s_addTarget.Get()(&pTarget);
    // the world's glow, as the workshop puts it up (not VATS's own); then our colour over the default
    s_enable.Get()(false, 0);
    s_setColour.Get()(aRed, aGreen, aBlue);
    s_glow = {pTarget, aHandle, p3D, {aRed, aGreen, aBlue}};
}

void SelectionGlow::Clear() noexcept
{
    if (StillOurs())
    {
        using TClearTargets = void();
        using TDisable = void(bool);
        POINTER_FALLOUT4(TClearTargets, s_clearTargets, kClearTargets);
        POINTER_FALLOUT4(TDisable, s_disable, kDisable);
        s_clearTargets.Get()();
        s_disable.Get()(false);
    }
    ReleaseTarget(s_glow.pTarget);
    s_glow = {};
}

void SelectionGlow::Release() noexcept
{
    if (StillOurs())
        return;
    ReleaseTarget(s_glow.pTarget);
    s_glow = {};
}

uint32_t SelectionGlow::Current() noexcept
{
    return StillOurs() ? s_glow.Handle : 0;
}

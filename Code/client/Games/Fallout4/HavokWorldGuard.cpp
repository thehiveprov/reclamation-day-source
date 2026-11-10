// A guard against a crash in Havok's ragdolls (seen 2026-10-13/14, three times, once 4 s after a random encounter quest
// finished): a worker thread updating an actor's ragdoll (hkbnpRagdollInterface, its constraint pass id 1142510) read
// the constraints of the physics world the ragdoll was in after that world had been destroyed and freed (the world's
// first quadword a heap free list link instead of its vtable, its body and constraint arrays emptied), while the main
// thread was in the middle of a frame. What destroys the world isn't known yet: every world destroyed is logged
// (thread, the call stack as game offsets, the player's cell) so the next time says, and the ragdoll's pass is skipped
// for a world destroyed moments ago or one without its constraints, which is all that was left to crash on.

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>

#include <array>
#include <atomic>
#include <chrono>

namespace
{
using Clock = std::chrono::steady_clock;

// The game's code is mapped at its own image base: offsets from it are what tools/addr.js and the crash logs use
constexpr uintptr_t kGameBase = 0x140000000;
constexpr uintptr_t kGameEnd = 0x148000000;

// Worlds destroyed lately: a ragdoll still pointing at one of these is past saving. Kept this long.
constexpr auto kDestroyedMemory = std::chrono::seconds(3);
constexpr size_t kDestroyedSlots = 32;
std::array<std::atomic<uintptr_t>, kDestroyedSlots> s_destroyedWorlds{};
std::array<std::atomic<int64_t>, kDestroyedSlots> s_destroyedAt{};
std::atomic<size_t> s_nextSlot{0};

int64_t Now() noexcept
{
    return Clock::now().time_since_epoch().count();
}

bool DestroyedLately(uintptr_t aWorld) noexcept
{
    const int64_t cutoff = Now() - std::chrono::duration_cast<Clock::duration>(kDestroyedMemory).count();
    for (size_t i = 0; i < kDestroyedSlots; ++i)
    {
        if (s_destroyedWorlds[i].load(std::memory_order_acquire) == aWorld && s_destroyedAt[i].load(std::memory_order_relaxed) >= cutoff)
            return true;
    }
    return false;
}

// The call stack as game offsets ("+1d7a1c9 ..."), others as "-"
std::string GameStack(int aSkip) noexcept
{
    void* frames[16]{};
    const USHORT count = RtlCaptureStackBackTrace(static_cast<DWORD>(aSkip), static_cast<DWORD>(std::size(frames)), frames, nullptr);
    std::string text;
    for (USHORT i = 0; i < count; ++i)
    {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        if (address >= kGameBase && address < kGameEnd)
            text += fmt::format(" +{:x}", address - kGameBase);
        else
            text += " -";
    }
    return text;
}

// hknpBSWorld's destructor (slot 0 of its vtable, true id 231167, scalar deleting: this, flags)
using TWorldDestructor = void*(void*, uint32_t);
TWorldDestructor* RealWorldDestructor = nullptr;

void* HookWorldDestructor(void* apWorld, uint32_t aFlags)
{
    const auto world = reinterpret_cast<uintptr_t>(apWorld);
    const size_t slot = s_nextSlot.fetch_add(1, std::memory_order_relaxed) % kDestroyedSlots;
    s_destroyedAt[slot].store(Now(), std::memory_order_relaxed);
    s_destroyedWorlds[slot].store(world, std::memory_order_release);

    uint32_t cellId = 0;
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
    {
        if (TESObjectCELL* pCell = pPlayer->parentCell)
            cellId = pCell->formID;
    }
    spdlog::info("Havok: world {:X} destroyed on thread {}, the player in cell {:X}, from{}", world, GetCurrentThreadId(), cellId, GameStack(1));

    return RealWorldDestructor(apWorld, aFlags);
}

// hkbnpRagdollInterface's constraint pass (true id 1142510): its world at [[this + 0x10] + 0x18], the constraints it
// goes through at [this + 0x18] (an array, its count at + 0x38), the world's constraint storage at + 0x128
using TRagdollConstraints = void(uint8_t*);
TRagdollConstraints* RealRagdollConstraints = nullptr;

void HookRagdollConstraints(uint8_t* apRagdoll)
{
    const uint8_t* pOwner = apRagdoll ? *reinterpret_cast<uint8_t* const*>(apRagdoll + 0x10) : nullptr;
    const uint8_t* pWorld = pOwner ? *reinterpret_cast<uint8_t* const*>(pOwner + 0x18) : nullptr;
    const uint8_t* pList = apRagdoll ? *reinterpret_cast<uint8_t* const*>(apRagdoll + 0x18) : nullptr;
    const int32_t count = pList ? *reinterpret_cast<const int32_t*>(pList + 0x38) : 0;
    if (pWorld && count > 0)
    {
        const bool destroyed = DestroyedLately(reinterpret_cast<uintptr_t>(pWorld));
        const bool noConstraints = *reinterpret_cast<const void* const*>(pWorld + 0x128) == nullptr;
        if (destroyed || noConstraints)
        {
            static std::atomic<int> s_reports{0};
            if (s_reports.fetch_add(1, std::memory_order_relaxed) < 20)
                spdlog::warn("Havok: a ragdoll ({:X}) left alone, its world {:X} {} ({} constraints), thread {}", reinterpret_cast<uintptr_t>(apRagdoll),
                             reinterpret_cast<uintptr_t>(pWorld), destroyed ? "was destroyed moments ago" : "has no constraints", count, GetCurrentThreadId());
            return;
        }
    }
    RealRagdollConstraints(apRagdoll);
}

// A collision query's check of a hit (true id 1078096: whether the body hit is on layer 11) read a body of a world freed
// 2 ms before, on a worker thread (seen 2026-10-15 riding fast through Sanctuary: the exterior's world destroyed from
// +1d6ce42, which also happens in normal play, then this check faulting on a body pointer from the freed world). It only
// reads before that point: a fault there is answered "not that layer", which a hit in a world that's gone is anyway.
using THitCheck = uint32_t(void*, void*);
THitCheck* RealHitCheck = nullptr;
std::atomic<int> s_hitCheckFaults{0};

uint32_t CallHitCheck(void* apQuery, void* apHit) noexcept
{
    __try
    {
        return RealHitCheck(apQuery, apHit);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        s_hitCheckFaults.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
}

uint32_t HookHitCheck(void* apQuery, void* apHit)
{
    const int faultsBefore = s_hitCheckFaults.load(std::memory_order_relaxed);
    const uint32_t result = CallHitCheck(apQuery, apHit);
    if (s_hitCheckFaults.load(std::memory_order_relaxed) != faultsBefore && faultsBefore < 20)
        spdlog::warn("Havok: a hit check read a freed physics world, answered no (thread {})", GetCurrentThreadId());
    return result;
}

TiltedPhoques::Initializer s_havokWorldGuard(
    []()
    {
        POINTER_FALLOUT4(TWorldDestructor, s_worldDestructor, 231168);
        RealWorldDestructor = s_worldDestructor.Get();
        TP_HOOK(&RealWorldDestructor, HookWorldDestructor);

        POINTER_FALLOUT4(TRagdollConstraints, s_ragdollConstraints, 1142511);
        RealRagdollConstraints = s_ragdollConstraints.Get();
        TP_HOOK(&RealRagdollConstraints, HookRagdollConstraints);

        POINTER_FALLOUT4(THitCheck, s_hitCheck, 1078097);
        RealHitCheck = s_hitCheck.Get();
        TP_HOOK(&RealHitCheck, HookHitCheck);
    });
} // namespace

#include <ShadowFeatures.h>

#include <cstring>
#include <thread>

// More visibility workers. With bDoMultithreadedVisQuery on, the game splits its Umbra visibility query into 4 jobs (and the
// shadow caster query that follows into 5). Umbra takes any count from 1 to 16: each job is told which part of the count it
// is. The number is hard-coded in the code that allocates the jobs' working memory, starts them, waits and merges, so it's
// changed there, at startup: a change needs a restart. Same result, sooner.

namespace
{
constexpr uint32_t kKick = 1276849;           // starts the visibility query, once a frame
constexpr uint32_t kWaitForResults = 1264354; // the main thread waits for the query's results and takes them
// Where the kick is in 1.10.163; everything patched is found from it
constexpr uint32_t kKickRva = 0x27AE2F0;

// One query job's working memory, its thread number; the shadow caster query's
constexpr size_t kJobSize = 0x19220;
constexpr size_t kJobThread = 0x19214;
constexpr size_t kShadowJobSize = 0x19038;
constexpr size_t kShadowJobThread = 0x19034;
// The manager: its job lists and the jobs
constexpr size_t kManagerList = 0x48;
constexpr size_t kManagerShadowList = 0x58;
constexpr size_t kManagerJobs = 0x108;
constexpr size_t kManagerShadowJobs = 0x110;
// A job list: how many pool threads it wakes
constexpr size_t kListThreads = 0xC4;

// What's patched: where, and where in the instruction the number is
struct Patch
{
    uint32_t Rva;
    uint8_t ValueOffset;
    uint8_t ValueSize;
    uint32_t Expected;
    enum Kind : uint8_t
    {
        Count,      // the count itself
        CountLess1, // the count less one
        Allocation, // 8 + count * the job's size
    } What;
    bool Shadow; // of the shadow caster query, or the main query
};

constexpr Patch kPatches[] = {
    {0x27ADA23, 1, 4, 0x64888, Patch::Allocation, false}, // mov edx,64888h: the jobs' memory
    {0x27ADA36, 2, 4, 4, Patch::Count, false},            // mov dword ptr [rax],4: their count, for the destructor
    {0x27ADA3C, 1, 4, 3, Patch::CountLess1, false},       // mov esi,3: the constructors' loop
    {0x27AE383, 3, 1, 4, Patch::Count, false},            // lea ebp,[r14+4]: starting them, on a tome
    {0x27AE423, 3, 1, 4, Patch::Count, false},            // lea ebp,[r14+4]: starting them, on a tome collection
    {0x27AE6D6, 1, 4, 3, Patch::CountLess1, false},       // mov edi,3: merging 1 to the last into 0
    {0x27B5B16, 4, 4, 4, Patch::Count, false},            // mov [rsp+40h],4: what each job tells Umbra
    {0x27ADB12, 1, 4, 0x7D120, Patch::Allocation, true},  // mov edx,7D120h
    {0x27ADB25, 2, 4, 5, Patch::Count, true},             // mov dword ptr [rax],5
    {0x27ADB2B, 1, 4, 4, Patch::CountLess1, true},        // mov esi,4
    {0x27AEA79, 2, 1, 5, Patch::Count, true},             // cmp ebx,5: setting up and starting
    {0x27AEDC8, 2, 1, 5, Patch::Count, true},             // cmp ebx,5
    {0x27AEC24, 3, 1, 5, Patch::Count, true},             // cmp r12d,5: taking the results
    {0x27AEF78, 3, 1, 5, Patch::Count, true},             // cmp r12d,5
    {0x27AEAB3, 4, 4, 5, Patch::Count, true},             // mov [rsp+28h],5: the part the main thread does itself
    {0x27AEE02, 4, 4, 5, Patch::Count, true},             // mov [rsp+28h],5
    {0x27AFE94, 4, 4, 5, Patch::Count, true},             // mov [rsp+28h],5: what each job tells Umbra
};

using TKick = void(void* apManager, void* apCamera);
using TWaitForResults = void();
TKick* RealKick = nullptr;
TWaitForResults* RealWaitForResults = nullptr;

uint32_t s_jobs = 4;
uint32_t s_shadowJobs = 5;
bool s_threadsSet = false;

LARGE_INTEGER s_frequency{};
uint64_t s_waitTicks = 0;
uint32_t s_waits = 0;

template <class T> T& At(void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(apObject) + aOffset);
}

uint32_t PatchedValue(const Patch& acPatch) noexcept
{
    const uint32_t count = acPatch.Shadow ? s_shadowJobs : s_jobs;
    switch (acPatch.What)
    {
    case Patch::Count: return count;
    case Patch::CountLess1: return count - 1;
    default: return static_cast<uint32_t>(8 + count * (acPatch.Shadow ? kShadowJobSize : kJobSize));
    }
}

void HookKick(void* apManager, void* apCamera)
{
    if (!s_threadsSet)
    {
        s_threadsSet = true;
        // The constructor numbers jobs 0 to 3 (and 0 to 4); the rest get theirs here, and the lists wake that many threads
        if (auto* pJobs = At<uint8_t*>(apManager, kManagerJobs))
        {
            for (uint32_t i = 4; i < s_jobs; ++i)
                At<int32_t>(pJobs + i * kJobSize, kJobThread) = static_cast<int32_t>(i);
        }
        if (auto* pJobs = At<uint8_t*>(apManager, kManagerShadowJobs))
        {
            for (uint32_t i = 5; i < s_shadowJobs; ++i)
                At<int32_t>(pJobs + i * kShadowJobSize, kShadowJobThread) = static_cast<int32_t>(i);
        }
        if (auto** pHolder = At<void**>(apManager, kManagerList); pHolder && *pHolder)
            At<uint32_t>(*pHolder, kListThreads) = s_jobs;
        if (auto** pHolder = At<void**>(apManager, kManagerShadowList); pHolder && *pHolder)
            At<uint32_t>(*pHolder, kListThreads) = s_shadowJobs;
        spdlog::info("Visibility workers: {} jobs for the visibility query, {} for the shadow caster query", s_jobs, s_shadowJobs);
    }
    RealKick(apManager, apCamera);
}

void HookWaitForResults()
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    RealWaitForResults();
    QueryPerformanceCounter(&end);
    s_waitTicks += static_cast<uint64_t>(end.QuadPart - start.QuadPart);
    ++s_waits;
}
} // namespace

void VisibilityWorkers::Report(double) noexcept
{
    if (s_waits)
        spdlog::info("Visibility query ({} jobs): the main thread waits {:.2f} ms a frame for the results", s_jobs,
                     static_cast<double>(s_waitTicks) * 1000.0 / static_cast<double>(s_frequency.QuadPart) / s_waits);
    s_waitTicks = 0;
    s_waits = 0;
}

static TiltedPhoques::Initializer s_visibilityWorkersHooks(
    []()
    {
        QueryPerformanceFrequency(&s_frequency);

        // Measured whatever the setting
        static constexpr uint8_t kWaitStart[] = {0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0xE8};
        if (ShadowFeatures::Matches("Visibility workers", "the wait for results", kWaitForResults, kWaitStart, sizeof(kWaitStart)))
        {
            const VersionDbPtr<uint8_t> wait(kWaitForResults);
            RealWaitForResults = reinterpret_cast<TWaitForResults*>(wait.Get());
            TP_HOOK(&RealWaitForResults, HookWaitForResults);
        }

        // 0: the game's own 4. Otherwise 6, 8 or 12, never more than the machine's threads less two (the main thread, the
        // rendering's other work); Umbra takes up to 16.
        constexpr uint32_t kChoices[] = {0, 6, 8, 12};
        const uint32_t choice = ShadowFeatures::ReadSetting(L"iVisibilityWorkers", 0);
        uint32_t wanted = choice < std::size(kChoices) ? kChoices[choice] : 0;
        if (wanted == 0)
            return;
        const uint32_t threads = std::thread::hardware_concurrency();
        if (threads > 2)
            wanted = std::min(wanted, threads - 2);
        if (wanted <= 4)
        {
            spdlog::info("Visibility workers: this machine has {} threads, the game's own 4 jobs stay", threads);
            return;
        }

        static constexpr uint8_t kKickStart[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC};
        if (!ShadowFeatures::Matches("Visibility workers", "the query's start", kKick, kKickStart, sizeof(kKickStart)))
            return;
        const VersionDbPtr<uint8_t> kick(kKick);

        // Every number has to say this is the code they were read from
        uint8_t* pBase = kick.Get() - kKickRva;
        for (const Patch& patch : kPatches)
        {
            uint32_t value = 0;
            std::memcpy(&value, pBase + patch.Rva + patch.ValueOffset, patch.ValueSize);
            if (value != patch.Expected)
            {
                spdlog::warn("Visibility workers: off, the code at +{:X} isn't what was read from 1.10.163 (has {:X}, expected {:X})", patch.Rva, value, patch.Expected);
                return;
            }
        }

        s_jobs = wanted;
        s_shadowJobs = wanted + 1;
        for (const Patch& patch : kPatches)
        {
            const uint32_t value = PatchedValue(patch);
            uint8_t* pTarget = pBase + patch.Rva + patch.ValueOffset;
            if (patch.ValueSize == 1)
                TiltedPhoques::Put<uint8_t>(pTarget, static_cast<uint8_t>(value));
            else
                TiltedPhoques::Put<uint32_t>(pTarget, value);
        }
        RealKick = reinterpret_cast<TKick*>(kick.Get());
        TP_HOOK(&RealKick, HookKick);
        spdlog::info("Visibility workers: patched for {} jobs (the game's own: 4 and 5)", wanted);
    });

#include "PerfSampler.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
const bool s_enabled = std::getenv("RD_PROFILE") != nullptr;

// Samples a second (the timer's period, ms), stack walked at most this deep, the stack copied at most this much (bytes)
// plus zeros after it, so an unwind that reads past what was copied stays inside our buffer
constexpr DWORD kPeriodMs = 1;
constexpr int kMaxDepth = 48;
constexpr size_t kStackCopy = 192 * 1024;
constexpr size_t kStackSlack = 64 * 1024;
constexpr int64_t kReportSeconds = 10;
// A frame is slow from this much over the window's median
constexpr double kSlowRatio = 1.25;
constexpr double kSlowExtraMs = 2.0;
// Functions per list in the log
constexpr size_t kTopAll = 30;
constexpr size_t kTopDiff = 20;
constexpr size_t kTopModules = 12;
constexpr size_t kTopWaits = 10;
// Other threads: one is sampled each tick besides the main thread, in turn; those busy at least this share of their
// samples get a line, the busiest first, at most this many
constexpr double kBusyThreadShare = 0.03;
constexpr size_t kMaxThreadLines = 16;
constexpr size_t kTopPerThread = 8;

struct FrameRecord
{
    uint32_t Index;
    float Ms;
    float CellMs;
};

std::atomic<uint32_t> s_frameIndex{0};
std::atomic<uint64_t> s_cellNs{0};
std::mutex s_frameLock;
std::vector<FrameRecord> s_frames;

// A sampled thread: its stack's reserved bottom and its top, and where it started (a key, for naming it). The main
// thread is the first; the rest are found again every report (sampler thread only).
struct Thread
{
    DWORD Id = 0;
    HANDLE Handle = nullptr;
    uint64_t StackLow = 0;
    uint64_t StackBase = 0;
    uint64_t Start = 0;
    bool Seen = false;
};
std::vector<Thread> s_threads;
DWORD s_samplerId = 0;
size_t s_nextOther = 1;

// The system calls a thread waits in (ntdll's and win32u's stubs): a sample whose top is in one is waiting, not working
std::vector<std::pair<uint64_t, uint64_t>> s_waitStubs;

using TNtQueryInformationThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
TNtQueryInformationThread s_queryThread = nullptr;

// The game's image (mapped into the launcher at its own base, its headers copied over the launcher's) and its function
// table: the loader's lookup only knows the launcher's, whose code comes after the game's image
uint64_t s_gameBase = 0;
uint64_t s_gameSize = 0;
const RUNTIME_FUNCTION* s_gameFunctions = nullptr;
size_t s_gameFunctionCount = 0;

// Sampler thread only from here
std::vector<uint8_t> s_stack;

// A walked frame: module index in the high 32 bits, offset into it in the low; bit 31 of the offset set when it's the
// address itself (no function entry found) rather than the function's start
using Key = uint64_t;
constexpr uint32_t kAddressFlag = 0x80000000u;

struct Sample
{
    uint32_t Frame;
    uint32_t First;
    uint16_t Depth;
    uint16_t Thread;
    bool Waiting;
};
std::vector<Sample> s_samples;
std::vector<Key> s_keys;

struct Module
{
    uint64_t Base;
    std::string Name;
};
std::vector<Module> s_modules{{0, "?"}};
std::unordered_map<uint64_t, uint32_t> s_moduleByBase;
uint32_t s_gameModule = 0;
uint32_t s_ourModule = 0;

uint32_t ModuleFor(uint64_t aBase) noexcept
{
    if (const auto it = s_moduleByBase.find(aBase); it != s_moduleByBase.end())
        return it->second;

    std::string name = "?";
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(reinterpret_cast<HMODULE>(aBase), path, MAX_PATH))
    {
        const wchar_t* pName = wcsrchr(path, L'\\');
        pName = pName ? pName + 1 : path;
        char narrow[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, pName, -1, narrow, MAX_PATH, nullptr, nullptr);
        name = narrow;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    const auto index = static_cast<uint32_t>(s_modules.size());
    s_modules.push_back({aBase, name});
    s_moduleByBase.emplace(aBase, index);
    return index;
}

const RUNTIME_FUNCTION* FindGameFunction(uint64_t aPc) noexcept
{
    const auto rva = static_cast<DWORD>(aPc - s_gameBase);
    size_t low = 0;
    size_t high = s_gameFunctionCount;
    while (low < high)
    {
        const size_t mid = (low + high) / 2;
        const RUNTIME_FUNCTION& function = s_gameFunctions[mid];
        if (rva < function.BeginAddress)
            high = mid;
        else if (rva >= function.EndAddress)
            low = mid + 1;
        else
            return &function;
    }
    return nullptr;
}

// The function a (possibly chained) entry belongs to: chained parts of a function point back to the entry before them
const RUNTIME_FUNCTION* PrimaryEntry(uint64_t aImageBase, const RUNTIME_FUNCTION* apEntry) noexcept
{
    for (int i = 0; i < 32; ++i)
    {
        const auto* pInfo = reinterpret_cast<const uint8_t*>(aImageBase + apEntry->UnwindData);
        if (!((pInfo[0] >> 3) & UNW_FLAG_CHAININFO))
            break;
        apEntry = reinterpret_cast<const RUNTIME_FUNCTION*>(pInfo + 4 + ((pInfo[2] + 1) & ~1) * 2);
    }
    return apEntry;
}

// How far above its base (the stack pointer, or the frame register less its offset) unwinding a frame can read, and its
// frame register; false for unwind data we don't know
struct FrameReach
{
    uint32_t Bytes = 0;
    int FrameRegister = 0;
    uint32_t FrameOffset = 0;
};

bool Reach(uint64_t aImageBase, const RUNTIME_FUNCTION* apEntry, FrameReach& aReach) noexcept
{
    uint32_t pushed = 0;
    uint32_t saved = 0;
    for (int chain = 0; chain < 32; ++chain)
    {
        const auto* pInfo = reinterpret_cast<const uint8_t*>(aImageBase + apEntry->UnwindData);
        const uint8_t flags = pInfo[0] >> 3;
        const uint8_t count = pInfo[2];
        if (chain == 0 && (pInfo[3] & 0xF))
        {
            aReach.FrameRegister = pInfo[3] & 0xF;
            aReach.FrameOffset = (pInfo[3] >> 4) * 16u;
        }

        const auto* pCodes = reinterpret_cast<const uint16_t*>(pInfo + 4);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t op = (pCodes[i] >> 8) & 0xF;
            const uint8_t opInfo = (pCodes[i] >> 12) & 0xF;
            switch (op)
            {
            case 0: pushed += 8; break; // push nonvolatile
            case 1:                     // large allocation
                if (opInfo == 0 && i + 1 < count)
                {
                    pushed += pCodes[i + 1] * 8u;
                    i += 1;
                }
                else if (i + 2 < count)
                {
                    pushed += pCodes[i + 1] | (static_cast<uint32_t>(pCodes[i + 2]) << 16);
                    i += 2;
                }
                else
                    return false;
                break;
            case 2: pushed += opInfo * 8u + 8u; break; // small allocation
            case 3: break;                             // frame register set
            case 4:                                    // nonvolatile saved
                if (i + 1 >= count)
                    return false;
                saved = std::max(saved, pCodes[i + 1] * 8u + 8u);
                i += 1;
                break;
            case 5:
                if (i + 2 >= count)
                    return false;
                saved = std::max(saved, (pCodes[i + 1] | (static_cast<uint32_t>(pCodes[i + 2]) << 16)) + 8u);
                i += 2;
                break;
            case 6: break; // epilog (version 2)
            case 7: i += 1; break;
            case 8: // xmm saved
                if (i + 1 >= count)
                    return false;
                saved = std::max(saved, pCodes[i + 1] * 16u + 16u);
                i += 1;
                break;
            case 9:
                if (i + 2 >= count)
                    return false;
                saved = std::max(saved, (pCodes[i + 1] | (static_cast<uint32_t>(pCodes[i + 2]) << 16)) + 16u);
                i += 2;
                break;
            case 10: pushed += 48; break; // machine frame
            default: return false;
            }
        }

        if (!(flags & UNW_FLAG_CHAININFO))
            break;
        apEntry = reinterpret_cast<const RUNTIME_FUNCTION*>(pInfo + 4 + ((count + 1) & ~1) * 2);
    }
    // the return address, and some to spare
    aReach.Bytes = std::max(pushed, saved) + 8u + 64u;
    return true;
}

// Walks the copied stack (copied from aRealRsp, aCopied bytes) from aContext, adding a key per frame
uint16_t Walk(CONTEXT& aContext, uint64_t aRealRsp, size_t aCopied) noexcept
{
    const uint64_t copyStart = reinterpret_cast<uint64_t>(s_stack.data());
    const uint64_t copyEnd = copyStart + s_stack.size();
    const uint64_t realEnd = aRealRsp + aCopied;
    DWORD64* pRegisters = &aContext.Rax;

    // Registers pointing into the real stack point into the copy instead
    auto translate = [&]()
    {
        for (int r = 0; r < 16; ++r)
        {
            if (pRegisters[r] >= aRealRsp && pRegisters[r] < realEnd)
                pRegisters[r] = pRegisters[r] - aRealRsp + copyStart;
        }
    };
    auto inCopy = [&](uint64_t aAddress, uint64_t aBytes) { return aAddress >= copyStart && aAddress + aBytes <= copyEnd && aAddress + aBytes >= aAddress; };

    translate();
    uint16_t depth = 0;
    while (depth < kMaxDepth && aContext.Rip)
    {
        const uint64_t pc = aContext.Rip;
        uint64_t imageBase = 0;
        const RUNTIME_FUNCTION* pEntry = nullptr;
        if (pc >= s_gameBase && pc < s_gameBase + s_gameSize)
        {
            imageBase = s_gameBase;
            pEntry = FindGameFunction(pc);
        }
        else
            pEntry = RtlLookupFunctionEntry(pc, &imageBase, nullptr);

        uint32_t module = 0;
        uint64_t offset = pc;
        if (imageBase)
        {
            module = imageBase == s_gameBase ? (pc - s_gameBase < s_gameSize ? s_gameModule : s_ourModule) : ModuleFor(imageBase);
            offset = pc - imageBase;
        }
        else
        {
            HMODULE hModule = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(pc), &hModule) && hModule)
            {
                imageBase = reinterpret_cast<uint64_t>(hModule);
                module = imageBase == s_gameBase ? (pc - s_gameBase < s_gameSize ? s_gameModule : s_ourModule) : ModuleFor(imageBase);
                offset = pc - imageBase;
            }
        }
        const uint32_t start = pEntry ? PrimaryEntry(imageBase, pEntry)->BeginAddress : static_cast<uint32_t>(offset & ~uint64_t(kAddressFlag)) | kAddressFlag;
        s_keys.push_back((static_cast<Key>(module) << 32) | start);
        ++depth;

        // Code we can't place (a hook's trampoline, say) has no frame we can know
        if (!imageBase)
            break;

        if (!pEntry)
        {
            // A leaf: the return address is on top
            if (!inCopy(aContext.Rsp, 8))
                break;
            aContext.Rip = *reinterpret_cast<const uint64_t*>(aContext.Rsp);
            aContext.Rsp += 8;
        }
        else
        {
            FrameReach reach;
            if (!Reach(imageBase, pEntry, reach) || !inCopy(aContext.Rsp, reach.Bytes))
                break;
            if (reach.FrameRegister && reach.FrameRegister != 4)
            {
                const uint64_t frameBase = pRegisters[reach.FrameRegister] - reach.FrameOffset;
                if (!inCopy(frameBase, reach.Bytes))
                    break;
            }

            const uint64_t before = aContext.Rsp;
            void* pHandlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, const_cast<PRUNTIME_FUNCTION>(pEntry), &aContext, &pHandlerData, &establisher, nullptr);
            if (aContext.Rsp <= before || !inCopy(aContext.Rsp, 0))
                break;
        }
        translate();
    }
    return depth;
}

bool InWaitStub(uint64_t aPc) noexcept
{
    for (const auto& [begin, end] : s_waitStubs)
    {
        if (aPc >= begin && aPc < end)
            return true;
    }
    return false;
}

void TakeSample(size_t aThread) noexcept
{
    const Thread& thread = s_threads[aThread];
    const uint32_t frame = s_frameIndex.load(std::memory_order_relaxed);

    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(thread.Handle) == static_cast<DWORD>(-1))
        return;
    size_t copied = 0;
    // Nothing between suspending and resuming may take a lock the thread could hold: no allocation, no logging. Only its
    // own stack is copied (from the stack pointer up, all of it in use, so committed).
    if (GetThreadContext(thread.Handle, &context) && context.Rsp >= thread.StackLow && context.Rsp < thread.StackBase)
    {
        copied = std::min<size_t>(thread.StackBase - context.Rsp, kStackCopy);
        std::memcpy(s_stack.data(), reinterpret_cast<const void*>(context.Rsp), copied);
    }
    ResumeThread(thread.Handle);
    if (!copied)
        return;

    std::memset(s_stack.data() + copied, 0, std::min(s_stack.size() - copied, kStackSlack));
    const bool waiting = InWaitStub(context.Rip);
    const auto first = static_cast<uint32_t>(s_keys.size());
    const uint16_t depth = Walk(context, context.Rsp, copied);
    if (depth)
        s_samples.push_back({frame, first, depth, static_cast<uint16_t>(aThread), waiting});
}

// A thread's stack: its top (StackBase, at 8 in the TEB) and the bottom of what's reserved for it (DeallocationStack, at
// 0x1478). Read while it's suspended, so it can't exit meanwhile and take its TEB with it.
bool ReadStack(Thread& aThread) noexcept
{
    struct BasicInformation
    {
        LONG ExitStatus;
        PVOID TebBaseAddress;
        PVOID UniqueProcess;
        PVOID UniqueThread;
        ULONG_PTR AffinityMask;
        LONG Priority;
        LONG BasePriority;
    } info{};
    if (!s_queryThread || s_queryThread(aThread.Handle, 0, &info, sizeof(info), nullptr) < 0 || !info.TebBaseAddress)
        return false;
    if (SuspendThread(aThread.Handle) == static_cast<DWORD>(-1))
        return false;
    const auto* pTeb = static_cast<const uint8_t*>(info.TebBaseAddress);
    aThread.StackBase = *reinterpret_cast<const uint64_t*>(pTeb + 8);
    aThread.StackLow = *reinterpret_cast<const uint64_t*>(pTeb + 0x1478);
    ResumeThread(aThread.Handle);

    void* pStart = nullptr;
    s_queryThread(aThread.Handle, 9, &pStart, sizeof(pStart), nullptr);
    aThread.Start = reinterpret_cast<uint64_t>(pStart);
    return aThread.StackBase > aThread.StackLow;
}

// The process's threads, besides the main one and the sampler: new ones opened, gone ones closed
void RefreshThreads() noexcept
{
    for (size_t i = 1; i < s_threads.size(); ++i)
        s_threads[i].Seen = false;

    const HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{sizeof(entry)};
    const DWORD process = GetCurrentProcessId();
    for (BOOL more = Thread32First(hSnapshot, &entry); more; more = Thread32Next(hSnapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == s_samplerId || entry.th32ThreadID == s_threads[0].Id)
            continue;
        auto it = std::find_if(s_threads.begin() + 1, s_threads.end(), [&](const Thread& t) { return t.Id == entry.th32ThreadID; });
        if (it != s_threads.end())
        {
            it->Seen = true;
            continue;
        }
        Thread thread;
        thread.Id = entry.th32ThreadID;
        thread.Handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread.Id);
        if (!thread.Handle)
            continue;
        if (!ReadStack(thread))
        {
            CloseHandle(thread.Handle);
            continue;
        }
        thread.Seen = true;
        s_threads.push_back(thread);
    }
    CloseHandle(hSnapshot);

    // Samples name their thread by index: closed ones keep their slot until the next report has counted them
    for (size_t i = 1; i < s_threads.size(); ++i)
    {
        if (!s_threads[i].Seen && s_threads[i].Handle)
        {
            CloseHandle(s_threads[i].Handle);
            s_threads[i].Handle = nullptr;
        }
    }
}

void FindWaitStubs() noexcept
{
    const char* ntdll[] = {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution", "NtRemoveIoCompletion", "NtRemoveIoCompletionEx",
                           "NtWaitForAlertByThreadId", "NtWaitForWorkViaWorkerFactory", "NtSignalAndWaitForSingleObject", "NtYieldExecution", "NtWaitForKeyedEvent",
                           "NtReplyWaitReceivePort", "NtWaitForDebugEvent"};
    const char* win32u[] = {"NtUserMsgWaitForMultipleObjectsEx", "NtUserWaitMessage", "NtUserGetMessage"};
    auto add = [](const wchar_t* acpModule, const char* const* acpNames, size_t aCount)
    {
        const HMODULE hModule = GetModuleHandleW(acpModule);
        if (!hModule)
            return;
        for (size_t i = 0; i < aCount; ++i)
        {
            // a stub is a few instructions around its syscall; 0x20 covers it
            if (const auto address = reinterpret_cast<uint64_t>(GetProcAddress(hModule, acpNames[i])))
                s_waitStubs.emplace_back(address, address + 0x20);
        }
    };
    add(L"ntdll.dll", ntdll, std::size(ntdll));
    add(L"win32u.dll", win32u, std::size(win32u));
}

std::string KeyName(Key aKey)
{
    const auto module = static_cast<uint32_t>(aKey >> 32);
    const auto offset = static_cast<uint32_t>(aKey);
    char text[96];
    std::snprintf(text, sizeof(text), "%s+%X%s", module < s_modules.size() ? s_modules[module].Name.c_str() : "?", offset & ~kAddressFlag, (offset & kAddressFlag) ? "@" : "");
    return text;
}

struct Group
{
    uint32_t Frames = 0;
    std::unordered_map<Key, uint32_t> Inclusive;
    std::unordered_map<Key, uint32_t> Own;
};

void Count(Group& aGroup, const Sample& acSample, std::vector<Key>& aSeen)
{
    aSeen.clear();
    for (uint16_t i = 0; i < acSample.Depth; ++i)
    {
        const Key key = s_keys[acSample.First + i];
        if (std::find(aSeen.begin(), aSeen.end(), key) != aSeen.end())
            continue;
        aSeen.push_back(key);
        ++aGroup.Inclusive[key];
    }
    ++aGroup.Own[s_keys[acSample.First]];
}

// "name a/b" per function, ms per frame, inclusive then own, the biggest inclusive first
std::string ListAll(const Group& acGroup, double aMsPerSample)
{
    std::vector<std::pair<Key, uint32_t>> entries(acGroup.Inclusive.begin(), acGroup.Inclusive.end());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const double perFrame = aMsPerSample / std::max<uint32_t>(acGroup.Frames, 1);
    std::string out;
    char text[64];
    for (size_t i = 0; i < entries.size() && i < kTopAll; ++i)
    {
        const auto own = acGroup.Own.find(entries[i].first);
        std::snprintf(text, sizeof(text), " %.2f/%.2f", entries[i].second * perFrame, own != acGroup.Own.end() ? own->second * perFrame : 0.0);
        out += " " + KeyName(entries[i].first) + text;
    }
    return out;
}

std::string ListOwn(const Group& acGroup, double aMsPerSample)
{
    std::vector<std::pair<Key, uint32_t>> entries(acGroup.Own.begin(), acGroup.Own.end());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const double perFrame = aMsPerSample / std::max<uint32_t>(acGroup.Frames, 1);
    std::string out;
    char text[32];
    for (size_t i = 0; i < entries.size() && i < kTopAll; ++i)
    {
        std::snprintf(text, sizeof(text), " %.2f", entries[i].second * perFrame);
        out += " " + KeyName(entries[i].first) + text;
    }
    return out;
}

// What grows in aGroup against aRest: "name +d (a vs b)" in inclusive ms per frame, the biggest growth first
std::string ListDiff(const Group& acGroup, const Group& acRest, double aMsPerSample)
{
    const double perFrame = aMsPerSample / std::max<uint32_t>(acGroup.Frames, 1);
    const double perRestFrame = aMsPerSample / std::max<uint32_t>(acRest.Frames, 1);
    std::vector<std::tuple<double, double, double, Key>> entries;
    for (const auto& [key, count] : acGroup.Inclusive)
    {
        const auto rest = acRest.Inclusive.find(key);
        const double a = count * perFrame;
        const double b = rest != acRest.Inclusive.end() ? rest->second * perRestFrame : 0.0;
        entries.emplace_back(a - b, a, b, key);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::string out;
    char text[64];
    for (size_t i = 0; i < entries.size() && i < kTopDiff; ++i)
    {
        std::snprintf(text, sizeof(text), " +%.2f (%.2f vs %.2f)", std::get<0>(entries[i]), std::get<1>(entries[i]), std::get<2>(entries[i]));
        out += " " + KeyName(std::get<3>(entries[i])) + text;
    }
    return out;
}

void Report(double aSeconds, uint32_t aSamplesTaken)
{
    std::vector<FrameRecord> frames;
    {
        std::scoped_lock _(s_frameLock);
        frames.swap(s_frames);
    }
    if (frames.empty())
    {
        s_samples.clear();
        s_keys.clear();
        return;
    }

    const uint32_t firstFrame = frames.front().Index;
    const uint32_t lastFrame = frames.back().Index;
    std::vector<float> lengths;
    lengths.reserve(frames.size());
    uint32_t cellFrames = 0;
    double cellMs = 0.0;
    for (const FrameRecord& record : frames)
    {
        lengths.push_back(record.Ms);
        if (record.CellMs > 0.0f)
        {
            ++cellFrames;
            cellMs += record.CellMs;
        }
    }
    std::nth_element(lengths.begin(), lengths.begin() + lengths.size() / 2, lengths.end());
    const double median = lengths[lengths.size() / 2];
    const double slowFrom = std::max(median * kSlowRatio, median + kSlowExtraMs);

    // index -> record (indices are consecutive)
    auto recordOf = [&](uint32_t aIndex) -> const FrameRecord*
    {
        if (aIndex < firstFrame || aIndex > lastFrame)
            return nullptr;
        const FrameRecord& record = frames[aIndex - firstFrame];
        return record.Index == aIndex ? &record : nullptr;
    };

    Group all;
    Group slow;
    Group rest;
    Group cells;
    Group noCells;
    double slowMs = 0.0;
    double totalMs = 0.0;
    for (const FrameRecord& record : frames)
    {
        ++all.Frames;
        totalMs += record.Ms;
        if (record.Ms >= slowFrom)
        {
            ++slow.Frames;
            slowMs += record.Ms;
        }
        else
            ++rest.Frames;
        ++(record.CellMs > 0.0f ? cells : noCells).Frames;
    }

    std::vector<Sample> pending;
    std::vector<Key> pendingKeys;
    std::vector<Key> seen;
    uint32_t walkedSamples = 0;
    // Where the main thread waits: the first frame below the wait that isn't the system's
    std::unordered_map<Key, uint32_t> waitSites;
    // Other threads: their samples, the waiting ones, and what they do
    struct Other
    {
        uint32_t Samples = 0;
        uint32_t Waiting = 0;
        Group Busy;
    };
    std::unordered_map<uint16_t, Other> others;
    auto isSystem = [](Key aKey)
    {
        const auto module = static_cast<uint32_t>(aKey >> 32);
        if (module >= s_modules.size())
            return true;
        const std::string& name = s_modules[module].Name;
        return name == "ntdll.dll" || name == "kernelbase.dll" || name == "kernel32.dll" || name == "win32u.dll" || name == "user32.dll";
    };
    for (const Sample& sample : s_samples)
    {
        if (sample.Thread != 0)
        {
            Other& other = others[sample.Thread];
            ++other.Samples;
            if (sample.Waiting)
                ++other.Waiting;
            else
                Count(other.Busy, sample, seen);
            continue;
        }
        if (sample.Frame > lastFrame)
        {
            // its frame isn't over yet: next window's
            Sample moved = sample;
            moved.First = static_cast<uint32_t>(pendingKeys.size());
            pending.push_back(moved);
            pendingKeys.insert(pendingKeys.end(), s_keys.begin() + sample.First, s_keys.begin() + sample.First + sample.Depth);
            continue;
        }
        const FrameRecord* pRecord = recordOf(sample.Frame);
        if (!pRecord)
            continue;
        ++walkedSamples;
        if (sample.Waiting)
        {
            for (uint16_t i = 0; i < sample.Depth; ++i)
            {
                const Key key = s_keys[sample.First + i];
                if (!isSystem(key))
                {
                    ++waitSites[key];
                    break;
                }
            }
        }
        Count(all, sample, seen);
        Count(pRecord->Ms >= slowFrom ? slow : rest, sample, seen);
        Count(pRecord->CellMs > 0.0f ? cells : noCells, sample, seen);
    }
    s_samples.swap(pending);
    s_keys.swap(pendingKeys);

    // Wall time each sample stands for
    const double msPerSample = aSamplesTaken ? aSeconds * 1000.0 / aSamplesTaken : 1.0;

    char text[256];
    std::snprintf(text, sizeof(text),
                  "Profile: %.1f s, %u frames (median %.2f ms; slow from %.2f ms: %u frames, %.0f%% of the time), %u samples (%.2f ms apart), cells attached in %u frames (%.2f ms each)",
                  aSeconds, all.Frames, median, slowFrom, slow.Frames, totalMs > 0.0 ? slowMs * 100.0 / totalMs : 0.0, walkedSamples, msPerSample, cellFrames,
                  cellFrames ? cellMs / cellFrames : 0.0);
    std::string out = text;
    out += "\nProfile all, ms per frame inclusive/own:" + ListAll(all, msPerSample);
    out += "\nProfile all, own ms per frame:" + ListOwn(all, msPerSample);
    if (slow.Frames)
        out += "\nProfile slow frames against the rest:" + ListDiff(slow, rest, msPerSample);
    if (cells.Frames)
        out += "\nProfile cell-attach frames against the rest:" + ListDiff(cells, noCells, msPerSample);

    // own time by module
    std::unordered_map<uint32_t, uint32_t> byModule;
    for (const auto& [key, count] : all.Own)
        byModule[static_cast<uint32_t>(key >> 32)] += count;
    std::vector<std::pair<uint32_t, uint32_t>> modules(byModule.begin(), byModule.end());
    std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    out += "\nProfile modules, own ms per frame:";
    const double perFrame = msPerSample / std::max<uint32_t>(all.Frames, 1);
    for (size_t i = 0; i < modules.size() && i < kTopModules; ++i)
    {
        std::snprintf(text, sizeof(text), " %s %.2f", modules[i].first < s_modules.size() ? s_modules[modules[i].first].Name.c_str() : "?", modules[i].second * perFrame);
        out += text;
    }

    std::vector<std::pair<Key, uint32_t>> waits(waitSites.begin(), waitSites.end());
    std::sort(waits.begin(), waits.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    out += "\nProfile main thread waits in, ms per frame:";
    for (size_t i = 0; i < waits.size() && i < kTopWaits; ++i)
    {
        std::snprintf(text, sizeof(text), " %.2f", waits[i].second * perFrame);
        out += " " + KeyName(waits[i].first) + text;
    }

    // Other threads, the busiest first: busy share of their own samples, then what they spend it on (inclusive share of
    // the busy samples, the frames every one of them has left out)
    std::vector<std::pair<uint16_t, const Other*>> busy;
    for (const auto& [index, other] : others)
    {
        if (other.Samples && static_cast<double>(other.Samples - other.Waiting) / other.Samples >= kBusyThreadShare)
            busy.emplace_back(index, &other);
    }
    std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.second->Samples - a.second->Waiting > b.second->Samples - b.second->Waiting; });
    std::snprintf(text, sizeof(text), "\nProfile threads (%zu sampled, %zu busy):", s_threads.size() - 1, busy.size());
    out += text;
    for (size_t i = 0; i < busy.size() && i < kMaxThreadLines; ++i)
    {
        const Thread& thread = s_threads[busy[i].first];
        const Other& other = *busy[i].second;
        const uint32_t working = other.Samples - other.Waiting;
        std::snprintf(text, sizeof(text), "\nProfile thread %u busy %.0f%%, started at ", thread.Id, working * 100.0 / other.Samples);
        out += text;
        std::string start = "?";
        if (thread.Start)
        {
            if (thread.Start >= s_gameBase && thread.Start < s_gameBase + s_gameSize)
                start = KeyName((static_cast<Key>(s_gameModule) << 32) | static_cast<uint32_t>(thread.Start - s_gameBase));
            else
            {
                HMODULE hModule = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(thread.Start), &hModule) && hModule)
                {
                    const auto base = reinterpret_cast<uint64_t>(hModule);
                    start = KeyName((static_cast<Key>(base == s_gameBase ? s_ourModule : ModuleFor(base)) << 32) | static_cast<uint32_t>(thread.Start - base));
                }
            }
        }
        out += start + ":";
        std::vector<std::pair<Key, uint32_t>> entries;
        for (const auto& [key, count] : other.Busy.Inclusive)
        {
            if (count < working * 95 / 100)
                entries.emplace_back(key, count);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t j = 0; j < entries.size() && j < kTopPerThread; ++j)
        {
            std::snprintf(text, sizeof(text), " %.0f%%", entries[j].second * 100.0 / other.Samples);
            out += " " + KeyName(entries[j].first) + text;
        }
        std::vector<std::pair<Key, uint32_t>> own(other.Busy.Own.begin(), other.Busy.Own.end());
        std::sort(own.begin(), own.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        out += "; own";
        for (size_t j = 0; j < own.size() && j < 4; ++j)
        {
            std::snprintf(text, sizeof(text), " %.0f%%", own[j].second * 100.0 / other.Samples);
            out += " " + KeyName(own[j].first) + text;
        }
    }
    spdlog::info("{}", out);
}

void Run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hTimer)
        hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due{};
    due.QuadPart = -10000ll * kPeriodMs;
    if (!hTimer || !SetWaitableTimer(hTimer, &due, kPeriodMs, nullptr, nullptr, FALSE))
    {
        spdlog::warn("Profile: no timer, not sampling");
        return;
    }

    s_samplerId = GetCurrentThreadId();
    RefreshThreads();

    auto windowStart = std::chrono::steady_clock::now();
    uint32_t taken = 0;
    for (;;)
    {
        WaitForSingleObject(hTimer, INFINITE);
        TakeSample(0);
        ++taken;
        // two other threads a tick, in turn
        for (int n = 0; n < 2 && s_threads.size() > 1; ++n)
        {
            for (size_t tries = 0; tries < s_threads.size(); ++tries)
            {
                if (s_nextOther >= s_threads.size())
                    s_nextOther = 1;
                const size_t index = s_nextOther++;
                if (s_threads[index].Handle)
                {
                    TakeSample(index);
                    break;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - windowStart).count();
        if (seconds >= kReportSeconds)
        {
            Report(seconds, taken);
            RefreshThreads();
            windowStart = now;
            taken = 0;
        }
    }
}

void Start() noexcept
{
    const auto* pTeb = reinterpret_cast<const uint8_t*>(NtCurrentTeb());
    Thread main;
    main.Id = GetCurrentThreadId();
    main.StackBase = *reinterpret_cast<const uint64_t*>(pTeb + 8);
    main.StackLow = *reinterpret_cast<const uint64_t*>(pTeb + 0x1478);
    main.Seen = true;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main.Handle, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
    {
        spdlog::warn("Profile: can't open the main thread ({}), not sampling", GetLastError());
        return;
    }
    s_threads.push_back(main);
    s_queryThread = reinterpret_cast<TNtQueryInformationThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    FindWaitStubs();

    s_gameBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(s_gameBase);
    const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(s_gameBase + pDos->e_lfanew);
    s_gameSize = pNt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& exceptions = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    s_gameFunctions = reinterpret_cast<const RUNTIME_FUNCTION*>(s_gameBase + exceptions.VirtualAddress);
    s_gameFunctionCount = exceptions.Size / sizeof(RUNTIME_FUNCTION);

    s_modules.push_back({s_gameBase, "game"});
    s_gameModule = static_cast<uint32_t>(s_modules.size() - 1);
    s_modules.push_back({s_gameBase, "rd"});
    s_ourModule = static_cast<uint32_t>(s_modules.size() - 1);
    s_stack.resize(kStackCopy + kStackSlack);
    s_samples.reserve(32 * 1024);
    s_keys.reserve(32 * 1024 * 24);

    spdlog::info("Profile: sampling the main thread every {} ms, game image {:X} ({:X} bytes, {} functions)", kPeriodMs, s_gameBase, s_gameSize, s_gameFunctionCount);
    std::thread(&Run).detach();
}
} // namespace

namespace PerfSampler
{
bool Enabled() noexcept
{
    return s_enabled;
}

void Frame(double aFrameMs) noexcept
{
    if (!s_enabled)
        return;

    static const bool s_started = (Start(), true);
    (void)s_started;

    const uint32_t index = s_frameIndex.fetch_add(1, std::memory_order_relaxed);
    const auto cellMs = static_cast<float>(static_cast<double>(s_cellNs.exchange(0, std::memory_order_relaxed)) / 1e6);
    std::scoped_lock _(s_frameLock);
    s_frames.push_back({index, static_cast<float>(aFrameMs), cellMs});
}

void AddCellWork(uint64_t aNanoseconds) noexcept
{
    if (s_enabled)
        s_cellNs.fetch_add(aNanoseconds, std::memory_order_relaxed);
}
} // namespace PerfSampler

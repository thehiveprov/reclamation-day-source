#include <Misc/BSFixedString.h>
#include <PerfSampler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <vector>

// Smoother cell loading, based on Zzyxzz's Smooth Cell Loading (Nexus 107810), rebuilt in our own code:
// - cell batching: while the game loads the 3D of a cell's references, the model jobs it starts one by one are held and
//   started together when the cell is done (one batch for the whole pass over the attached cells);
// - FaceGen batching: the three resource jobs one face build starts are held until the build is done;
// - subgraph cache: the lookup that picks an actor's animation subgraph from its race and keywords is remembered.
// Everything is patched at start, before the script extender loads plugins. With Smooth Cell Loading itself installed
// none of ours go in, so players who still have it keep exactly what they had.

namespace
{
// Reclamation Day ids are CommonLibF4's + 1 (VersionDb::LoadFallout4). Offsets are into the function with that id.

// BSQueuedResourceCollectionBase's start: a batch of resource jobs (model, texture or FaceGen loads) begins
constexpr uint32_t kStartJobs = 312705;
// Queues a reference's 3D: unless only queueing, it starts the reference's model jobs right away, here
constexpr uint32_t kQueueReference3D = 83542;
constexpr uint32_t kQueueReference3DStart = 0x3CC;
// The cell function that loads its references' 3D (cell, bool), and every call to it
constexpr uint32_t kCellReferences3D = 1465589;
struct CallSite
{
    uint32_t Function;
    uint32_t Offset;
};
constexpr CallSite kCellReferences3DCalls[] = {{271427, 0x17D}, {358323, 0x37}, {358323, 0x7E}, {358323, 0xBB}, {358323, 0xE6}, {399300, 0x34}, {999773, 0x150}, {999773, 0x188}};
// Goes over every attached cell (the interior, the loaded grid and the rest), calling the cell function on each
constexpr uint32_t kAttachedCellsPass = 358323;
// Builds an actor's FaceGen; three resource job batches (head TRI handles, textures, models) start as it ends
constexpr uint32_t kBuildFaceGen = 1211129;
constexpr uint32_t kBuildFaceGenStarts[] = {0x4B2, 0x4E6, 0x519};
constexpr CallSite kBuildFaceGenCall = {493841, 0x59};
// Picks an actor's animation subgraph (the race's subgraph table, its keywords, its equipment's keywords), and the two
// calls to it
constexpr uint32_t kSubgraphLookup = 801416;
constexpr CallSite kSubgraphLookupCalls[] = {{915679, 0x7E}, {929640, 0xBC}};
// BSFixedString copy constructor and assignment, release; the lookup's own way of appending strings to its arena;
// BSScrapArray's free
constexpr uint32_t kStringCopy = 374021;
constexpr uint32_t kStringAssign = 533177;
constexpr uint32_t kStringRelease = 1204431;
constexpr uint32_t kAppendStrings = 1090265;
constexpr uint32_t kFreeScrapArray = 25438;

// Smooth Cell Loading's builds: with either installed, ours stay out of the way
constexpr const wchar_t* kSmoothCellLoadingDlls[] = {L"SmoothCellLoadingRD.dll", L"SmoothCellLoading.dll"};
// Our MCM page's settings (Data/MCM/Config/ReclamationDay/config.json), read once at start
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr wchar_t kSettingsSection[] = L"SmoothLoading";

// FaceGen holds this many job batches at most (one build starts three), more start at once
constexpr size_t kFaceGenHeld = 8;
// Subgraph cache: shards (each its own lock and map), and found results kept at most
constexpr size_t kShards = 64;
constexpr uint32_t kMaxEntries = 32768;
// Totals go to the log at most this often (s), when they changed
constexpr int64_t kReportInterval = 60;

// Every request also runs the game's lookup and a hit is compared with it (tests only)
const bool s_verify = std::getenv("RD_SMOOTH_VERIFY") != nullptr;

using TStartJobs = void(void* apJobs);
using TCellReferences3D = uint64_t(void* apCell, bool aFlag);
using TAttachedCellsPass = void(void* apThis);
using TBuildFaceGen = uint64_t(void* apManager, void* apNpc, uint32_t aFlags, void* apRequest);
struct StringArena;
using TSubgraphLookup = bool(void* apTable, uint32_t aRaceId, const uint32_t* apRole, void* apKeywords, void* apItems, bool aFlag, uint64_t* apId, BSFixedString::Data** apName,
                             StringArena* apStrings);
using TStringCopy = void*(BSFixedString::Data** apTo, BSFixedString::Data* const* apFrom);
using TStringRelease = void(BSFixedString::Data** apString);
struct StringList;
using TAppendStrings = bool(const StringList* apFrom, StringArena* apTo);
struct KeywordList;
using TFreeScrapArray = void(KeywordList* apList);

TStartJobs* s_startJobs = nullptr;
TCellReferences3D* s_cellReferences3D = nullptr;
TAttachedCellsPass* RealAttachedCellsPass = nullptr;
TBuildFaceGen* s_buildFaceGen = nullptr;
TSubgraphLookup* s_subgraphLookup = nullptr;
TStringCopy* s_stringCopy = nullptr;
TStringCopy* s_stringAssign = nullptr;
TStringRelease* s_stringRelease = nullptr;
TAppendStrings* s_appendStrings = nullptr;
TFreeScrapArray* s_freeScrapArray = nullptr;

// Totals for the log. Cell passes: outermost cell functions (a batch when they held anything); direct: model jobs started
// right away outside any (other loading paths do that too, as without us)
std::atomic<uint64_t> s_cellPasses{0};
std::atomic<uint64_t> s_cellDirect{0};
std::atomic<uint64_t> s_cellBatches{0};
std::atomic<uint64_t> s_cellJobs{0};
std::atomic<uint64_t> s_faceGenBatches{0};
std::atomic<uint64_t> s_faceGenJobs{0};
std::atomic<uint64_t> s_requests{0};
std::atomic<uint64_t> s_hits{0};
std::atomic<uint64_t> s_coalesced{0};
std::atomic<uint64_t> s_misses{0};
std::atomic<uint64_t> s_uncached{0};
std::atomic<uint32_t> s_entries{0};
std::atomic<uint64_t> s_full{0};
std::atomic<uint64_t> s_mismatches{0};
std::atomic<int64_t> s_lastReport{0};
std::atomic<uint64_t> s_lastReported{0};

void MaybeReport() noexcept
{
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t last = s_lastReport.load(std::memory_order_relaxed);
    if (now - last < kReportInterval || !s_lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;

    const uint64_t activity = s_cellPasses + s_cellDirect + s_cellJobs + s_faceGenJobs + s_requests;
    if (activity == s_lastReported.exchange(activity, std::memory_order_relaxed))
        return;

    spdlog::info("Smooth loading: cells {} passes, {} batches / {} jobs, {} started directly, FaceGen {} batches / {} jobs, subgraphs {} requests: {} hits, {} coalesced, {} misses, "
                 "{} uncached, {} kept{}{}",
                 s_cellPasses.load(), s_cellBatches.load(), s_cellJobs.load(), s_cellDirect.load(), s_faceGenBatches.load(), s_faceGenJobs.load(), s_requests.load(), s_hits.load(),
                 s_coalesced.load(), s_misses.load(),
                 s_uncached.load(), s_entries.load(), s_full ? fmt::format(", {} not kept (full)", s_full.load()) : "",
                 s_verify ? fmt::format(", {} mismatches", s_mismatches.load()) : "");
}

// A resource job batch (BSQueuedResourceCollectionBase) is reference counted at 0x30; the deleting destructor comes first
// in its vtable
void AddRef(void* apJobs) noexcept
{
    _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(apJobs) + 0x30));
}

void Release(void* apJobs) noexcept
{
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(apJobs) + 0x30)) != 0)
        return;

    using TDelete = void(void*, uint32_t);
    (*reinterpret_cast<TDelete***>(apJobs))[0](apJobs, 1);
}

// Cell batching. Per thread: how deep in cell functions we are, and the job batches held until the outermost returns.
struct CellBatch
{
    int Depth = 0;
    std::vector<void*> Held;
    // when the outermost cell function began (the profiler's cell work)
    std::chrono::steady_clock::time_point Start;
};
thread_local CellBatch t_cells;

void StartHeldCellJobs() noexcept
{
    CellBatch& batch = t_cells;
    if (batch.Held.empty())
        return;

    // Starting a batch could hold more on this thread, those wait for the next round
    std::vector<void*> held;
    held.swap(batch.Held);
    for (void* pJobs : held)
    {
        s_startJobs(pJobs);
        Release(pJobs);
    }

    ++s_cellBatches;
    s_cellJobs += held.size();
    held.clear();
    if (batch.Held.empty())
        batch.Held.swap(held);

    MaybeReport();
}

struct CellScope
{
    // Leftovers start before we count ourselves in, so whatever they queue isn't held again
    CellScope() noexcept
    {
        CellBatch& batch = t_cells;
        if (batch.Depth == 0)
        {
            if (PerfSampler::Enabled())
                batch.Start = std::chrono::steady_clock::now();
            StartHeldCellJobs();
            batch.Held.reserve(4096);
            ++s_cellPasses;
        }
        ++batch.Depth;
    }

    ~CellScope()
    {
        if (--t_cells.Depth != 0)
            return;
        StartHeldCellJobs();
        if (PerfSampler::Enabled())
            PerfSampler::AddCellWork(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_cells.Start).count()));
    }
};

uint64_t HookCellReferences3D(void* apCell, bool aFlag)
{
    CellScope scope;
    return s_cellReferences3D(apCell, aFlag);
}

void HookAttachedCellsPass(void* apThis)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    CellScope scope;
    RealAttachedCellsPass(apThis);
}

void HookStartReferenceJobs(void* apJobs)
{
    CellBatch& batch = t_cells;
    if (batch.Depth == 0 || !apJobs)
    {
        ++s_cellDirect;
        s_startJobs(apJobs);
        return;
    }

    AddRef(apJobs);
    batch.Held.push_back(apJobs);
}

// FaceGen batching, the same per thread, in a fixed array
struct FaceGenBatch
{
    int Depth = 0;
    size_t Count = 0;
    std::array<void*, kFaceGenHeld> Held{};
};
thread_local FaceGenBatch t_faceGen;

void StartHeldFaceGenJobs() noexcept
{
    FaceGenBatch& batch = t_faceGen;
    if (batch.Count == 0)
        return;

    const size_t count = batch.Count;
    std::array<void*, kFaceGenHeld> held = batch.Held;
    batch.Count = 0;
    batch.Held.fill(nullptr);
    for (size_t i = 0; i < count; ++i)
    {
        s_startJobs(held[i]);
        Release(held[i]);
    }

    ++s_faceGenBatches;
    s_faceGenJobs += count;
    MaybeReport();
}

uint64_t HookBuildFaceGen(void* apManager, void* apNpc, uint32_t aFlags, void* apRequest)
{
    FaceGenBatch& batch = t_faceGen;
    if (batch.Depth == 0)
        StartHeldFaceGenJobs();
    ++batch.Depth;

    const uint64_t result = s_buildFaceGen(apManager, apNpc, aFlags, apRequest);

    if (--t_faceGen.Depth == 0)
        StartHeldFaceGenJobs();
    return result;
}

void HookStartFaceGenJobs(void* apJobs)
{
    FaceGenBatch& batch = t_faceGen;
    if (batch.Depth == 0 || batch.Count >= kFaceGenHeld || !apJobs)
    {
        s_startJobs(apJobs);
        return;
    }

    AddRef(apJobs);
    batch.Held[batch.Count++] = apJobs;
}

// Subgraph cache.
// BSScrapArray<BGSKeyword*> as the keyword collectors fill it: the scrap heap (the game sets it when it allocates), the
// keywords, capacity, count
struct KeywordList
{
    void* Heap = nullptr;
    void** Data = nullptr;
    uint32_t Capacity = 0;
    uint32_t Pad14 = 0;
    uint32_t Size = 0;
    uint32_t Pad1C = 0;
};
static_assert(sizeof(KeywordList) == 0x20);

// The equipment the lookup checks: keyword forms (each with the keyword interface's vtable)
struct KeywordForms
{
    uint8_t Pad0[0x8];
    void** Data;
    uint8_t Pad10[0x8];
    uint32_t Count;
};
static_assert(offsetof(KeywordForms, Count) == 0x18);

// BSTObjectArena<BSFixedString>, the lookup's file list: pages of 32 strings, the next page's address after them
constexpr size_t kArenaPageSlots = 32;
struct StringArena
{
    void* Allocator;
    BSFixedString::Data** FirstPage;
    void* Tail;
    BSFixedString::Data** CurrentPage;
    void* FreePages;
    BSFixedString::Data** Next;
    BSFixedString::Data** First;
    uint32_t Count;
};
static_assert(offsetof(StringArena, Count) == 0x38);

// The small array the game's append copies from: capacity (top bit: stored inline), the strings, count
struct StringList
{
    uint32_t Capacity;
    uint32_t Pad4;
    BSFixedString::Data* const* Data;
    uint32_t Size;
    uint32_t Pad14;
};
static_assert(offsetof(StringList, Size) == 0x10);

// Keyword interfaces: CollectAllKeywords is third in the vtable
void CollectKeywords(void* apForm, KeywordList& aList) noexcept
{
    using TCollect = void(void*, KeywordList*, void*);
    (*reinterpret_cast<TCollect***>(apForm))[2](apForm, &aList, nullptr);
}

struct Key
{
    uint32_t Race = 0;
    uint32_t Role = 0;
    bool Flag = false;
    // The actor's keywords the game's matcher keeps (bit 0 at 0x18), sorted, duplicates kept: it starts its search from
    // their count
    std::vector<const void*> Keywords;
    // The equipment's keywords, sorted, once each: it only asks whether each one is there
    std::vector<const void*> ItemKeywords;
    uint64_t Hash = 0;

    bool operator==(const Key& acRhs) const noexcept
    {
        return Race == acRhs.Race && Role == acRhs.Role && Flag == acRhs.Flag && Keywords == acRhs.Keywords && ItemKeywords == acRhs.ItemKeywords;
    }
};

struct KeyHash
{
    size_t operator()(const Key& acKey) const noexcept { return acKey.Hash; }
};

uint64_t Mix(uint64_t aHash, uint64_t aValue) noexcept
{
    uint64_t x = aHash ^ (aValue + 0x9E3779B97F4A7C15ull + (aHash << 6) + (aHash >> 2));
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

bool MakeKey(uint32_t aRaceId, const uint32_t* apRole, void* apKeywords, void* apItems, bool aFlag, Key& aKey) noexcept
{
    if (!apRole || !apKeywords || !apItems)
        return false;

    aKey.Race = aRaceId;
    aKey.Role = *apRole;
    aKey.Flag = aFlag;

    KeywordList list;
    CollectKeywords(apKeywords, list);
    for (uint32_t i = 0; i < list.Size; ++i)
    {
        const auto* pKeyword = static_cast<const uint8_t*>(list.Data[i]);
        if (pKeyword && (pKeyword[0x18] & 1))
            aKey.Keywords.push_back(pKeyword);
    }
    s_freeScrapArray(&list);
    std::sort(aKey.Keywords.begin(), aKey.Keywords.end());

    const auto* pItems = static_cast<const KeywordForms*>(apItems);
    for (uint32_t i = 0; pItems->Data && i < pItems->Count; ++i)
    {
        if (!pItems->Data[i])
            continue;

        KeywordList itemList;
        CollectKeywords(pItems->Data[i], itemList);
        for (uint32_t j = 0; j < itemList.Size; ++j)
        {
            if (itemList.Data[j])
                aKey.ItemKeywords.push_back(itemList.Data[j]);
        }
        s_freeScrapArray(&itemList);
    }
    std::sort(aKey.ItemKeywords.begin(), aKey.ItemKeywords.end());
    aKey.ItemKeywords.erase(std::unique(aKey.ItemKeywords.begin(), aKey.ItemKeywords.end()), aKey.ItemKeywords.end());

    uint64_t hash = Mix(Mix(Mix(0, aKey.Race), aKey.Role), aKey.Flag);
    hash = Mix(hash, aKey.Keywords.size());
    for (const void* pKeyword : aKey.Keywords)
        hash = Mix(hash, reinterpret_cast<uintptr_t>(pKeyword));
    hash = Mix(hash, aKey.ItemKeywords.size());
    for (const void* pKeyword : aKey.ItemKeywords)
        hash = Mix(hash, reinterpret_cast<uintptr_t>(pKeyword));
    aKey.Hash = hash;
    return true;
}

// What the game's lookup gave for a key it found: the subgraph's id, its name and the strings it added to the list. We
// hold a reference on each string.
struct Found
{
    uint64_t Id = 0;
    BSFixedString::Data* Name = nullptr;
    std::vector<BSFixedString::Data*> Strings;
};

BSFixedString::Data* Copy(BSFixedString::Data* apString) noexcept
{
    BSFixedString::Data* pCopy = nullptr;
    s_stringCopy(&pCopy, &apString);
    return pCopy;
}

void Drop(Found& aFound) noexcept
{
    if (aFound.Name)
        s_stringRelease(&aFound.Name);
    for (BSFixedString::Data*& pString : aFound.Strings)
    {
        if (pString)
            s_stringRelease(&pString);
    }
    aFound.Strings.clear();
}

// What the lookup just wrote: the id, the name, and the arena's strings from aFrom on. False if the arena isn't laid out
// as expected (then nothing is kept).
bool Capture(uint64_t aId, BSFixedString::Data* apName, const StringArena& acStrings, uint32_t aFrom, Found& aFound) noexcept
{
    aFound.Id = aId;
    aFound.Name = Copy(apName);
    if (acStrings.Count <= aFrom)
        return true;

    // Only ever appended to here: the first string is on the first page
    BSFixedString::Data** pPage = acStrings.FirstPage;
    BSFixedString::Data** pSlot = acStrings.First;
    if (!pPage || pSlot < pPage || pSlot >= pPage + kArenaPageSlots)
        return false;

    for (uint32_t i = 0; i < acStrings.Count; ++i)
    {
        if (i >= aFrom)
            aFound.Strings.push_back(Copy(*pSlot));
        if (++pSlot == pPage + kArenaPageSlots && pSlot != acStrings.Next && i + 1 < acStrings.Count)
        {
            pPage = reinterpret_cast<BSFixedString::Data**>(pPage[kArenaPageSlots]);
            if (!pPage)
                return false;
            pSlot = pPage;
        }
    }
    return true;
}

void Replay(const Found& acFound, uint64_t* apId, BSFixedString::Data** apName, StringArena* apStrings) noexcept
{
    *apId = acFound.Id;
    s_stringAssign(apName, &acFound.Name);
    const auto count = static_cast<uint32_t>(acFound.Strings.size());
    const StringList list{count, 0, acFound.Strings.data(), count, 0};
    s_appendStrings(&list, apStrings);
}

bool Same(const Found& acCached, const Found& acGame) noexcept
{
    return acCached.Id == acGame.Id && acCached.Name == acGame.Name && acCached.Strings == acGame.Strings;
}

struct Shard
{
    SRWLOCK Lock = SRWLOCK_INIT;
    std::unordered_map<Key, Found, KeyHash> Map;
};
std::array<Shard, kShards> s_shards;

bool HookSubgraphLookup(void* apTable, uint32_t aRaceId, const uint32_t* apRole, void* apKeywords, void* apItems, bool aFlag, uint64_t* apId, BSFixedString::Data** apName,
                        StringArena* apStrings)
{
    ++s_requests;

    Key key;
    if (!apId || !apName || !apStrings || !MakeKey(aRaceId, apRole, apKeywords, apItems, aFlag, key))
    {
        ++s_uncached;
        return s_subgraphLookup(apTable, aRaceId, apRole, apKeywords, apItems, aFlag, apId, apName, apStrings);
    }

    Shard& shard = s_shards[key.Hash % kShards];
    if (!s_verify)
    {
        AcquireSRWLockShared(&shard.Lock);
        const auto it = shard.Map.find(key);
        if (it != shard.Map.end())
            Replay(it->second, apId, apName, apStrings);
        const bool hit = it != shard.Map.end();
        ReleaseSRWLockShared(&shard.Lock);
        if (hit)
        {
            ++s_hits;
            MaybeReport();
            return true;
        }
    }

    // A miss: one thread at a time per shard asks the game, so the same miss on two threads is only worked out once
    AcquireSRWLockExclusive(&shard.Lock);
    const auto it = shard.Map.find(key);
    if (it != shard.Map.end() && !s_verify)
    {
        Replay(it->second, apId, apName, apStrings);
        ReleaseSRWLockExclusive(&shard.Lock);
        ++s_coalesced;
        return true;
    }

    const uint32_t before = apStrings->Count;
    const bool found = s_subgraphLookup(apTable, aRaceId, apRole, apKeywords, apItems, aFlag, apId, apName, apStrings);

    Found result;
    const bool captured = found && Capture(*apId, *apName, *apStrings, before, result);
    if (it != shard.Map.end())
    {
        // Verifying: the game just worked out what the cache holds
        if (!captured || !Same(it->second, result))
        {
            ++s_mismatches;
            spdlog::warn("Smooth loading: cache mismatch for race {:X} role {} flag {} ({} keywords, {} item keywords): game {} with {} strings, cache {} strings", aRaceId, key.Role, key.Flag,
                         key.Keywords.size(), key.ItemKeywords.size(), found ? "found" : "nothing", result.Strings.size(), it->second.Strings.size());
        }
        else
            ++s_hits;
        Drop(result);
    }
    else if (captured && s_entries < kMaxEntries)
    {
        ++s_entries;
        ++s_misses;
        shard.Map.emplace(std::move(key), std::move(result));
    }
    else
    {
        ++s_misses;
        if (captured)
            ++s_full;
        Drop(result);
    }
    ReleaseSRWLockExclusive(&shard.Lock);

    MaybeReport();
    return found;
}

// Installing. Each patch replaces a call the game makes (E8, 5 bytes) after checking it still calls what we expect, so
// another mod's patch there turns that part of ours off instead of being undone.
uint8_t* FunctionAt(uint32_t aId) noexcept
{
    const VersionDbPtr<uint8_t> function(aId);
    return function.Get();
}

bool CallsInto(const uint8_t* apSite, const void* apTarget) noexcept
{
    return apSite && apSite[0] == 0xE8 && apSite + 5 + *reinterpret_cast<const int32_t*>(apSite + 1) == apTarget;
}

bool InReach(const uint8_t* apSite, const void* apHook) noexcept
{
    const intptr_t distance = reinterpret_cast<intptr_t>(apHook) - reinterpret_cast<intptr_t>(apSite + 5);
    return distance == static_cast<int32_t>(distance);
}

// All of these sites call aTarget and can reach aHook
template <class TSites> bool CheckSites(const TSites& acSites, const void* apTarget, const void* apHook, const char* acpPart) noexcept
{
    for (const uint8_t* pSite : acSites)
    {
        if (!CallsInto(pSite, apTarget) || !InReach(pSite, apHook))
        {
            spdlog::warn("Smooth loading: {} stays off, the game's call at {} isn't the expected one (another mod patched it?)", acpPart, static_cast<const void*>(pSite));
            return false;
        }
    }
    return true;
}

bool InstallCellBatching() noexcept
{
    std::vector<uint8_t*> cellCalls;
    for (const CallSite& site : kCellReferences3DCalls)
        cellCalls.push_back(FunctionAt(site.Function) + site.Offset);
    uint8_t* const startSite[] = {FunctionAt(kQueueReference3D) + kQueueReference3DStart};

    if (!CheckSites(cellCalls, s_cellReferences3D, reinterpret_cast<const void*>(&HookCellReferences3D), "cell batching") ||
        !CheckSites(startSite, s_startJobs, reinterpret_cast<const void*>(&HookStartReferenceJobs), "cell batching"))
        return false;

    for (uint8_t* pSite : cellCalls)
        TiltedPhoques::PutCall(pSite, &HookCellReferences3D);
    TiltedPhoques::PutCall(startSite[0], &HookStartReferenceJobs);

    // One batch for the whole pass over the attached cells
    RealAttachedCellsPass = reinterpret_cast<TAttachedCellsPass*>(FunctionAt(kAttachedCellsPass));
    TP_HOOK(&RealAttachedCellsPass, HookAttachedCellsPass);
    return true;
}

bool InstallFaceGenBatching() noexcept
{
    uint8_t* const buildSite[] = {FunctionAt(kBuildFaceGenCall.Function) + kBuildFaceGenCall.Offset};
    std::vector<uint8_t*> startSites;
    for (const uint32_t offset : kBuildFaceGenStarts)
        startSites.push_back(reinterpret_cast<uint8_t*>(s_buildFaceGen) + offset);

    if (!CheckSites(buildSite, s_buildFaceGen, reinterpret_cast<const void*>(&HookBuildFaceGen), "FaceGen batching") ||
        !CheckSites(startSites, s_startJobs, reinterpret_cast<const void*>(&HookStartFaceGenJobs), "FaceGen batching"))
        return false;

    TiltedPhoques::PutCall(buildSite[0], &HookBuildFaceGen);
    for (uint8_t* pSite : startSites)
        TiltedPhoques::PutCall(pSite, &HookStartFaceGenJobs);
    return true;
}

bool InstallSubgraphCache() noexcept
{
    std::vector<uint8_t*> sites;
    for (const CallSite& site : kSubgraphLookupCalls)
        sites.push_back(FunctionAt(site.Function) + site.Offset);

    if (!CheckSites(sites, s_subgraphLookup, reinterpret_cast<const void*>(&HookSubgraphLookup), "the subgraph cache"))
        return false;

    for (uint8_t* pSite : sites)
        TiltedPhoques::PutCall(pSite, &HookSubgraphLookup);
    return true;
}

bool ReadSetting(const std::filesystem::path& acPath, const wchar_t* acpKey) noexcept
{
    return GetPrivateProfileIntW(kSettingsSection, acpKey, 1, acPath.c_str()) != 0;
}
} // namespace

static TiltedPhoques::Initializer s_smoothCellLoadingHooks(
    []()
    {
        // the game's folder, as the script extender is found (ScriptExtender.cpp)
        const auto data = std::filesystem::current_path() / L"Data";
        for (const wchar_t* pDll : kSmoothCellLoadingDlls)
        {
            std::error_code ec;
            if (std::filesystem::exists(data / L"F4SE" / L"Plugins" / pDll, ec))
            {
                spdlog::info(L"Smooth loading: off, Smooth Cell Loading ({}) is installed and does the same (remove it to use ours)", pDll);
                return;
            }
        }

        s_startJobs = reinterpret_cast<TStartJobs*>(FunctionAt(kStartJobs));
        s_cellReferences3D = reinterpret_cast<TCellReferences3D*>(FunctionAt(kCellReferences3D));
        s_buildFaceGen = reinterpret_cast<TBuildFaceGen*>(FunctionAt(kBuildFaceGen));
        s_subgraphLookup = reinterpret_cast<TSubgraphLookup*>(FunctionAt(kSubgraphLookup));
        s_stringCopy = reinterpret_cast<TStringCopy*>(FunctionAt(kStringCopy));
        s_stringAssign = reinterpret_cast<TStringCopy*>(FunctionAt(kStringAssign));
        s_stringRelease = reinterpret_cast<TStringRelease*>(FunctionAt(kStringRelease));
        s_appendStrings = reinterpret_cast<TAppendStrings*>(FunctionAt(kAppendStrings));
        s_freeScrapArray = reinterpret_cast<TFreeScrapArray*>(FunctionAt(kFreeScrapArray));
        if (!s_startJobs || !s_cellReferences3D || !s_buildFaceGen || !s_subgraphLookup || !s_stringCopy || !s_stringAssign || !s_stringRelease || !s_appendStrings ||
            !s_freeScrapArray || !FunctionAt(kAttachedCellsPass))
        {
            spdlog::warn("Smooth loading: off, a game function wasn't found in the address library");
            return;
        }

        const auto settings = data / kSettingsFile;
        const bool cells = ReadSetting(settings, L"bCellBatching") && InstallCellBatching();
        const bool faceGen = ReadSetting(settings, L"bFaceGenBatching") && InstallFaceGenBatching();
        const bool subgraphs = ReadSetting(settings, L"bSubgraphCache") && InstallSubgraphCache();
        spdlog::info("Smooth loading: cell batching {}, FaceGen batching {}, subgraph cache {}{}", cells ? "on" : "off", faceGen ? "on" : "off", subgraphs ? "on" : "off",
                     s_verify ? " (verifying every subgraph lookup against the game)" : "");
    });

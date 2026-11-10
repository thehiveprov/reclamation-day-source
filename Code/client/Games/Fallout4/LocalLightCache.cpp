#include <ShadowFeatures.h>

#include <atomic>
#include <cstring>
#include <unordered_map>

// Local light shadow caching. Every shadowed spot and point light draws its casters into the same depth slice of the game's
// shadow texture and is lit from it straight away, then the next light draws over it. So a light whose map hasn't changed
// still costs all its draw calls every frame. Here the casters each light gathers are fingerprinted (pointer, transform);
// when a light's fingerprint and its own camera haven't changed, its map from a kept copy goes into the slice instead of
// the draw calls. The picture is the same by construction: anything moving, skinned or animated makes the light draw.

namespace
{
// NiAVObject: world transform, flags; BSGeometry: skin (bones move the mesh)
constexpr size_t kWorldTransform = 0x70;
constexpr size_t kWorldTransformSize = 0x40;
constexpr size_t kObjectFlags = 0x108;
constexpr size_t kGeometrySkin = 0x140;
constexpr uint64_t kObjectDynamicMesh = 1ull << 0x22;
// BSShaderProperty's flags: ones whose pass goes a different way than an ordinary caster's
constexpr size_t kPropertyFlags = 0x30;
constexpr uint64_t kPropertySpecialPass = 0x2000000000000000ull;
// NiCamera: world transform, its frustum; the map's size is in the camera's viewport
constexpr size_t kCameraWorld = 0x70;
constexpr size_t kCameraWorldSize = 0x40;
constexpr size_t kCameraFrustum = 0x160;
constexpr size_t kCameraFrustumAndViewportSize = 0x34;
// BSShaderAccumulator's draw lists: per group of shaders, what its render leaves empty
constexpr size_t kAccumulatorBatch = 200;
constexpr size_t kBatchClears = 0x350;
constexpr size_t kBatchFirst = 0x2E0; // first and last in a list, by group
constexpr size_t kBatchLast = 0x2E4;
constexpr size_t kBatchEntries = 8;
constexpr size_t kBatchEntryStride = 0x18;
constexpr uint32_t kShadowGroup = 1;
constexpr uint32_t kNoEntry = 0xFFFF;
constexpr size_t kEntryNext = 0xC;
// The accumulator's virtual that finishes it after a draw
constexpr size_t kAccumulatorFinish = 0x178;

// How many frames a fingerprint must stay before the map is kept (a copy costs something, a light that keeps changing shouldn't)
constexpr uint32_t kStableFrames = 3;

struct Entry
{
    uint32_t Frame = 0;
    uint64_t Sum = 0;
    uint64_t Mixed = 0;
    uint32_t Casters = 0;
    bool Dynamic = false;
    uint64_t Last = 0;
    uint32_t Stable = 0;
    int32_t Slot = -1;
    uint64_t SlotPrint = 0;
    uint32_t LastUsed = 0;
};

std::unordered_map<const void*, Entry> s_entries;
uint32_t s_slots = 0;
const void* s_slotOwner[ShadowDepthStore::kMaxSlots]{};

uint32_t s_reused = 0;
uint32_t s_drawn = 0;
uint32_t s_dynamic = 0;
uint32_t s_copies = 0;

template <class T> T Read(const void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(apObject) + aOffset);
}

uint64_t Mix(uint64_t aValue) noexcept
{
    aValue ^= aValue >> 33;
    aValue *= 0xff51afd7ed558ccdull;
    aValue ^= aValue >> 33;
    aValue *= 0xc4ceb9fe1a85ec53ull;
    aValue ^= aValue >> 33;
    return aValue;
}

uint64_t HashBytes(const void* apData, size_t aSize) noexcept
{
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto* pWords = static_cast<const uint32_t*>(apData);
    for (size_t i = 0; i < aSize / 4; ++i)
        hash = (hash ^ pWords[i]) * 0x100000001b3ull;
    return hash;
}

// What the render would have left empty: the list of this accumulator's passes (14287F380's clearing, without its drawing)
void ClearPasses(void* apAccumulator) noexcept
{
    auto* pBatch = static_cast<uint8_t*>(apAccumulator) + kAccumulatorBatch;
    if (!*(pBatch + kBatchClears))
        return;
    auto*& pEntries = *reinterpret_cast<uint8_t**>(pBatch + kBatchEntries + kShadowGroup * kBatchEntryStride);
    auto& first = *reinterpret_cast<uint32_t*>(pBatch + kBatchFirst + kShadowGroup * 8);
    auto& last = *reinterpret_cast<uint32_t*>(pBatch + kBatchLast + kShadowGroup * 8);
    uint32_t index = first;
    while (index != kNoEntry && pEntries)
    {
        auto* pEntry = pEntries + static_cast<size_t>(index) * 0x10;
        *reinterpret_cast<uint64_t*>(pEntry) = 0;
        index = *reinterpret_cast<uint16_t*>(pEntry + kEntryNext);
    }
    first = kNoEntry;
    last = kNoEntry;
}

int32_t TakeSlot(const void* apAccumulator, Entry& arEntry) noexcept
{
    if (arEntry.Slot >= 0 && s_slotOwner[arEntry.Slot] == apAccumulator)
        return arEntry.Slot;
    // the slot not used the longest; the first ones are the sun's when it keeps its cascades
    int32_t oldest = -1;
    uint32_t oldestUse = 0xFFFFFFFF;
    const uint32_t count = std::min<uint32_t>(s_slots, ShadowDepthStore::kMaxSlots - ShadowDepthStore::kCascadeSlots);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t slot = ShadowDepthStore::kCascadeSlots + i;
        if (!s_slotOwner[slot])
            return static_cast<int32_t>(slot);
        const auto it = s_entries.find(s_slotOwner[slot]);
        const uint32_t used = it == s_entries.end() ? 0 : it->second.LastUsed;
        if (used < oldestUse)
        {
            oldestUse = used;
            oldest = static_cast<int32_t>(slot);
        }
    }
    if (oldest >= 0)
    {
        if (const auto it = s_entries.find(s_slotOwner[oldest]); it != s_entries.end())
            it->second.Slot = -1;
    }
    return oldest;
}
} // namespace

bool LocalLightCache::Enabled() noexcept
{
    return s_slots != 0;
}

void LocalLightCache::Refresh() noexcept
{
    const uint32_t slots = std::min<uint32_t>(ShadowFeatures::ReadSetting(L"iLocalLightCache", 0), ShadowDepthStore::kMaxSlots - ShadowDepthStore::kCascadeSlots);
    if (slots != s_slots)
    {
        spdlog::info("Local light shadow cache: {}", slots ? fmt::format("keeps up to {} maps", slots) : std::string("off"));
        // what's kept goes on a change: the slots' owners were for the old count
        for (auto& pOwner : s_slotOwner)
            pOwner = nullptr;
        s_entries.clear();
    }
    s_slots = slots;
}

void LocalLightCache::NoteCaster(void* apAccumulator, void* apGeometry, void* apProperty) noexcept
{
    if (!s_slots)
        return;
    Entry& entry = s_entries[apAccumulator];
    const uint32_t frame = ShadowFeatures::FrameNumber();
    if (entry.Frame != frame)
    {
        entry.Frame = frame;
        entry.Sum = 0;
        entry.Mixed = 0;
        entry.Casters = 0;
        entry.Dynamic = false;
    }
    ++entry.Casters;
    // By addition: the order casters come in may change from frame to frame, and the map doesn't care
    const uint64_t hash = Mix(reinterpret_cast<uint64_t>(apGeometry)) ^ HashBytes(static_cast<const uint8_t*>(apGeometry) + kWorldTransform, kWorldTransformSize);
    entry.Sum += hash;
    entry.Mixed ^= Mix(hash);
    if (Read<const void*>(apGeometry, kGeometrySkin) || (Read<uint64_t>(apGeometry, kObjectFlags) & kObjectDynamicMesh) || (Read<uint64_t>(apProperty, kPropertyFlags) & kPropertySpecialPass))
        entry.Dynamic = true;
}

bool LocalLightCache::OnRender(void* apCamera, void* apAccumulator, void* apFlag, TRender* apRender) noexcept
{
    if (!s_slots || !apCamera)
        return false;

    const auto found = s_entries.find(apAccumulator);
    if (found == s_entries.end() || found->second.Frame != ShadowFeatures::FrameNumber() || found->second.Casters == 0)
        return false; // nothing gathered: clearing the slice is all the draw does
    Entry& entry = found->second;
    entry.LastUsed = ShadowFeatures::FrameNumber();

    if (entry.Dynamic)
    {
        entry.Stable = 0;
        ++s_dynamic;
        return false;
    }

    const auto* pCamera = static_cast<const uint8_t*>(apCamera);
    const uint64_t print = entry.Sum ^ Mix(entry.Mixed) ^ Mix(HashBytes(pCamera + kCameraWorld, kCameraWorldSize)) ^ HashBytes(pCamera + kCameraFrustum, kCameraFrustumAndViewportSize);
    entry.Stable = print == entry.Last ? entry.Stable + 1 : 0;
    entry.Last = print;

    if (entry.Stable >= kStableFrames && entry.Slot >= 0 && s_slotOwner[entry.Slot] == apAccumulator && entry.SlotPrint == print)
    {
        if (ShadowDepthStore::Restore(static_cast<uint32_t>(entry.Slot)))
        {
            // what the render and its end would have done to the accumulator
            ClearPasses(apAccumulator);
            using TFinish = void(void*);
            (*reinterpret_cast<TFinish* const*>(*static_cast<const uint8_t* const*>(apAccumulator) + kAccumulatorFinish))(apAccumulator);
            ++s_reused;
            return true;
        }
    }

    apRender(apCamera, apAccumulator, apFlag);
    ++s_drawn;

    if (entry.Stable >= kStableFrames)
    {
        const int32_t slot = TakeSlot(apAccumulator, entry);
        if (slot >= 0 && ShadowDepthStore::Save(static_cast<uint32_t>(slot)))
        {
            s_slotOwner[slot] = apAccumulator;
            entry.Slot = slot;
            entry.SlotPrint = print;
            ++s_copies;
        }
    }
    return true;
}

void LocalLightCache::Report(double aSeconds) noexcept
{
    if (s_slots && (s_reused || s_drawn || s_dynamic))
        spdlog::info("Local light shadow cache, maps a second: {:.0f} from a kept copy, {:.0f} drawn, {:.0f} have moving casters; {:.0f} copies kept; {} lights seen", s_reused / aSeconds, s_drawn / aSeconds, s_dynamic / aSeconds,
                     s_copies / aSeconds, s_entries.size());
    s_reused = 0;
    s_drawn = 0;
    s_dynamic = 0;
    s_copies = 0;
    // lights long gone (their accumulators too, perhaps; nothing is ever read through these keys)
    if (s_entries.size() > 512)
    {
        for (auto it = s_entries.begin(); it != s_entries.end();)
        {
            bool held = false;
            for (const auto* pOwner : s_slotOwner)
                held = held || pOwner == it->first;
            if (!held && ShadowFeatures::FrameNumber() - it->second.LastUsed > 600)
                it = s_entries.erase(it);
            else
                ++it;
        }
    }
}

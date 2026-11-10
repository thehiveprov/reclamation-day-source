#include <HivemindPCH.h>

#include <Interface/GameHud.h>

#include <Misc/BSFixedString.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <intrin.h>
#include <utility>

namespace
{
using TShouldBeVisible = bool(void*);

constexpr size_t kPieceCount = static_cast<size_t>(GameHud::Piece::kCount);

// Each piece's class and its main vtable (RD ids: the address library's + 1, checked with re\rdid.js)
struct PieceInfo
{
    const char* pName;
    uint32_t VtableId;
};
constexpr std::array<PieceInfo, kPieceCount> kPieces{{
    {"HUDCompass", 1322924},
    {"HUDPlayerHealthMeter", 514596},
    {"HUDRadiationMeter", 349394},
    {"HUDActionPointMeter", 531328},
    {"HUDEnemyHealthMeter", 1224019},
    {"HUDAmmoCounter", 1125795},
    {"HUDExplosiveAmmoCounter", 1471614},
    {"HUDCrosshair", 387514},
}};
// HUDComponentBase's "should it be visible": the per-frame fade (slot 4) and the immediate show (slot 5) ask it
constexpr size_t kShouldBeVisibleSlot = 7;
// The game's image: a slot that points elsewhere is another mod's hook, left alone
constexpr uintptr_t kImageSize = 0x6D00000;
// Not asked for this long: the HUD isn't up
constexpr int64_t kAskedLately = 300;

// HUDDataModel (the pointer in id 910584): the enemy's bar and the ammo as BSTValueEventSources (vtable, the value,
// a has-value byte, a BSSpinLock), the compass markers and their lock
constexpr size_t kEnemyValue = 0x668 + 0x08;
constexpr size_t kEnemyHas = 0x668 + 0x20;
constexpr size_t kEnemyLock = 0x668 + 0x28;
constexpr size_t kAmmoValue = 0x9E0 + 0x08;
constexpr size_t kAmmoHas = 0x9E0 + 0x10;
constexpr size_t kAmmoLock = 0x9E0 + 0x14;
constexpr size_t kMarkers = 0x1340;
constexpr size_t kMarkersLock = 0x1398;
// HUDExplosiveAmmoCounter: its HUDExplosiveAmmoCounterData sink at 0xF8 keeps the event's second number at 0x100, the
// first at 0x104
constexpr size_t kExplosivesSecond = 0x100;
constexpr size_t kExplosivesFirst = 0x104;

// The compass's copy of the markers: blocks of 48 records, the next block's pointer after them
constexpr size_t kRecordSize = 0x38;
constexpr size_t kBlockSize = 0xA80;
constexpr size_t kMaxMarkers = 512;

struct PieceState
{
    TShouldBeVisible* pOriginal{nullptr};
    std::atomic<void*> pComponent{nullptr};
    std::atomic<bool> WouldShow{false};
    std::atomic<int64_t> AskedAt{0};
};
std::array<PieceState, kPieceCount> s_pieces;
std::atomic<bool> s_hide{false};
std::atomic<bool> s_hideCrosshair{false};
bool s_installed = false;

int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The HUD asks every frame, on its own thread: the game's answer is kept, ours is "no" while hidden
template <size_t I> bool ShouldBeVisible(void* apThis) noexcept
{
    auto& state = s_pieces[I];
    const bool show = state.pOriginal(apThis);
    state.pComponent.store(apThis, std::memory_order_relaxed);
    state.WouldShow.store(show, std::memory_order_relaxed);
    state.AskedAt.store(NowMs(), std::memory_order_relaxed);
    if constexpr (I == static_cast<size_t>(GameHud::Piece::kCrosshair))
        return show && !s_hideCrosshair.load(std::memory_order_relaxed);
    else
        return show && !s_hide.load(std::memory_order_relaxed);
}

template <size_t... I> constexpr std::array<TShouldBeVisible*, sizeof...(I)> MakeDetours(std::index_sequence<I...>) noexcept
{
    return {&ShouldBeVisible<I>...};
}
constexpr auto kDetours = MakeDetours(std::make_index_sequence<kPieceCount>{});

uint8_t* GetModel() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_model, 910585);
    return *s_model.Get();
}

// BSSpinLock: locked by the game's own function (id 1425657), unlocked as the game does it (owning thread, then the
// count), as PipboyMapService does with the quest targets
void SpinLock(uint8_t* apLock) noexcept
{
    using TLock = void(void*, const char*);
    POINTER_FALLOUT4(TLock, s_lock, 1425658);
    s_lock.Get()(apLock, nullptr);
}

void SpinUnlock(uint8_t* apLock) noexcept
{
    auto* pCount = reinterpret_cast<volatile long*>(apLock + 4);
    if (*pCount == 1)
    {
        *reinterpret_cast<volatile uint32_t*>(apLock) = 0;
        _InterlockedCompareExchange(pCount, 0, 1);
    }
    else
    {
        _InterlockedDecrement(pCount);
    }
}

template <class T> T Read(const uint8_t* apBase, size_t aOffset) noexcept
{
    T value;
    std::memcpy(&value, apBase + aOffset, sizeof(T));
    return value;
}
} // namespace

void GameHud::Install() noexcept
{
    if (s_installed)
        return;
    s_installed = true;

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (size_t i = 0; i < kPieceCount; ++i)
    {
        auto** pVtable = static_cast<void**>(VersionDb::Get().FindAddressById(kPieces[i].VtableId));
        if (!pVtable)
        {
            spdlog::warn("Game HUD: no vtable for {}", kPieces[i].pName);
            continue;
        }
        void** pSlot = pVtable + kShouldBeVisibleSlot;
        const auto current = reinterpret_cast<uintptr_t>(*pSlot);
        if (current < base || current >= base + kImageSize)
        {
            spdlog::warn("Game HUD: {}'s slot {} is someone else's ({:#x}), left alone", kPieces[i].pName, kShouldBeVisibleSlot, current);
            continue;
        }

        s_pieces[i].pOriginal = reinterpret_cast<TShouldBeVisible*>(*pSlot);
        DWORD oldProtection = 0;
        if (!VirtualProtect(pSlot, sizeof(void*), PAGE_READWRITE, &oldProtection))
        {
            spdlog::warn("Game HUD: {}'s vtable can't be written", kPieces[i].pName);
            s_pieces[i].pOriginal = nullptr;
            continue;
        }
        _InterlockedExchangePointer(pSlot, reinterpret_cast<void*>(kDetours[i]));
        VirtualProtect(pSlot, sizeof(void*), oldProtection, &oldProtection);
    }
    spdlog::info("Game HUD: our answer in the HUD pieces' slot {}", kShouldBeVisibleSlot);
}

void GameHud::Hide(bool aHide) noexcept
{
    s_hide.store(aHide, std::memory_order_relaxed);
}

void GameHud::HideCrosshair(bool aHide) noexcept
{
    s_hideCrosshair.store(aHide, std::memory_order_relaxed);
}

bool GameHud::WouldShow(Piece aPiece) noexcept
{
    const auto& state = s_pieces[static_cast<size_t>(aPiece)];
    return state.pOriginal && state.WouldShow.load(std::memory_order_relaxed) && NowMs() - state.AskedAt.load(std::memory_order_relaxed) < kAskedLately;
}

GameHud::EnemyHealth GameHud::GetEnemyHealth() noexcept
{
    EnemyHealth enemy;
    uint8_t* pModel = GetModel();
    if (!pModel)
        return enemy;

    SpinLock(pModel + kEnemyLock);
    if (Read<uint8_t>(pModel, kEnemyHas))
    {
        const uint8_t* pValue = pModel + kEnemyValue;
        const auto* pName = reinterpret_cast<const BSFixedString*>(pValue);
        const char* pText = pName->AsAscii();
        enemy.Valid = true;
        enemy.Name = pText ? pText : "";
        enemy.Health = Read<float>(pValue, 0x08);
        enemy.Optional = Read<float>(pValue, 0x0C);
        enemy.Hostile = Read<uint8_t>(pValue, 0x10) != 0;
        enemy.Legendary = Read<uint8_t>(pValue, 0x11) != 0;
        enemy.Skull = Read<uint8_t>(pValue, 0x12) != 0;
    }
    SpinUnlock(pModel + kEnemyLock);
    return enemy;
}

GameHud::Counts GameHud::GetAmmo() noexcept
{
    Counts ammo;
    uint8_t* pModel = GetModel();
    if (!pModel)
        return ammo;

    SpinLock(pModel + kAmmoLock);
    if (Read<uint8_t>(pModel, kAmmoHas))
    {
        ammo.Valid = true;
        ammo.First = Read<uint32_t>(pModel, kAmmoValue);
        ammo.Second = Read<uint32_t>(pModel, kAmmoValue + 4);
    }
    SpinUnlock(pModel + kAmmoLock);
    return ammo;
}

GameHud::Counts GameHud::GetExplosives() noexcept
{
    Counts explosives;
    const auto* pComponent = static_cast<const uint8_t*>(s_pieces[static_cast<size_t>(Piece::kExplosives)].pComponent.load(std::memory_order_relaxed));
    if (!pComponent)
        return explosives;

    explosives.Valid = true;
    explosives.First = Read<uint32_t>(pComponent, kExplosivesFirst);
    explosives.Second = Read<uint32_t>(pComponent, kExplosivesSecond);
    return explosives;
}

void GameHud::GetCompassMarkers(std::vector<CompassMarker>& aOut) noexcept
{
    aOut.clear();
    uint8_t* pModel = GetModel();
    if (!pModel)
        return;

    using TInit = void*(void*);
    using TCopy = void(void*, void*);
    using TFree = void(void*);
    POINTER_FALLOUT4(TInit, s_init, 822532);
    POINTER_FALLOUT4(TCopy, s_copy, 1195000);
    POINTER_FALLOUT4(TFree, s_free, 588306);

    // The compass's own steps (HUDCompass's update): the list on this thread's scrap heap, the rest empty
    alignas(16) uint8_t local[0x40]{};
    s_init.Get()(local);
    *reinterpret_cast<uint8_t**>(local + 0x10) = local + 0x08;

    SpinLock(pModel + kMarkersLock);
    s_copy.Get()(pModel + kMarkers, local);
    SpinUnlock(pModel + kMarkersLock);

    // Walked as the compass walks it: from begin (0x30) in the first block (0x08) to end (0x28); an end at the last
    // block's (0x18) end is the start of the block after it
    uint8_t* pBegin = Read<uint8_t*>(local, 0x30);
    uint8_t* pEnd = Read<uint8_t*>(local, 0x28);
    uint8_t* pLast = Read<uint8_t*>(local, 0x18);
    uint8_t* pBlock = Read<uint8_t*>(local, 0x08);
    const auto normalise = [](uint8_t* apAt, uint8_t* apBlock) noexcept {
        if (apBlock && apAt >= apBlock + kBlockSize)
        {
            if (auto* pNext = Read<uint8_t*>(apBlock, kBlockSize))
                return pNext;
        }
        return apAt;
    };
    pEnd = normalise(pEnd, pLast);
    uint8_t* pAt = pBegin != Read<uint8_t*>(local, 0x28) && pBlock ? pBegin : pEnd;

    while (pAt && pAt != pEnd && aOut.size() < kMaxMarkers)
    {
        CompassMarker& marker = aOut.emplace_back();
        std::memcpy(marker.Raw.data(), pAt, kRecordSize);
        marker.Heading = Read<float>(pAt, 0x18);
        marker.Distance = Read<float>(pAt, 0x20);
        marker.Kind = Read<int32_t>(pAt, 0x24);
        marker.Height = Read<int32_t>(pAt, 0x28);
        marker.State = Read<int32_t>(pAt, 0x2C);
        marker.Icon = Read<int32_t>(pAt, 0x30);
        marker.Colour = Read<uint8_t>(pAt, 0x35);

        pAt += kRecordSize;
        if (pAt >= pBlock + kBlockSize)
        {
            pBlock = Read<uint8_t*>(pBlock, kBlockSize);
            if (!pBlock)
                break;
            pAt = pBlock;
        }
    }

    s_free.Get()(local);
}

std::array<float, 2> GameHud::GetCompassGlobals() noexcept
{
    POINTER_FALLOUT4(float, s_first, 1176553);
    POINTER_FALLOUT4(float, s_second, 896828);
    return {*s_first.Get(), *s_second.Get()};
}

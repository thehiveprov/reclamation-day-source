#if TP_FALLOUT4

// The settlements' day, the client's side of WorkshopService (see WorkshopService.h and the server's WorkshopDays.cpp)

#include <Services/WorkshopService.h>

#include <World.h>
#include <TESObjectREFR.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/BGSInventoryList.h>
#include <Interface/MainMenuIntegration.h>
#include <Interface/UI.h>
#include <Messages/RequestSettlementProduction.h>
#include <Messages/RequestSettlementRatings.h>
#include <PapyrusCall.h>
#include <Services/TransportService.h>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// WorkshopParent's WorkshopRatings (Fallout4.esm 0002058E, WorkshopDataScript:WorkshopRatingKeyword resourceValue, read
// from its VMAD, docs/SYNC_PASS.md gap 7): the actor values on a workbench that are its ratings, in that list's order.
// Food, happiness, population, safety, water, power, beds, bonus happiness, unassigned, radio, damage (current, max,
// days since the last attack, food, water, safety, power, population), happiness modifier, food actual, happiness
// target, artillery, artillery damage, last attack faction, robots, vendor income, brahmin, missing food, water and
// beds, scavenge (general, building, parts, rare), caravan, the eight food types, synths, missing safety.
constexpr uint32_t kRatingValues[] = {0x000331, 0x129157, 0x12723E, 0x000333, 0x000332, 0x00032E, 0x000334, 0x12722C, 0x127240, 0x127241, 0x12722F, 0x127231,
                                      0x127239, 0x127230, 0x127235, 0x127234, 0x127233, 0x127232, 0x127237, 0x127236, 0x127238, 0x000336, 0x12722E, 0x12723A,
                                      0x12723F, 0x10C847, 0x12722D, 0x12723C, 0x12723D, 0x12723B, 0x086748, 0x086749, 0x08674A, 0x08674B, 0x0A46FD, 0x0A46FF,
                                      0x0A4700, 0x0A4701, 0x0A4702, 0x0A4703, 0x0A4704, 0x0A4705, 0x0A4706, 0x0CFF76, 0x1E3272};
constexpr size_t kRatingCount = std::size(kRatingValues);

// The hold is set again this often: a build-mode update (DailyUpdate(false)) clears it at its end
constexpr auto kHoldInterval = 500ms;
// The host's ratings: looked at this often, the changed ones sent; all of them again this often
constexpr auto kRatingsInterval = 2s;
constexpr auto kRatingsFullInterval = 60s;
// Production: workbenches looked at this often; after the host used a workbench (build mode, a container or a trade),
// this long before what's in it counts as the day's again
constexpr auto kProductionInterval = 2s;
constexpr auto kAfterUse = 3s;
// Values this close are the same (floats from the wire)
constexpr float kSameValue = 0.001f;

uint64_t DayKey(const GameId& acId) noexcept
{
    return (static_cast<uint64_t>(acId.ModId) << 32) | acId.BaseId;
}

// The inventory list's lock, taken as ExtraDataList.cpp does (the game's lock_read, given back by a decrement): the
// day adds to the workbench from the VM's thread
struct InventoryReadLock
{
    explicit InventoryReadLock(BSReadWriteLock& aLock) noexcept
        : m_lock(aLock)
    {
        // BSReadWriteLock::lock_read (CommonLibF4 id 1573164)
        TP_THIS_FUNCTION(TLockRead, void, BSReadWriteLock);
        POINTER_FALLOUT4(TLockRead, s_lockRead, 1573165);
        TiltedPhoques::ThisCall(s_lockRead, &m_lock);
    }

    ~InventoryReadLock() { InterlockedDecrement(reinterpret_cast<volatile LONG*>(&m_lock.uiLock)); }

private:
    BSReadWriteLock& m_lock;
};

// How many of each object a container holds, all stacks together
TiltedPhoques::Map<uint32_t, int32_t> CountItems(TESObjectREFR* apRef) noexcept
{
    TiltedPhoques::Map<uint32_t, int32_t> counts;
    BGSInventoryList* pList = apRef->pInventoryList;
    if (!pList)
        return counts;

    InventoryReadLock _{pList->RWLock};
    for (const BGSInventoryItem& item : pList->DataA)
    {
        if (!item.pObject)
            continue;
        int32_t count = 0;
        for (const BGSInventoryItem::Stack* pStack = item.spStackData; pStack; pStack = pStack->spNextStack)
            count += static_cast<int32_t>(pStack->uiCount);
        counts[item.pObject->formID] += count;
    }
    return counts;
}

bool IsUsingAContainer() noexcept
{
    static const BSFixedString s_container("ContainerMenu");
    static const BSFixedString s_barter("BarterMenu");
    UI* pUi = UI::Get();
    return pUi && (pUi->GetMenuOpen(s_container) || pUi->GetMenuOpen(s_barter));
}
} // namespace

void WorkshopService::RunSettlementDay() noexcept
{
    const auto now = Clock::now();
    const bool connected = m_transport.IsConnected();
    const bool hosting = connected && IsHostingWorld();

    // This game holds its day while another's (the host's) runs it
    const bool hold = connected && !hosting && m_dayHostId != 0 && m_dayHostId != m_transport.GetLocalPlayerId();
    if ((hold || m_dayHeld) && now - m_dayHeldAt >= kHoldInterval)
        HoldDay(hold);

    if (!connected)
        return;

    if (hosting)
    {
        SendRatings(now);
        RunProduction(now);
        return;
    }

    // Ratings for workbenches this game didn't have yet (a C.A.M.P.'s module not built here)
    if (!m_pendingRatings.empty() && now - m_ratingsSentAt >= kRatingsInterval)
    {
        m_ratingsSentAt = now;
        for (auto it = m_pendingRatings.begin(); it != m_pendingRatings.end();)
            it = ApplyRatings(it->second) ? m_pendingRatings.erase(it) : std::next(it);
    }
    for (auto it = m_pendingProduction.begin(); it != m_pendingProduction.end();)
        it = GiveProduction(*it) ? m_pendingProduction.erase(it) : std::next(it);
}

void WorkshopService::HoldDay(bool aHold) noexcept
{
    // Tried at most every kHoldInterval, also when the call can't be made
    m_dayHeldAt = Clock::now();
    if (!PapyrusCall::Static("ReclamationDay:Settlements", "Hold", {aHold}))
        return;

    if (aHold != m_dayHeld)
        spdlog::info("Settlements: {}", aHold ? "the host's game runs the day, this one's is held" : "this game runs its own day again");
    m_dayHeld = aHold;
}

void WorkshopService::SendRatings(Clock::time_point aNow) noexcept
{
    if (m_daySentHello && aNow - m_ratingsSentAt < kRatingsInterval)
        return;
    m_ratingsSentAt = aNow;

    const bool full = !m_daySentHello || aNow - m_ratingsFullAt >= kRatingsFullInterval;
    if (full)
        m_ratingsFullAt = aNow;

    RequestSettlementRatings request;
    request.Full = full;
    for (const SettlementClaim& claim : m_claims)
    {
        TESObjectREFR* pWorkshop = GetWorkshopRef(claim.WorkshopId);
        if (!pWorkshop)
            continue;

        SettlementRatings ratings;
        ratings.WorkshopId = claim.WorkshopId;
        ratings.Values.reserve(kRatingCount * 2);
        for (const uint32_t id : kRatingValues)
        {
            auto* pInfo = Cast<ActorValueInfo>(TESForm::GetById(id));
            ratings.Values.push_back(pInfo ? pWorkshop->actorValueOwner.GetBaseValue(pInfo) : 0.f);
            ratings.Values.push_back(pInfo ? pWorkshop->actorValueOwner.GetValue(pInfo) : 0.f);
        }

        Vector<float>& sent = m_sentRatings[DayKey(claim.WorkshopId)];
        if (!full && sent == ratings.Values)
            continue;
        sent = ratings.Values;
        request.Settlements.push_back(std::move(ratings));
    }

    // The first, even empty, tells the others who runs the day
    if (!request.Settlements.empty() || !m_daySentHello)
        m_transport.Send(request);
    if (!m_daySentHello)
        spdlog::info("Settlements: this game runs the day for everyone ({} claimed settlements here)", request.Settlements.size());
    m_daySentHello = true;
}

void WorkshopService::OnNotifySettlementRatings(const NotifySettlementRatings& acMessage) noexcept
{
    if (acMessage.HostPlayerId != m_dayHostId)
    {
        m_dayHostId = acMessage.HostPlayerId;
        if (!m_dayHostId)
            m_pendingRatings.clear();
    }

    for (const SettlementRatings& ratings : acMessage.Settlements)
    {
        if (!ApplyRatings(ratings))
            m_pendingRatings[DayKey(ratings.WorkshopId)] = ratings;
        else
            m_pendingRatings.erase(DayKey(ratings.WorkshopId));
    }
}

bool WorkshopService::ApplyRatings(const SettlementRatings& acRatings) noexcept
{
    TESObjectREFR* pWorkshop = GetWorkshopRef(acRatings.WorkshopId);
    if (!pWorkshop)
        return false;

    // As WorkshopParentScript.SetResourceData does, through the slots Papyrus' natives call (read from the exe): the
    // base value set (SetValue, slot 9), then damage, the current value below it, as the host's game has it (DamageValue:
    // slot 6 with mode DAMAGE; RestoreValue: slot 8)
    ActorValueOwner& values = pWorkshop->actorValueOwner;
    const size_t count = std::min(kRatingCount, acRatings.Values.size() / 2);
    for (size_t i = 0; i < count; ++i)
    {
        auto* pInfo = Cast<ActorValueInfo>(TESForm::GetById(kRatingValues[i]));
        if (!pInfo)
            continue;

        const float base = acRatings.Values[i * 2];
        const float current = acRatings.Values[i * 2 + 1];
        if (std::abs(values.GetBaseValue(pInfo) - base) > kSameValue)
            values.SetValue(pInfo, base);

        const float change = current - values.GetValue(pInfo);
        if (change < -kSameValue)
            values.ForceCurrent(ActorValueOwner::ForceMode::DAMAGE, pInfo, change);
        else if (change > kSameValue)
            values.RestoreValue(pInfo, change);
    }
    return true;
}

void WorkshopService::RunProduction(Clock::time_point aNow) noexcept
{
    if (aNow - m_productionCheckedAt < kProductionInterval)
        return;
    m_productionCheckedAt = aNow;

    // What the host puts in or takes out themselves isn't the day's
    if (m_inBuildMode || IsUsingAContainer())
        m_productionBusyUntil = aNow + kAfterUse;
    if (aNow < m_productionBusyUntil)
    {
        m_productionWatch.clear();
        return;
    }

    Set<uint32_t> watched;
    for (const SettlementClaim& claim : m_claims)
    {
        // Host or leader: a settlement led by another player who's here gets its day's production in their game
        if (!claim.LeaderPlayerId || claim.LeaderName == m_localName)
            continue;
        TESObjectREFR* pWorkshop = GetWorkshopRef(claim.WorkshopId);
        if (!pWorkshop)
            continue;
        watched.insert(pWorkshop->formID);

        TiltedPhoques::Map<uint32_t, int32_t> counts = CountItems(pWorkshop);
        const auto watch = m_productionWatch.find(pWorkshop->formID);
        if (watch == m_productionWatch.end())
        {
            m_productionWatch[pWorkshop->formID] = std::move(counts);
            continue;
        }

        RequestSettlementProduction request;
        request.WorkshopId = claim.WorkshopId;
        Vector<std::pair<uint32_t, int32_t>> taken;
        for (const auto& [formId, count] : counts)
        {
            const auto before = watch->second.find(formId);
            const int32_t added = count - (before == watch->second.end() ? 0 : before->second);
            if (added <= 0)
                continue;

            Inventory::Entry entry{};
            if (!m_world.GetModSystem().GetServerModId(formId, entry.BaseId))
                continue;
            entry.Count = -added;
            pWorkshop->AddOrRemoveItem(entry);
            taken.emplace_back(formId, added);

            entry.Count = added;
            request.Items.Entries.push_back(std::move(entry));
        }
        // What's left in it after the taking is where the next look starts
        for (const auto& [formId, added] : taken)
            counts[formId] -= added;
        m_productionWatch[pWorkshop->formID] = std::move(counts);

        if (!request.Items.Entries.empty())
        {
            spdlog::info("Settlements: the day's production at workshop {:X} ({} kinds of item) goes to its leader '{}'", pWorkshop->formID, request.Items.Entries.size(),
                         claim.LeaderName.c_str());
            m_transport.Send(request);
        }
    }

    // Those no longer led by someone else: their next look starts over
    for (auto it = m_productionWatch.begin(); it != m_productionWatch.end();)
        it = watched.contains(it->first) ? std::next(it) : m_productionWatch.erase(it);
}

void WorkshopService::OnNotifySettlementProduction(const NotifySettlementProduction& acMessage) noexcept
{
    if (!GiveProduction(acMessage) && m_pendingProduction.size() < 64)
        m_pendingProduction.push_back(acMessage);
}

bool WorkshopService::GiveProduction(const NotifySettlementProduction& acMessage) noexcept
{
    TESObjectREFR* pWorkshop = GetWorkshopRef(acMessage.WorkshopId);
    if (!pWorkshop)
        return false;

    for (const Inventory::Entry& entry : acMessage.Items.Entries)
    {
        if (entry.Count > 0)
            pWorkshop->AddOrRemoveItem(entry);
    }
    // Not the day's here (the host's game, given back): what's in it now is where the next look starts
    m_productionWatch.erase(pWorkshop->formID);
    spdlog::info("Settlements: the day's production at workshop {:X} ({} kinds of item) put in its workbench", pWorkshop->formID, acMessage.Items.Entries.size());
    return true;
}

void WorkshopService::ResetSettlementDay() noexcept
{
    m_dayHostId = 0;
    m_daySentHello = false;
    m_sentRatings.clear();
    m_pendingRatings.clear();
    m_productionWatch.clear();
    m_pendingProduction.clear();
}

#endif

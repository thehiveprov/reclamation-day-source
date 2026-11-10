#if TP_FALLOUT4

// Faction quests together, the client's side (see FactionService.h, the server's FactionService, docs/FACTIONS.md)

#include <Services/FactionService.h>

#include <World.h>
#include <Actor.h>
#include <Components.h>
#include <TESObjectREFR.h>
#include <Games/TES.h>
#include <Forms/TESQuest.h>
#include <Messages/NotifySharedQuest.h>
#include <Messages/RequestSharedQuest.h>
#include <PapyrusCall.h>
#include <Services/PapyrusService.h>
#include <Services/PartyService.h>
#include <Services/QuestService.h>
#include <Services/TransportService.h>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Standing = FactionStanding;
using Fill = SharedQuest::Fill;

// How long a party member's quest may take to start here (Quest.Start runs on the VM's time)
constexpr auto kStartTimeout = 15s;
// The starter's characters: looked for this often, and for this long after the quest was shared
constexpr auto kFillsInterval = 1s;
constexpr auto kWatchFor = 30min;

// The factions' radiant quests (read from the plugins, docs/FACTIONS.md): repeatable ones the faction gives its
// members, not its story quests
struct Radiant
{
    const char* Plugin; // null: Fallout4.esm
    uint32_t QuestId;
    Standing::Faction Faction;
};
constexpr Radiant kRadiants[] = {
    // MinRadiantOwned01-11 (settlement help: MinRadiantStart), MinRecruit00-09 (new settlements: MinRecruitmentStart)
    {nullptr, 0x0A1412, Standing::kMinutemen}, {nullptr, 0x03DF95, Standing::kMinutemen}, {nullptr, 0x03E0C1, Standing::kMinutemen},
    {nullptr, 0x0E477F, Standing::kMinutemen}, {nullptr, 0x160409, Standing::kMinutemen}, {nullptr, 0x161D1D, Standing::kMinutemen},
    {nullptr, 0x162F44, Standing::kMinutemen}, {nullptr, 0x186A08, Standing::kMinutemen}, {nullptr, 0x05E51F, Standing::kMinutemen},
    {nullptr, 0x109D73, Standing::kMinutemen}, {nullptr, 0x1533A9, Standing::kMinutemen}, {nullptr, 0x11B36E, Standing::kMinutemen},
    {nullptr, 0x098136, Standing::kMinutemen}, {nullptr, 0x099848, Standing::kMinutemen}, {nullptr, 0x106F05, Standing::kMinutemen},
    {nullptr, 0x157CA5, Standing::kMinutemen}, {nullptr, 0x15F03F, Standing::kMinutemen}, {nullptr, 0x15F040, Standing::kMinutemen},
    {nullptr, 0x186642, Standing::kMinutemen}, {nullptr, 0x157577, Standing::kMinutemen}, {nullptr, 0x164167, Standing::kMinutemen},
    // BoSR01-05
    {nullptr, 0x064EC7, Standing::kBrotherhood}, {nullptr, 0x0C30DC, Standing::kBrotherhood}, {nullptr, 0x0C8675, Standing::kBrotherhood},
    {nullptr, 0x0CF3E2, Standing::kBrotherhood}, {nullptr, 0x0D1EB2, Standing::kBrotherhood},
    // RRR01a-09
    {nullptr, 0x0B1F1D, Standing::kRailroad}, {nullptr, 0x0B1F1C, Standing::kRailroad}, {nullptr, 0x0B3E82, Standing::kRailroad},
    {nullptr, 0x0B926B, Standing::kRailroad}, {nullptr, 0x0B26C2, Standing::kRailroad}, {nullptr, 0x0B926A, Standing::kRailroad},
    {nullptr, 0x1845F7, Standing::kRailroad}, {nullptr, 0x186C79, Standing::kRailroad}, {nullptr, 0x13A33D, Standing::kRailroad},
    {nullptr, 0x145F1A, Standing::kRailroad},
    // InstR01-05
    {nullptr, 0x0F7933, Standing::kInstitute}, {nullptr, 0x0EB268, Standing::kInstitute}, {nullptr, 0x0EDCEF, Standing::kInstitute},
    {nullptr, 0x0EDE28, Standing::kInstitute}, {nullptr, 0x0F0D8C, Standing::kInstitute},
    // Nuka-World's raids: DLC04RaidWipeOut, ChaseOff, Subdue, Coerce
    {"DLCNukaWorld.esm", 0x00DB37, Standing::kNukaWorldGangs}, {"DLCNukaWorld.esm", 0x014352, Standing::kNukaWorldGangs},
    {"DLCNukaWorld.esm", 0x016E18, Standing::kNukaWorldGangs}, {"DLCNukaWorld.esm", 0x019088, Standing::kNukaWorldGangs},
};

// Alias fill types (BGSRefAlias::FILL_TYPE, CommonLibF4): Forced; alias flags (BGSBaseAlias::FLAGS at +0x1C): Optional
constexpr uint16_t kFillForced = 1;
constexpr uint32_t kAliasOptional = 1u << 1;
// Where the fill is (read from BGSRefAlias::Load and BGSLocAlias::Load / InitItem, 1.10.163): the fill type at +0x20;
// the fill data at +0x28: a forced reference's handle (32 bits: ALFR's reference through TESObjectREFR::GetHandle,
// stored where 1113342 says, +0x28 when the type is Forced), a forced location's BGSLocation* (ALFL, looked up and
// cast by InitItem). Reference collection aliases are reference aliases there (the same InitItem and forced handle).
constexpr size_t kFlagsOffset = 0x1C;
constexpr size_t kFillTypeOffset = 0x20;
constexpr size_t kFillDataOffset = 0x28;
constexpr size_t kSavedSize = 0x24; // +0x1C to +0x40: the flags, the type, then the fill data

// The alias kinds by their vtables (CommonLibF4 VTABLE BGSRefAlias 125571, BGSLocAlias 1011963, BGSRefCollectionAlias
// 978860; RD's ids are + 1)
enum class AliasKind
{
    kOther,
    kReference,
    kLocation,
    kCollection
};

AliasKind KindOf(const BGSBaseAlias* apAlias) noexcept
{
    POINTER_FALLOUT4(uint8_t, s_refAliasVtable, 125572);
    POINTER_FALLOUT4(uint8_t, s_locAliasVtable, 1011964);
    POINTER_FALLOUT4(uint8_t, s_collectionAliasVtable, 978861);
    const void* pVtable = *reinterpret_cast<void* const*>(apAlias);
    if (pVtable == static_cast<const void*>(s_refAliasVtable.Get()))
        return AliasKind::kReference;
    if (pVtable == static_cast<const void*>(s_locAliasVtable.Get()))
        return AliasKind::kLocation;
    if (pVtable == static_cast<const void*>(s_collectionAliasVtable.Get()))
        return AliasKind::kCollection;
    return AliasKind::kOther;
}

BGSBaseAlias* FindAlias(TESQuest* apQuest, uint32_t aAliasId) noexcept
{
    for (uint32_t i = 0; i < apQuest->aliases.length; ++i)
    {
        BGSBaseAlias* pAlias = apQuest->aliases.data[i];
        if (pAlias && pAlias->id == aAliasId)
            return pAlias;
    }
    return nullptr;
}

uint8_t* Bytes(BGSBaseAlias* apAlias) noexcept
{
    return reinterpret_cast<uint8_t*>(apAlias);
}

// The handle of no reference, as the game's alias loader sets a Forced alias without one (888641)
uint32_t NoReferenceHandle() noexcept
{
    POINTER_FALLOUT4(uint32_t, s_noReference, 888642);
    return *s_noReference.Get();
}

// How many references a collection alias holds (832205: the quest's aliasedHandles for it, under its alias lock, as
// RefCollectionAlias.GetAt reads it)
uint32_t CollectionCount(TESQuest* apQuest, const BGSBaseAlias* apAlias) noexcept
{
    using TGetCount = uint32_t(TESQuest*, const BGSBaseAlias*);
    POINTER_FALLOUT4(TGetCount, s_getCount, 832206);
    return s_getCount.Get()(apQuest, apAlias);
}

// The game's alias natives (read from the exe: ReferenceAlias.GetReference NativeFunction0<BGSRefAlias,
// TESObjectREFR*>, LocationAlias.GetLocation NativeFunction0<BGSLocAlias, BGSLocation*>, RefCollectionAlias.GetAt
// NativeFunction1<BGSRefCollectionAlias, TESObjectREFR*, uint>, ReferenceAlias.ForceRefTo NativeFunction1<BGSRefAlias,
// void, TESObjectREFR*>, RefCollectionAlias.AddRef NativeFunction1<BGSRefCollectionAlias, void, TESObjectREFR*>)
struct AliasNatives
{
    PapyrusFunction<TESObjectREFR*, BGSBaseAlias> GetReference;
    PapyrusFunction<TESForm*, BGSBaseAlias> GetLocation;
    PapyrusFunction<TESObjectREFR*, BGSBaseAlias, uint32_t> GetAt;
    PapyrusFunction<void, BGSBaseAlias, TESObjectREFR*> ForceRefTo;
    PapyrusFunction<void, BGSBaseAlias, TESObjectREFR*> AddRef;
};

const AliasNatives* GetAliasNatives(PapyrusService& aPapyrus) noexcept
{
    static const char* s_names[][2] = {{"ReferenceAlias", "GetReference"}, {"LocationAlias", "GetLocation"}, {"RefCollectionAlias", "GetAt"},
                                       {"ReferenceAlias", "ForceRefTo"},   {"RefCollectionAlias", "AddRef"}};
    for (const auto& name : s_names)
    {
        if (!aPapyrus.Get(name[0], name[1]))
            return nullptr;
    }
    static const AliasNatives s_natives{aPapyrus.Get("ReferenceAlias", "GetReference"), aPapyrus.Get("LocationAlias", "GetLocation"),
                                        aPapyrus.Get("RefCollectionAlias", "GetAt"), aPapyrus.Get("ReferenceAlias", "ForceRefTo"),
                                        aPapyrus.Get("RefCollectionAlias", "AddRef")};
    return &s_natives;
}
} // namespace

FactionStanding::Faction FactionService::RadiantFaction(uint32_t aQuestId) noexcept
{
    if (!m_radiantsRead)
    {
        for (const Radiant& radiant : kRadiants)
        {
            uint32_t formId = radiant.QuestId;
            if (radiant.Plugin)
            {
                const Mod* pMod = ModManager::Get()->GetByName(radiant.Plugin);
                formId = pMod && pMod->IsLoaded() ? pMod->GetFormId(radiant.QuestId) : 0;
            }
            if (formId)
                m_radiants[formId] = radiant.Faction;
        }
        m_radiantsRead = true;
    }
    const auto it = m_radiants.find(aQuestId);
    return it == m_radiants.end() ? Standing::kCount : it->second;
}

BSTEventResult FactionService::OnEvent(const TESQuestStartStopEvent* apEvent, const EventDispatcher<TESQuestStartStopEvent>*)
{
    // The game's thread that started or stopped it: looked at from the main loop
    if (apEvent)
        m_world.GetRunner().Queue([this, questId = apEvent->formId]() { OnQuestStarted(questId); });
    return BSTEventResult::kOk;
}

void FactionService::OnQuestStarted(uint32_t aQuestId) noexcept
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aQuestId));
    if (!pQuest || !pQuest->IsRunning())
        return;

    // Started here for a party member: its aliases back, and the member's invite accepted
    uint32_t starterId = 0;
    bool mirrored = false;
    {
        std::scoped_lock _{m_mirrorsLock};
        if (const auto it = m_mirrors.find(aQuestId); it != m_mirrors.end())
        {
            starterId = it->second.StarterId;
            mirrored = true;
        }
    }
    if (mirrored)
    {
        RestoreAliases(aQuestId);
        m_world.ctx().at<QuestService>().TeamUp(aQuestId, starterId, false);
        spdlog::info("Factions: quest {:X} ({}) started here with player {}'s places and targets", aQuestId, pQuest->GetName(), starterId);
        return;
    }

    const Standing::Faction faction = RadiantFaction(aQuestId);
    if (faction != Standing::kCount && m_transport.IsConnected() && m_local.IsMember(faction))
        ShareQuest(pQuest, faction);
}

void FactionService::ShareQuest(TESQuest* apQuest, FactionStanding::Faction aFaction) noexcept
{
    // The party members in the same faction (the server checks again)
    auto& party = m_world.GetPartyService();
    if (!party.IsInParty())
        return;
    const uint32_t localId = m_transport.GetLocalPlayerId();
    Vector<uint32_t> members;
    for (const uint32_t playerId : party.GetPartyMembers())
    {
        const FactionStanding* pStanding = GetStanding(playerId);
        if (playerId != localId && pStanding && pStanding->IsMember(aFaction))
            members.push_back(playerId);
    }
    if (members.empty())
        return;

    const AliasNatives* pNatives = GetAliasNatives(m_world.ctx().at<PapyrusService>());
    RequestSharedQuest request;
    if (!pNatives || !m_world.GetModSystem().GetServerModId(apQuest->formID, request.Quest.QuestId))
        return;
    request.Quest.Faction = aFaction;

    Sharing sharing{};
    sharing.QuestId = request.Quest.QuestId;
    sharing.Faction = aFaction;
    sharing.Until = Clock::now() + kWatchFor;

    // A reference as every game knows it: the world's own by its id; a character made while playing by its server id,
    // or, until the character sync has it, as one to wait for. An object made while playing isn't sent: each game makes
    // its own (loot is each player's), at the same place
    const auto addReference = [&](uint32_t aAliasId, uint8_t aType, TESObjectREFR* apRef)
    {
        if (!apRef)
            return;
        Fill fill{};
        fill.AliasId = aAliasId;
        fill.Type = aType;
        if (apRef->formID < 0xFF000000 && m_world.GetModSystem().GetServerModId(apRef->formID, fill.FormId))
        {
            request.Quest.Fills.push_back(fill);
            return;
        }
        if (!Cast<Actor>(apRef))
            return;
        fill.Character = true;
        fill.ServerId = FindServerId(apRef->formID);
        if (!fill.ServerId)
            sharing.Pending.push_back({aAliasId, aType, apRef->formID});
        request.Quest.Fills.push_back(fill);
    };

    for (uint32_t i = 0; i < apQuest->aliases.length; ++i)
    {
        BGSBaseAlias* pAlias = apQuest->aliases.data[i];
        switch (pAlias ? KindOf(pAlias) : AliasKind::kOther)
        {
        case AliasKind::kReference: addReference(pAlias->id, Fill::kReference, pNatives->GetReference(pAlias)); break;
        case AliasKind::kCollection:
        {
            const uint32_t count = CollectionCount(apQuest, pAlias);
            for (uint32_t entry = 0; entry < count && request.Quest.Fills.size() < SharedQuest::kMaxFills; ++entry)
                addReference(pAlias->id, Fill::kCollectionEntry, pNatives->GetAt(pAlias, entry));
            break;
        }
        case AliasKind::kLocation:
        {
            Fill fill{};
            fill.AliasId = pAlias->id;
            fill.Type = Fill::kLocation;
            TESForm* pLocation = pNatives->GetLocation(pAlias);
            if (pLocation && m_world.GetModSystem().GetServerModId(pLocation->formID, fill.FormId))
                request.Quest.Fills.push_back(fill);
            break;
        }
        default: break;
        }
    }

    m_transport.Send(request);
    auto& quests = m_world.ctx().at<QuestService>();
    for (const uint32_t playerId : members)
        quests.TeamUp(apQuest->formID, playerId, true);
    spdlog::info("Factions: quest {:X} ({}) shared with {} party member(s): {} fills, {} characters to send once synced", apQuest->formID, apQuest->GetName(), members.size(),
                 request.Quest.Fills.size(), sharing.Pending.size());
    if (!sharing.Pending.empty())
        m_sharing[apQuest->formID] = std::move(sharing);
}

void FactionService::OnNotifySharedQuest(const NotifySharedQuest& acMessage) noexcept
{
    const SharedQuest& shared = acMessage.Quest;
    const uint32_t questId = m_world.GetModSystem().GetGameId(shared.QuestId);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(questId));
    if (!pQuest)
    {
        spdlog::warn("Factions: player {}'s shared quest {:X}:{:X} isn't in this game", acMessage.StarterId, shared.QuestId.ModId, shared.QuestId.BaseId);
        return;
    }

    // The starter's characters synced since: their server ids, put in the aliases once they're here
    if (shared.Update)
    {
        auto it = m_waiting.find(questId);
        if (it == m_waiting.end())
            return;
        Vector<Waiting>& entries = it.value();
        for (const Fill& fill : shared.Fills)
        {
            const auto waiting = std::find_if(entries.begin(), entries.end(), [&fill](const Waiting& acWaiting)
                                              { return acWaiting.AliasId == fill.AliasId && acWaiting.Type == fill.Type && !acWaiting.ServerId && acWaiting.FormId == GameId{}; });
            if (waiting != entries.end())
                waiting->ServerId = fill.ServerId;
            else
                entries.push_back({fill.AliasId, fill.Type, fill.ServerId, fill.FormId});
        }
        return;
    }

    if (pQuest->IsRunning())
    {
        // Already doing it (its own places): together anyway, the team keeps the stages together
        m_world.ctx().at<QuestService>().TeamUp(questId, acMessage.StarterId, false);
        spdlog::info("Factions: player {}'s shared quest {:X} already runs here", acMessage.StarterId, questId);
        return;
    }

    Mirror mirror{};
    mirror.StarterId = acMessage.StarterId;
    mirror.GiveUpAt = Clock::now() + kStartTimeout;
    Vector<Waiting> waiting;
    size_t forced = 0;

    const auto save = [&mirror](BGSBaseAlias* apAlias)
    {
        if (std::any_of(mirror.Saved.begin(), mirror.Saved.end(), [apAlias](const auto& acSaved) { return acSaved.first == apAlias; }))
            return false;
        std::array<uint8_t, kSavedSize> saved{};
        std::memcpy(saved.data(), Bytes(apAlias) + kFlagsOffset, kSavedSize);
        mirror.Saved.emplace_back(apAlias, saved);
        return true;
    };
    const auto force = [](BGSBaseAlias* apAlias, uint64_t aData, bool aOptional)
    {
        *reinterpret_cast<uint16_t*>(Bytes(apAlias) + kFillTypeOffset) = kFillForced;
        *reinterpret_cast<uint64_t*>(Bytes(apAlias) + kFillDataOffset) = aData;
        if (aOptional)
            *reinterpret_cast<uint32_t*>(Bytes(apAlias) + kFlagsOffset) |= kAliasOptional;
    };
    // Held empty for the start (no reference, Optional): this game neither makes nor finds its own
    const auto hold = [&](BGSBaseAlias* apAlias)
    {
        if (save(apAlias))
            force(apAlias, NoReferenceHandle(), true);
    };

    for (const Fill& fill : shared.Fills)
    {
        BGSBaseAlias* pAlias = FindAlias(pQuest, fill.AliasId);
        if (!pAlias)
            continue;
        const AliasKind kind = KindOf(pAlias);

        if (fill.Type == Fill::kLocation && kind == AliasKind::kLocation)
        {
            TESForm* pLocation = TESForm::GetById(m_world.GetModSystem().GetGameId(fill.FormId));
            if (pLocation && save(pAlias))
            {
                force(pAlias, reinterpret_cast<uint64_t>(pLocation), false);
                ++forced;
            }
        }
        else if (fill.Type == Fill::kReference && kind == AliasKind::kReference)
        {
            const uint32_t formId = fill.Character ? FindFormId(fill.ServerId) : m_world.GetModSystem().GetGameId(fill.FormId);
            auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
            if (pRef && save(pAlias))
            {
                force(pAlias, pRef->GetHandle().handle.iBits, false);
                ++forced;
            }
            else if (fill.Character)
            {
                // The starter's character, not in this game (yet): held, put in once it is
                hold(pAlias);
                waiting.push_back({fill.AliasId, fill.Type, fill.ServerId, {}});
            }
        }
        else if (fill.Type == Fill::kCollectionEntry && kind == AliasKind::kCollection)
        {
            // The collection is the starter's: held empty, its references added once the quest runs
            hold(pAlias);
            waiting.push_back({fill.AliasId, fill.Type, fill.ServerId, fill.Character ? GameId{} : fill.FormId});
        }
    }

    {
        std::scoped_lock _{m_mirrorsLock};
        m_mirrors[questId] = std::move(mirror);
    }
    if (!PapyrusCall::Static("ReclamationDay:Factions", "StartShared", {static_cast<int32_t>(questId)}))
    {
        RestoreAliases(questId);
        return;
    }
    const size_t toCome = waiting.size();
    if (!waiting.empty())
        m_waiting[questId] = std::move(waiting);
    spdlog::info("Factions: starting player {}'s shared quest {:X} ({}) here, {} of {} fills forced, {} of the starter's characters to come", acMessage.StarterId, questId,
                 pQuest->GetName(), forced, shared.Fills.size(), toCome);
}

void FactionService::RunSharedFills(Clock::time_point aNow) noexcept
{
    if (aNow - m_fillsCheckedAt < kFillsInterval || !m_transport.IsConnected())
        return;
    m_fillsCheckedAt = aNow;

    // The starter: characters the character sync has picked up since
    for (auto it = m_sharing.begin(); it != m_sharing.end();)
    {
        auto* pQuest = Cast<TESQuest>(TESForm::GetById(it->first));
        Sharing& sharing = it.value();
        if (!pQuest || !pQuest->IsRunning() || aNow > sharing.Until)
        {
            it = m_sharing.erase(it);
            continue;
        }

        RequestSharedQuest update;
        update.Quest.QuestId = sharing.QuestId;
        update.Quest.Faction = sharing.Faction;
        update.Quest.Update = true;
        for (auto watched = sharing.Pending.begin(); watched != sharing.Pending.end();)
        {
            const uint32_t serverId = FindServerId(watched->FormId);
            if (!serverId)
            {
                ++watched;
                continue;
            }
            Fill fill{};
            fill.AliasId = watched->AliasId;
            fill.Type = watched->Type;
            fill.ServerId = serverId;
            fill.Character = true;
            update.Quest.Fills.push_back(fill);
            watched = sharing.Pending.erase(watched);
        }
        if (!update.Quest.Fills.empty())
        {
            m_transport.Send(update);
            spdlog::info("Factions: quest {:X}: {} of its characters synced since, sent", it->first, update.Quest.Fills.size());
        }
        it = sharing.Pending.empty() ? m_sharing.erase(it) : std::next(it);
    }

    // The others: the starter's characters into the aliases, once the quest runs here and they're here
    const AliasNatives* pNatives = m_waiting.empty() ? nullptr : GetAliasNatives(m_world.ctx().at<PapyrusService>());
    for (auto it = m_waiting.begin(); pNatives && it != m_waiting.end();)
    {
        auto* pQuest = Cast<TESQuest>(TESForm::GetById(it->first));
        bool starting = false;
        {
            std::scoped_lock _{m_mirrorsLock};
            starting = m_mirrors.count(it->first) != 0;
        }
        if (!pQuest || (!starting && !pQuest->IsRunning()))
        {
            it = m_waiting.erase(it);
            continue;
        }
        if (starting)
        {
            ++it;
            continue;
        }

        Vector<Waiting>& entries = it.value();
        for (auto waiting = entries.begin(); waiting != entries.end();)
        {
            const uint32_t formId = waiting->ServerId ? FindFormId(waiting->ServerId) : (waiting->FormId != GameId{} ? m_world.GetModSystem().GetGameId(waiting->FormId) : 0);
            auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
            BGSBaseAlias* pAlias = pRef ? FindAlias(pQuest, waiting->AliasId) : nullptr;
            if (!pAlias)
            {
                ++waiting;
                continue;
            }
            if (waiting->Type == Fill::kCollectionEntry)
                pNatives->AddRef(pAlias, pRef);
            else
                pNatives->ForceRefTo(pAlias, pRef);
            spdlog::info("Factions: quest {:X}: the starter's {:X} is in alias {} here", it->first, pRef->formID, waiting->AliasId);
            waiting = entries.erase(waiting);
        }
        it = entries.empty() ? m_waiting.erase(it) : std::next(it);
    }
}

void FactionService::RestoreAliases(uint32_t aQuestId) noexcept
{
    Mirror mirror;
    {
        std::scoped_lock _{m_mirrorsLock};
        const auto it = m_mirrors.find(aQuestId);
        if (it == m_mirrors.end())
            return;
        mirror = std::move(it->second);
        m_mirrors.erase(it);
    }
    for (const auto& [pAlias, saved] : mirror.Saved)
        std::memcpy(Bytes(pAlias) + kFlagsOffset, saved.data(), kSavedSize);
}

uint32_t FactionService::FindServerId(uint32_t aFormId) const noexcept
{
    const auto view = m_world.view<FormIdComponent>();
    for (const auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id != aFormId)
            continue;
        if (const auto* pLocal = m_world.try_get<LocalComponent>(entity))
            return pLocal->Id;
        if (const auto* pRemote = m_world.try_get<RemoteComponent>(entity))
            return pRemote->Id;
    }
    return 0;
}

uint32_t FactionService::FindFormId(uint32_t aServerId) const noexcept
{
    if (!aServerId)
        return 0;
    const auto view = m_world.view<FormIdComponent>();
    for (const auto entity : view)
    {
        const auto* pLocal = m_world.try_get<LocalComponent>(entity);
        const auto* pRemote = m_world.try_get<RemoteComponent>(entity);
        if ((pLocal && pLocal->Id == aServerId) || (pRemote && pRemote->Id == aServerId))
            return view.get<FormIdComponent>(entity).Id;
    }
    return 0;
}

#endif

#if TP_FALLOUT4

#include <Services/FactionService.h>

#include <World.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Utils.h>
#include <Games/TES.h>
#include <Forms/TESFaction.h>
#include <Forms/TESGlobal.h>
#include <Components/PlayerComponent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyFactionStanding.h>
#include <Messages/NotifySharedQuest.h>
#include <Messages/RequestFactionStanding.h>
#include <PapyrusCall.h>
#include <Services/PapyrusService.h>
#include <Services/TransportService.h>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Standing = FactionStanding;

constexpr auto kReadInterval = 2s;
// The abandoning runs on the VM's time; asked again only if the standing still doesn't show it after this
constexpr auto kAbandonRetry = 30s;

// Fallout4.esm (docs/FACTIONS.md)
constexpr uint32_t kPlayerFaction = 0x01C21C;
constexpr uint32_t kPlayerAllyFaction = 0x106C30;
constexpr uint32_t kMinutemenFaction = 0x068043;
constexpr uint32_t kBrotherhoodFaction = 0x05DE41;
constexpr uint32_t kRailroadFaction = 0x0994F6;
constexpr uint32_t kInstituteFaction = 0x05E558;
constexpr uint32_t kGunnerFaction = 0x058303;
constexpr uint32_t kRaiderFaction = 0x01CBED;
constexpr uint32_t kMinutemenJoined = 0x0B9B82;      // PlayerMinutemen_JoinedFaction
constexpr uint32_t kBrotherhoodJoined = 0x02C028;    // BoSJoined
constexpr uint32_t kBrotherhoodKickedOut = 0x0C9B0C; // PlayerBoS_KickedOut
constexpr uint32_t kBrotherhoodDestroyed = 0x223339; // PlayerBOS_DestroyedFaction
constexpr uint32_t kRailroadJoined = 0x098135;       // PlayerRailroad_JoinedFaction
constexpr uint32_t kRailroadInFaction = 0x098134;    // PlayerRailroad_IsInFaction
constexpr uint32_t kRailroadKickedOut = 0x0B42DC;    // PlayerRailroad_KickedOut
constexpr uint32_t kInstituteJoined = 0x1E2F27;      // PlayerInstitute_Joined
constexpr uint32_t kInstituteKickedOut = 0x0C9B0D;   // PlayerInstitute_KickedOut
// DLCNukaWorld.esm: the gangs DLC04MQ01 stage 1000 puts the player in
constexpr const char* kNukaWorld = "DLCNukaWorld.esm";
constexpr uint32_t kNukaWorldGangs[] = {0x00F438, 0x00F439, 0x00F43A}; // Disciples, Operators, Pack
// Fight reactions (GROUP_REACTION::fightReaction)
constexpr int32_t kEnemy = 1;
constexpr int32_t kAlly = 2;
constexpr int32_t kFriend = 3;

// RD_Social.esp's standing factions (Tools/BuildSocialPlugin.js): fought by, then allied with, each of these
constexpr const char* kSocialPlugin = "RD_Social.esp";
constexpr uint32_t kFirstStandingFaction = 0x884;
struct Tracked
{
    const char* Plugin; // null: Fallout4.esm
    uint32_t FactionId;
    Standing::Faction Bit;
};
constexpr Tracked kTracked[] = {
    {nullptr, kMinutemenFaction, Standing::kMinutemen},    {nullptr, kBrotherhoodFaction, Standing::kBrotherhood},
    {nullptr, kRailroadFaction, Standing::kRailroad},      {nullptr, kInstituteFaction, Standing::kInstitute},
    {kNukaWorld, kNukaWorldGangs[0], Standing::kNukaWorldGangs}, {kNukaWorld, kNukaWorldGangs[1], Standing::kNukaWorldGangs},
    {kNukaWorld, kNukaWorldGangs[2], Standing::kNukaWorldGangs},
};
constexpr size_t kTrackedCount = std::size(kTracked);

bool GlobalSet(uint32_t aId) noexcept
{
    const auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(aId));
    return pGlobal && pGlobal->f >= 1.f;
}

TESFaction* GetFaction(uint32_t aId) noexcept
{
    return Cast<TESFaction>(TESForm::GetById(aId));
}

TESFaction* GetPluginFaction(const char* acpPlugin, uint32_t aLocalId) noexcept
{
    if (!acpPlugin)
        return GetFaction(aLocalId);
    const Mod* pMod = ModManager::Get()->GetByName(acpPlugin);
    return pMod && pMod->IsLoaded() ? GetFaction(pMod->GetFormId(aLocalId)) : nullptr;
}

TESFaction* GetTracked(size_t aIndex) noexcept
{
    return GetPluginFaction(kTracked[aIndex].Plugin, kTracked[aIndex].FactionId);
}

TESFaction* GetFoughtBy(size_t aIndex) noexcept
{
    return GetPluginFaction(kSocialPlugin, kFirstStandingFaction + static_cast<uint32_t>(aIndex));
}

TESFaction* GetAlliedWith(size_t aIndex) noexcept
{
    return GetPluginFaction(kSocialPlugin, kFirstStandingFaction + static_cast<uint32_t>(kTrackedCount + aIndex));
}

bool IsInFaction(Actor* apActor, TESFaction* apFaction) noexcept
{
    if (!apFaction)
        return false;
    // Actor.IsInFaction (NativeFunction1<Actor, bool, TESFaction*>, read from the exe)
    PAPYRUS_FUNCTION(bool, Actor, IsInFaction, TESFaction*);
    return s_pIsInFaction(apActor, apFaction);
}

void RemoveFromFaction(Actor* apActor, TESFaction* apFaction) noexcept
{
    // Actor.RemoveFromFaction (NativeFunction1<Actor, void, TESFaction*>, read from the exe)
    PAPYRUS_FUNCTION(void, Actor, RemoveFromFaction, TESFaction*);
    s_pRemoveFromFaction(apActor, apFaction);
}

void InFaction(Actor* apActor, TESFaction* apFaction, bool aIn) noexcept
{
    if (!apFaction || IsInFaction(apActor, apFaction) == aIn)
        return;
    if (aIn)
        apActor->SetFactionRank(apFaction, 0);
    else
        RemoveFromFaction(apActor, apFaction);
}

// How a faction's members react to the player: its reaction to PlayerFaction (TESReactionForm at TESFaction + 0x30,
// GetGroupFightReaction 167267, as the game's reaction between two actors reads it, 339469)
int32_t ReactionToPlayer(TESFaction* apFaction) noexcept
{
    TESFaction* pPlayerFaction = GetFaction(kPlayerFaction);
    if (!apFaction || !pPlayerFaction)
        return 0;
    using TGetGroupFightReaction = int32_t(const void* apReactionForm, const TESForm* apOther);
    POINTER_FALLOUT4(TGetGroupFightReaction, s_getGroupFightReaction, 167268);
    return s_getGroupFightReaction.Get()(reinterpret_cast<const uint8_t*>(apFaction) + 0x30, pPlayerFaction);
}

bool Friendly(int32_t aReaction) noexcept
{
    return aReaction == kAlly || aReaction == kFriend;
}

const char* FactionName(Standing::Faction aFaction) noexcept
{
    switch (aFaction)
    {
    case Standing::kMinutemen: return "the Minutemen";
    case Standing::kBrotherhood: return "the Brotherhood of Steel";
    case Standing::kRailroad: return "the Railroad";
    case Standing::kInstitute: return "the Institute";
    case Standing::kNukaWorldGangs: return "the Nuka-World gangs";
    case Standing::kGunners: return "the Gunners";
    case Standing::kRaiders: return "the raiders";
    default: return "a faction";
    }
}
} // namespace

FactionService::FactionService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&FactionService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&FactionService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&FactionService::OnDisconnected>(this);
    m_standingConnection = aDispatcher.sink<NotifyFactionStanding>().connect<&FactionService::OnNotifyStanding>(this);
    m_sharedQuestConnection = aDispatcher.sink<NotifySharedQuest>().connect<&FactionService::OnNotifySharedQuest>(this);
}

const FactionStanding* FactionService::GetStanding(uint32_t aPlayerId) const noexcept
{
    const auto it = m_standings.find(aPlayerId);
    return it == m_standings.end() ? nullptr : &it->second;
}

bool FactionService::ReadLocalStanding(FactionStanding& aStanding) const noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !m_world.ctx().at<PapyrusService>().Get("Actor", "IsInFaction"))
        return false;

    aStanding = {};
    const auto set = [](uint8_t& aMask, Standing::Faction aFaction, bool aOn)
    {
        if (aOn)
            aMask |= Standing::Bit(aFaction);
    };

    set(aStanding.Members, Standing::kMinutemen, GlobalSet(kMinutemenJoined) || IsInFaction(pPlayer, GetFaction(kMinutemenFaction)));
    set(aStanding.Members, Standing::kBrotherhood, GlobalSet(kBrotherhoodJoined) || IsInFaction(pPlayer, GetFaction(kBrotherhoodFaction)));
    set(aStanding.Members, Standing::kRailroad, GlobalSet(kRailroadJoined) || GlobalSet(kRailroadInFaction) || IsInFaction(pPlayer, GetFaction(kRailroadFaction)));
    set(aStanding.Members, Standing::kInstitute, GlobalSet(kInstituteJoined) || IsInFaction(pPlayer, GetFaction(kInstituteFaction)));
    set(aStanding.Members, Standing::kGunners, IsInFaction(pPlayer, GetFaction(kGunnerFaction)));
    set(aStanding.Members, Standing::kRaiders, IsInFaction(pPlayer, GetFaction(kRaiderFaction)));

    for (size_t i = 0; i < kTrackedCount; ++i)
    {
        TESFaction* pFaction = GetTracked(i);
        if (!pFaction)
            continue;
        const int32_t reaction = ReactionToPlayer(pFaction);
        set(aStanding.Hostile, kTracked[i].Bit, reaction == kEnemy);
        set(aStanding.Allied, kTracked[i].Bit, Friendly(reaction));
        if (kTracked[i].Bit == Standing::kNukaWorldGangs)
            set(aStanding.Members, Standing::kNukaWorldGangs, IsInFaction(pPlayer, pFaction));
    }
    set(aStanding.Hostile, Standing::kGunners, ReactionToPlayer(GetFaction(kGunnerFaction)) == kEnemy);
    set(aStanding.Hostile, Standing::kRaiders, ReactionToPlayer(GetFaction(kRaiderFaction)) == kEnemy);

    set(aStanding.Out, Standing::kBrotherhood, GlobalSet(kBrotherhoodKickedOut) || GlobalSet(kBrotherhoodDestroyed));
    set(aStanding.Out, Standing::kRailroad, GlobalSet(kRailroadKickedOut));
    set(aStanding.Out, Standing::kInstitute, GlobalSet(kInstituteKickedOut));
    // The Minutemen have no global of their own: a Minuteman they fight was put out (abandoning makes them enemies)
    set(aStanding.Out, Standing::kMinutemen, (aStanding.Members & Standing::Bit(Standing::kMinutemen)) && aStanding.IsHostile(Standing::kMinutemen));
    return true;
}

void FactionService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = Clock::now();
    if (!m_questSinkRegistered)
    {
        GetEventDispatcher_TESQuestStartStopEvent()->RegisterSink(this);
        m_questSinkRegistered = true;
    }
    // Shared quests that didn't start here in time: their aliases back
    Vector<uint32_t> givenUp;
    {
        std::scoped_lock _{m_mirrorsLock};
        for (const auto& [questId, mirror] : m_mirrors)
        {
            if (now > mirror.GiveUpAt)
                givenUp.push_back(questId);
        }
    }
    for (const uint32_t questId : givenUp)
    {
        spdlog::warn("Factions: shared quest {:X} didn't start here in time, its aliases are put back", questId);
        RestoreAliases(questId);
    }
    RunSharedFills(now);

    if (now - m_readAt < kReadInterval)
        return;
    m_readAt = now;

    FactionStanding standing{};
    if (!ReadLocalStanding(standing))
        return;

    const bool changed = !m_localRead || standing != m_local;
    if (changed)
        spdlog::info("Factions: the local player is in {:02X}, out of {:02X}, fought by {:02X}, allied with {:02X}", standing.Members, standing.Out, standing.Hostile,
                     standing.Allied);
    m_local = standing;
    m_localRead = true;

    RunReactivity();

    if (!m_transport.IsConnected())
        return;

    if (changed || !m_sent)
    {
        RequestFactionStanding request;
        request.Standing = m_local;
        m_transport.Send(request);
        m_sent = true;
    }

    // The other players' characters: their owners' standing (again after the character sync set their factions)
    if (!m_relationsSet)
        m_relationsSet = SetUpStandingRelations();
    if (!m_relationsSet)
        return;
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        auto* pCopy = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        const FactionStanding* pStanding = GetStanding(view.get<PlayerComponent>(entity).Id);
        if (pCopy && pCopy->formID != 0x14 && pStanding)
            ApplyStanding(pCopy, *pStanding);
    }
}

void FactionService::RunReactivity() noexcept
{
    // A Minuteman who joined the Gunners or the raiders: the Minutemen abandon them
    if (!m_local.IsMember(Standing::kMinutemen))
        return;

    Standing::Faction joined = Standing::kCount;
    for (const Standing::Faction faction : {Standing::kGunners, Standing::kRaiders, Standing::kNukaWorldGangs})
    {
        if (m_local.IsMember(faction))
        {
            joined = faction;
            break;
        }
    }
    if (joined == Standing::kCount)
        return;

    const auto now = Clock::now();
    if (m_abandonAskedAt != Clock::time_point{} && now - m_abandonAskedAt < kAbandonRetry)
        return;
    if (!PapyrusCall::Static("ReclamationDay:Factions", "AbandonMinutemen"))
        return;
    m_abandonAskedAt = now;

    const std::string message = std::string("The Minutemen have abandoned you: you joined ") + FactionName(joined) + ".";
    Utils::ShowHudMessage(message.c_str());
    spdlog::info("Factions: {}", message);
}

bool FactionService::SetUpStandingRelations() noexcept
{
    auto& papyrus = m_world.ctx().at<PapyrusService>();
    if (!papyrus.Get("Faction", "SetEnemy") || !papyrus.Get("Faction", "SetAlly") || !GetFoughtBy(0))
        return false;

    // Faction.SetEnemy / SetAlly (NativeFunction3<TESFaction, void, TESFaction*, bool, bool>, read from the exe): both
    // ways, as enemies and as allies (not friends)
    // (Papyrus' Faction is the game's TESFaction; RD's own Faction is the character sync's)
    static PapyrusFunction<void, TESFaction, TESFaction*, bool, bool> s_pSetEnemy(papyrus.Get("Faction", "SetEnemy"));
    static PapyrusFunction<void, TESFaction, TESFaction*, bool, bool> s_pSetAlly(papyrus.Get("Faction", "SetAlly"));
    size_t set = 0;
    for (size_t i = 0; i < kTrackedCount; ++i)
    {
        TESFaction* pFaction = GetTracked(i);
        TESFaction* pFoughtBy = GetFoughtBy(i);
        TESFaction* pAlliedWith = GetAlliedWith(i);
        if (!pFaction || !pFoughtBy || !pAlliedWith)
            continue; // a gang without Nuka-World
        s_pSetEnemy(pFaction, pFoughtBy, false, false);
        s_pSetAlly(pFaction, pAlliedWith, false, false);
        ++set;
    }
    spdlog::info("Factions: the standing factions' relations set for {} factions", set);
    return true;
}

void FactionService::ApplyStanding(Actor* apCopy) noexcept
{
    if (!m_relationsSet || !apCopy || apCopy->formID == 0x14)
        return;
    if (const FactionStanding* pStanding = GetStanding(FindPlayerId(apCopy)))
        ApplyStanding(apCopy, *pStanding);
}

void FactionService::ApplyStanding(Actor* apCopy, const FactionStanding& acStanding) noexcept
{
    // Fought by a faction this game's PlayerFaction is friendly with: that friendship would win in PlayerFaction
    bool conflict = false;
    for (size_t i = 0; i < kTrackedCount; ++i)
    {
        TESFaction* pFaction = GetTracked(i);
        const bool fought = pFaction && acStanding.IsHostile(kTracked[i].Bit);
        const bool allied = pFaction && !fought && (acStanding.IsAllied(kTracked[i].Bit) || acStanding.IsMember(kTracked[i].Bit));
        InFaction(apCopy, GetFoughtBy(i), fought);
        InFaction(apCopy, GetAlliedWith(i), allied);
        conflict = conflict || (fought && Friendly(ReactionToPlayer(pFaction)));
    }

    TESFaction* pPlayerFaction = GetFaction(kPlayerFaction);
    TESFaction* pPlayerAllyFaction = GetFaction(kPlayerAllyFaction);
    if (IsInFaction(apCopy, pPlayerFaction) == conflict || IsInFaction(apCopy, pPlayerAllyFaction) != conflict)
    {
        InFaction(apCopy, pPlayerFaction, !conflict);
        InFaction(apCopy, pPlayerAllyFaction, conflict);
        spdlog::info("Factions: player character {:X} is in {} (a faction friendly with this player fights theirs: {})", apCopy->formID,
                     conflict ? "PlayerAllyFaction" : "PlayerFaction", conflict);
    }
}

uint32_t FactionService::FindPlayerId(const Actor* apCopy) const noexcept
{
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == apCopy->formID)
            return view.get<PlayerComponent>(entity).Id;
    }
    return 0;
}

void FactionService::OnConnected(const ConnectedEvent&) noexcept
{
    m_sent = false;
}

void FactionService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_sent = false;
    m_standings.clear();
    m_sharing.clear();
    m_waiting.clear();
}

void FactionService::OnNotifyStanding(const NotifyFactionStanding& acMessage) noexcept
{
    m_standings[acMessage.PlayerId] = acMessage.Standing;
    spdlog::info("Factions: player {} is in {:02X}, out of {:02X}, fought by {:02X}, allied with {:02X}", acMessage.PlayerId, acMessage.Standing.Members, acMessage.Standing.Out,
                 acMessage.Standing.Hostile, acMessage.Standing.Allied);
}

#endif

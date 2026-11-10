#include <Services/FactionService.h>

#include <World.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Messages/NotifyFactionStanding.h>
#include <Messages/NotifySharedQuest.h>

FactionService::FactionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_standingConnection = aDispatcher.sink<PacketEvent<RequestFactionStanding>>().connect<&FactionService::OnStanding>(this);
    m_sharedQuestConnection = aDispatcher.sink<PacketEvent<RequestSharedQuest>>().connect<&FactionService::OnSharedQuest>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&FactionService::OnPlayerJoin>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&FactionService::OnPlayerLeave>(this);
}

const FactionStanding* FactionService::GetStanding(uint32_t aPlayerId) const noexcept
{
    const auto it = m_standings.find(aPlayerId);
    return it == m_standings.end() ? nullptr : &it->second;
}

void FactionService::OnStanding(const PacketEvent<RequestFactionStanding>& acMessage) noexcept
{
    Player& player = *acMessage.pPlayer;
    const FactionStanding& standing = acMessage.Packet.Standing;
    m_standings[player.GetId()] = standing;
    spdlog::info("Factions: player {} '{}' is in {:02X}, out of {:02X}, fought by {:02X}, allied with {:02X}", player.GetId(), player.GetUsername().c_str(), standing.Members,
                 standing.Out, standing.Hostile, standing.Allied);

    NotifyFactionStanding notify;
    notify.PlayerId = player.GetId();
    notify.Standing = standing;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != &player)
            pPlayer->Send(notify);
    }
}

void FactionService::OnSharedQuest(const PacketEvent<RequestSharedQuest>& acMessage) noexcept
{
    Player& starter = *acMessage.pPlayer;
    const SharedQuest& quest = acMessage.Packet.Quest;
    const auto& partyId = starter.GetParty().JoinedPartyId;
    if (!partyId || quest.Faction >= FactionStanding::kCount)
        return;

    const auto faction = static_cast<FactionStanding::Faction>(quest.Faction);
    NotifySharedQuest notify;
    notify.StarterId = starter.GetId();
    notify.Quest = quest;
    size_t sent = 0;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == &starter || pPlayer->GetParty().JoinedPartyId != partyId)
            continue;
        const FactionStanding* pStanding = GetStanding(pPlayer->GetId());
        if (!pStanding || !pStanding->IsMember(faction))
            continue;
        pPlayer->Send(notify);
        ++sent;
    }
    spdlog::info("Factions: player {} '{}' got quest {:X}:{:X} of faction {} ({} fills), passed to {} party member(s) in it", starter.GetId(), starter.GetUsername().c_str(),
                 quest.QuestId.ModId, quest.QuestId.BaseId, quest.Faction, quest.Fills.size(), sent);
}

void FactionService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    for (const auto& [playerId, standing] : m_standings)
    {
        if (playerId == acEvent.pPlayer->GetId())
            continue;
        NotifyFactionStanding notify;
        notify.PlayerId = playerId;
        notify.Standing = standing;
        acEvent.pPlayer->Send(notify);
    }
}

void FactionService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    m_standings.erase(acEvent.pPlayer->GetId());
}

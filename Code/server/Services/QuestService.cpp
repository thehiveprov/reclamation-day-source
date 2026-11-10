#include <GameServer.h>
#include <Components.h>

#include <World.h>
#include <Services/QuestService.h>

#include <Events/PlayerLeaveEvent.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/RequestQuestTeam.h>
#include <Messages/NotifyQuestTeam.h>

namespace
{
bool Contains(const Vector<uint32_t>& acIds, uint32_t aId) noexcept
{
    return std::find(acIds.begin(), acIds.end(), aId) != acIds.end();
}

void Erase(Vector<uint32_t>& aIds, uint32_t aId) noexcept
{
    aIds.erase(std::remove(aIds.begin(), aIds.end(), aId), aIds.end());
}

// Both ascending
bool IsSubset(const Vector<uint16_t>& acSmall, const Vector<uint16_t>& acLarge) noexcept
{
    return std::includes(acLarge.begin(), acLarge.end(), acSmall.begin(), acSmall.end());
}

void InsertStage(Vector<uint16_t>& aStages, uint16_t aStage) noexcept
{
    const auto it = std::lower_bound(aStages.begin(), aStages.end(), aStage);
    if (it == aStages.end() || *it != aStage)
        aStages.insert(it, aStage);
}

Vector<uint16_t> Sorted(Vector<uint16_t> aStages) noexcept
{
    std::sort(aStages.begin(), aStages.end());
    aStages.erase(std::unique(aStages.begin(), aStages.end()), aStages.end());
    return aStages;
}

void SendTo(uint32_t aPlayerId, const ServerMessage& acMessage) noexcept
{
    if (Player* pPlayer = GameServer::Get()->GetWorld().GetPlayerManager().GetById(aPlayerId))
        pPlayer->Send(acMessage);
}
} // namespace

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
    m_questTeamConnection = aDispatcher.sink<PacketEvent<RequestQuestTeam>>().connect<&QuestService::OnQuestTeam>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&QuestService::OnPlayerLeave>(this);
}

const QuestService::Team* QuestService::GetTeam(const GameId& acQuestId, uint32_t aPlayerId) const noexcept
{
    for (const auto& [id, team] : m_teams)
    {
        if (team.QuestId == acQuestId && (Contains(team.Participants, aPlayerId) || Contains(team.Helpers, aPlayerId)))
            return &team;
    }

    return nullptr;
}

QuestService::Team* QuestService::FindTeam(const GameId& acQuestId, uint32_t aPlayerId) noexcept
{
    return const_cast<Team*>(GetTeam(acQuestId, aPlayerId));
}

// Every player's quest stages come here (their own progress); they only reach the players they're teamed up with.
void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    Player* pPlayer = acMessage.pPlayer;
    const uint32_t playerId = pPlayer->GetId();

    auto& entries = pPlayer->GetQuestLogComponent().QuestContent.Entries;
    auto questIt = std::find_if(entries.begin(), entries.end(), [&message](const auto& e) { return e.Id == message.Id; });
    if (message.Status == RequestQuestUpdate::Stopped)
    {
        if (questIt != entries.end())
            entries.erase(questIt);
    }
    else if (questIt == entries.end())
        entries.push_back({message.Id, message.Stage});
    else
        questIt->Stage = message.Stage;

    // An invite not accepted yet: the team will start from where the inviter is by then
    if (message.Status == RequestQuestUpdate::StageUpdate)
    {
        for (auto& invite : m_invites)
        {
            if (invite.InviterId == playerId && invite.QuestId == message.Id)
                InsertStage(invite.InviterStages, message.Stage);
        }
    }

    Team* pTeam = FindTeam(message.Id, playerId);
    if (!pTeam)
        return;

    if (message.Status == RequestQuestUpdate::Stopped)
    {
        // A party's main quest team keeps who finished it: who joins the party later catches up to them
        if (pTeam->PartyId)
            return;

        // Finished (or failed) it: nothing left to share
        spdlog::info("Player {} is done with quest {:X}:{:X}, leaving its team", playerId, message.Id.ModId, message.Id.BaseId);
        for (const auto& [id, team] : m_teams)
        {
            if (&team == pTeam)
            {
                RemoveFromTeam(id, playerId);
                break;
            }
        }
        return;
    }

    // Helpers' own progress isn't the team's. A quest starting reports its current stage, the stage itself comes too.
    if (message.Status != RequestQuestUpdate::StageUpdate || !Contains(pTeam->Participants, playerId))
        return;

    // Also how the stages we sent come back from the others (setting them raises the same event there)
    if (std::binary_search(pTeam->Stages.begin(), pTeam->Stages.end(), message.Stage))
        return;

    InsertStage(pTeam->Stages, message.Stage);

    NotifyQuestUpdate notify{};
    notify.Id = message.Id;
    notify.Stage = message.Stage;
    notify.Status = NotifyQuestUpdate::StageUpdate;

    for (const uint32_t participantId : pTeam->Participants)
    {
        if (participantId != playerId)
            SendTo(participantId, notify);
    }

    spdlog::info("Quest {:X}:{:X} stage {} reached by player {}, shared with {} teammate(s)", message.Id.ModId, message.Id.BaseId, message.Stage, playerId, pTeam->Participants.size() - 1);
}

void QuestService::OnQuestTeam(const PacketEvent<RequestQuestTeam>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;

    switch (message.Action)
    {
    case RequestQuestTeam::kInvite: OnInvite(acMessage.pPlayer, message); break;
    case RequestQuestTeam::kAccept: OnAccept(acMessage.pPlayer, message); break;
    case RequestQuestTeam::kDecline: OnDecline(acMessage.pPlayer, message); break;
    case RequestQuestTeam::kSetMainQuestSync: OnSetMainQuestSync(acMessage.pPlayer, message.PlayerId != 0); break;
    case RequestQuestTeam::kJoinMainQuest: OnJoinMainQuest(acMessage.pPlayer, message); break;
    case RequestQuestTeam::kLeave:
        for (const auto& [id, team] : m_teams)
        {
            if (team.QuestId == message.QuestId && (Contains(team.Participants, acMessage.pPlayer->GetId()) || Contains(team.Helpers, acMessage.pPlayer->GetId())))
            {
                spdlog::info("Player {} left the team for quest {:X}:{:X}", acMessage.pPlayer->GetId(), message.QuestId.ModId, message.QuestId.BaseId);
                RemoveFromTeam(id, acMessage.pPlayer->GetId());
                break;
            }
        }
        break;
    default: break;
    }
}

void QuestService::OnInvite(Player* apInviter, const RequestQuestTeam& acMessage) noexcept
{
    const uint32_t inviterId = apInviter->GetId();
    Player* pInvitee = m_world.GetPlayerManager().GetById(acMessage.PlayerId);
    if (!pInvitee || pInvitee == apInviter)
        return;

    const Team* pTeam = GetTeam(acMessage.QuestId, inviterId);
    if (pTeam && (Contains(pTeam->Participants, pInvitee->GetId()) || Contains(pTeam->Helpers, pInvitee->GetId())))
        return;

    // Without a team yet there's only the inviter's progress to share: they must be doing the quest (or have done it, to help)
    if (!pTeam && acMessage.QuestProgress == RequestQuestTeam::kNotStarted)
    {
        spdlog::warn("Player {} invited {} to quest {:X}:{:X} they haven't started", inviterId, pInvitee->GetId(), acMessage.QuestId.ModId, acMessage.QuestId.BaseId);
        return;
    }

    std::erase_if(m_invites, [&](const Invite& acInvite) { return acInvite.InviteeId == pInvitee->GetId() && acInvite.QuestId == acMessage.QuestId; });

    Invite& invite = m_invites.emplace_back();
    invite.InviterId = inviterId;
    invite.InviteeId = pInvitee->GetId();
    invite.QuestId = acMessage.QuestId;
    invite.ExpiryTick = GameServer::Get()->GetTick() + 120000;
    invite.InviterProgress = acMessage.QuestProgress;
    invite.InviterStages = Sorted(acMessage.Stages);

    NotifyQuestTeam notify{};
    notify.Type = NotifyQuestTeam::kInvite;
    notify.QuestId = acMessage.QuestId;
    notify.PlayerId = inviterId;
    pInvitee->Send(notify);

    spdlog::info("Player {} invited {} to do quest {:X}:{:X} together", inviterId, pInvitee->GetId(), acMessage.QuestId.ModId, acMessage.QuestId.BaseId);
}

void QuestService::OnAccept(Player* apInvitee, const RequestQuestTeam& acMessage) noexcept
{
    const uint32_t inviteeId = apInvitee->GetId();
    const auto inviteIt = std::find_if(m_invites.begin(), m_invites.end(), [&](const Invite& acInvite) {
        return acInvite.InviteeId == inviteeId && acInvite.InviterId == acMessage.PlayerId && acInvite.QuestId == acMessage.QuestId;
    });

    if (inviteIt == m_invites.end() || inviteIt->ExpiryTick < GameServer::Get()->GetTick())
    {
        spdlog::info("Player {} accepted an invite to quest {:X}:{:X} that isn't there anymore", inviteeId, acMessage.QuestId.ModId, acMessage.QuestId.BaseId);
        if (inviteIt != m_invites.end())
            m_invites.erase(inviteIt);
        return;
    }

    const Invite invite = *inviteIt;
    m_invites.erase(inviteIt);

    if (!m_world.GetPlayerManager().GetById(invite.InviterId))
        return;

    if (GetTeam(acMessage.QuestId, inviteeId))
    {
        spdlog::info("Player {} is already on a team for quest {:X}:{:X}", inviteeId, acMessage.QuestId.ModId, acMessage.QuestId.BaseId);
        return;
    }

    Team* pTeam = FindTeam(acMessage.QuestId, invite.InviterId);
    if (!pTeam)
    {
        // The inviter's progress becomes the team's
        Team& team = m_teams[m_nextTeamId++];
        team.QuestId = acMessage.QuestId;
        if (invite.InviterProgress == RequestQuestTeam::kRunning)
        {
            team.Participants.push_back(invite.InviterId);
            team.Stages = invite.InviterStages;
        }
        else
            team.Helpers.push_back(invite.InviterId);

        pTeam = &team;
    }

    // Who already finished it can only help: they'd take the others to the end
    const bool participant = Place(*pTeam, inviteeId, acMessage.QuestProgress, acMessage.Stages, false);

    // Someone has to be doing it
    if (!participant && pTeam->Participants.empty())
    {
        spdlog::info("Nobody would be doing quest {:X}:{:X}: no team", acMessage.QuestId.ModId, acMessage.QuestId.BaseId);
        for (auto it = m_teams.begin(); it != m_teams.end(); ++it)
        {
            if (&it->second == pTeam)
            {
                m_teams.erase(it);
                break;
            }
        }
        return;
    }

    spdlog::info("Player {} joined the team for quest {:X}:{:X} as a {} ({} participant(s), {} helper(s), {} stage(s) done)", inviteeId, acMessage.QuestId.ModId, acMessage.QuestId.BaseId,
                 participant ? "participant" : "helper", pTeam->Participants.size(), pTeam->Helpers.size(), pTeam->Stages.size());

    SendTeam(*pTeam);
}

bool QuestService::Place(Team& aTeam, uint32_t aPlayerId, uint8_t aProgress, const Vector<uint16_t>& acStages, bool aFinishedLeads) noexcept
{
    Erase(aTeam.Participants, aPlayerId);
    Erase(aTeam.Helpers, aPlayerId);

    // Participants share the team's progress: who's behind on the same path catches up, who's ahead takes the others
    // along. Who took another path can only help.
    const Vector<uint16_t> stages = Sorted(acStages);
    bool participant = false;
    if (aProgress != RequestQuestTeam::kCompleted || aFinishedLeads)
    {
        if (IsSubset(stages, aTeam.Stages))
            participant = true;
        else if (IsSubset(aTeam.Stages, stages))
        {
            participant = true;
            aTeam.Stages = stages;
        }
    }

    (participant ? aTeam.Participants : aTeam.Helpers).push_back(aPlayerId);
    return participant;
}

void QuestService::EndTeam(uint32_t aTeamId) noexcept
{
    const auto teamIt = m_teams.find(aTeamId);
    if (teamIt == m_teams.end())
        return;

    NotifyQuestTeam ended{};
    ended.Type = NotifyQuestTeam::kDisbanded;
    ended.QuestId = teamIt->second.QuestId;
    for (const uint32_t id : teamIt->second.Participants)
        SendTo(id, ended);
    for (const uint32_t id : teamIt->second.Helpers)
        SendTo(id, ended);

    spdlog::info("Team for quest {:X}:{:X} ended", ended.QuestId.ModId, ended.QuestId.BaseId);
    m_teams.erase(teamIt);
}

void QuestService::OnSetMainQuestSync(Player* apPlayer, bool aSync) noexcept
{
    auto& parties = m_world.GetPartyService();
    auto* pParty = parties.GetPlayerParty(apPlayer);
    if (!pParty || !parties.IsPlayerLeader(apPlayer))
    {
        spdlog::warn("Player {} switched main quest sync but doesn't lead a party", apPlayer->GetId());
        return;
    }

    const uint32_t partyId = *apPlayer->GetParty().JoinedPartyId;
    pParty->SyncMainQuests = aSync;
    spdlog::info("Party {}: main quests {}", partyId, aSync ? "shared" : "each player's own again");

    // Off: the party's main quest teams end, everyone keeps the progress they have
    if (!aSync)
    {
        Vector<uint32_t> teamIds;
        for (const auto& [id, team] : m_teams)
        {
            if (team.PartyId == partyId)
                teamIds.push_back(id);
        }
        for (const uint32_t id : teamIds)
            EndTeam(id);
    }

    // On: everyone reports their main quests (OnJoinMainQuest)
    NotifyQuestTeam notify{};
    notify.Type = NotifyQuestTeam::kMainQuestSync;
    notify.PlayerId = aSync ? 1 : 0;
    for (Player* pMember : pParty->Members)
        pMember->Send(notify);
}

// A party member's progress in a main quest while the party shares them: everyone catches up to whoever is furthest.
void QuestService::OnJoinMainQuest(Player* apPlayer, const RequestQuestTeam& acMessage) noexcept
{
    auto& parties = m_world.GetPartyService();
    const auto partyId = apPlayer->GetParty().JoinedPartyId;
    const auto* pParty = partyId ? parties.GetById(*partyId) : nullptr;
    if (!pParty || !pParty->SyncMainQuests)
        return;

    const uint32_t playerId = apPlayer->GetId();

    // The team they're on for it, or the party's, or a new one
    uint32_t teamId = 0;
    for (const auto& [id, team] : m_teams)
    {
        if (team.QuestId == acMessage.QuestId && (team.PartyId == *partyId || Contains(team.Participants, playerId) || Contains(team.Helpers, playerId)))
        {
            teamId = id;
            break;
        }
    }

    if (!teamId)
    {
        teamId = m_nextTeamId++;
        m_teams[teamId].QuestId = acMessage.QuestId;
    }

    Team& team = m_teams.find(teamId).value();
    team.PartyId = *partyId;

    // Everyone in the party is on it: who hasn't started it catches up, the others' reports place them
    for (Player* pMember : pParty->Members)
    {
        const uint32_t memberId = pMember->GetId();
        if (!Contains(team.Participants, memberId) && !Contains(team.Helpers, memberId) && !GetTeam(acMessage.QuestId, memberId))
            team.Participants.push_back(memberId);
    }

    const bool participant = Place(team, playerId, acMessage.QuestProgress, acMessage.Stages, true);

    spdlog::info("Party {} main quest {:X}:{:X}: player {} is a {} ({} stage(s) done, the team has {})", *partyId, acMessage.QuestId.ModId, acMessage.QuestId.BaseId, playerId,
                 participant ? "participant" : "helper", acMessage.Stages.size(), team.Stages.size());

    SendTeam(team);
}

void QuestService::OnJoinedParty(uint32_t aPartyId, Player* apPlayer) noexcept
{
    const auto* pParty = m_world.GetPartyService().GetById(aPartyId);
    if (!pParty || !pParty->SyncMainQuests)
        return;

    // On the party's main quest teams right away (they catch up if they haven't started them), then their own report
    // places them
    for (auto it = m_teams.begin(); it != m_teams.end(); ++it)
    {
        Team& team = it.value();
        if (team.PartyId != aPartyId || GetTeam(team.QuestId, apPlayer->GetId()))
            continue;

        team.Participants.push_back(apPlayer->GetId());
        SendTeam(team);
    }

    NotifyQuestTeam notify{};
    notify.Type = NotifyQuestTeam::kMainQuestSync;
    notify.PlayerId = 1;
    apPlayer->Send(notify);
}

void QuestService::OnLeavingParty(uint32_t aPartyId, Player* apPlayer) noexcept
{
    Vector<uint32_t> teamIds;
    for (const auto& [id, team] : m_teams)
    {
        if (team.PartyId == aPartyId && (Contains(team.Participants, apPlayer->GetId()) || Contains(team.Helpers, apPlayer->GetId())))
            teamIds.push_back(id);
    }

    for (const uint32_t id : teamIds)
        RemoveFromTeam(id, apPlayer->GetId());

    const auto* pParty = m_world.GetPartyService().GetById(aPartyId);
    if (pParty && pParty->SyncMainQuests)
    {
        NotifyQuestTeam notify{};
        notify.Type = NotifyQuestTeam::kMainQuestSync;
        notify.PlayerId = 0;
        apPlayer->Send(notify);
    }
}

void QuestService::OnDecline(Player* apInvitee, const RequestQuestTeam& acMessage) noexcept
{
    const uint32_t inviteeId = apInvitee->GetId();
    const auto removed = std::erase_if(m_invites, [&](const Invite& acInvite) {
        return acInvite.InviteeId == inviteeId && acInvite.InviterId == acMessage.PlayerId && acInvite.QuestId == acMessage.QuestId;
    });

    if (removed == 0)
        return;

    NotifyQuestTeam notify{};
    notify.Type = NotifyQuestTeam::kDeclined;
    notify.QuestId = acMessage.QuestId;
    notify.PlayerId = inviteeId;
    SendTo(acMessage.PlayerId, notify);
}

void QuestService::RemoveFromTeam(uint32_t aTeamId, uint32_t aPlayerId) noexcept
{
    auto teamIt = m_teams.find(aTeamId);
    if (teamIt == m_teams.end())
        return;

    Team& team = teamIt.value();
    Erase(team.Participants, aPlayerId);
    Erase(team.Helpers, aPlayerId);

    NotifyQuestTeam left{};
    left.Type = NotifyQuestTeam::kDisbanded;
    left.QuestId = team.QuestId;
    SendTo(aPlayerId, left);

    // Nobody left doing it, or nobody to do it with
    if (team.Participants.empty() || team.Participants.size() + team.Helpers.size() < 2)
    {
        for (const uint32_t id : team.Participants)
            SendTo(id, left);
        for (const uint32_t id : team.Helpers)
            SendTo(id, left);

        spdlog::info("Team for quest {:X}:{:X} ended", team.QuestId.ModId, team.QuestId.BaseId);
        m_teams.erase(teamIt);
        return;
    }

    SendTeam(team);
}

void QuestService::SendTeam(const Team& acTeam) const noexcept
{
    NotifyQuestTeam notify{};
    notify.Type = NotifyQuestTeam::kTeam;
    notify.QuestId = acTeam.QuestId;
    notify.Participants = acTeam.Participants;
    notify.Helpers = acTeam.Helpers;
    notify.Stages = acTeam.Stages;

    for (const uint32_t id : acTeam.Participants)
        SendTo(id, notify);
    for (const uint32_t id : acTeam.Helpers)
        SendTo(id, notify);
}

void QuestService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    const uint32_t playerId = acEvent.pPlayer->GetId();

    std::erase_if(m_invites, [playerId](const Invite& acInvite) { return acInvite.InviterId == playerId || acInvite.InviteeId == playerId; });

    Vector<uint32_t> teamIds;
    for (const auto& [id, team] : m_teams)
    {
        if (Contains(team.Participants, playerId) || Contains(team.Helpers, playerId))
            teamIds.push_back(id);
    }

    for (const uint32_t id : teamIds)
        RemoveFromTeam(id, playerId);
}

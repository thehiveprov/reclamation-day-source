#include <Services/DialogueVoteService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Dialogue/DialogueVotes.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyDialogueVote.h>

namespace
{
bool IsHost(const Player& acPlayer) noexcept
{
    // The host connects through 127.0.0.1 to the server its game started (as NemesisService)
    return GameServer::Get()->GetAdminSessions().count(acPlayer.GetConnectionId()) != 0;
}
} // namespace

DialogueVoteService::DialogueVoteService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestDialogueVote>>().connect<&DialogueVoteService::OnRequest>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DialogueVoteService::OnUpdate>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&DialogueVoteService::OnPlayerLeave>(this))
{
}

void DialogueVoteService::OnRequest(const PacketEvent<RequestDialogueVote>& acMessage) noexcept
{
    Player* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    const auto& request = acMessage.Packet;
    switch (request.Action)
    {
    case RequestDialogueVote::kOpen: Open(*pPlayer, request); break;
    case RequestDialogueVote::kCast: Cast(*pPlayer, request); break;
    case RequestDialogueVote::kClose:
    {
        const auto it = m_votes.find(request.VoteId);
        if (it != m_votes.end() && it->second.TalkerId == pPlayer->GetId())
            Finish(request.VoteId, false);
        break;
    }
    default: break;
    }
}

void DialogueVoteService::Open(Player& aTalker, const RequestDialogueVote& acRequest) noexcept
{
    // A talker has one vote at a time: options that came up again replace it
    for (auto it = m_votes.begin(); it != m_votes.end();)
    {
        const auto current = it++;
        if (current->second.TalkerId == aTalker.GetId())
            Finish(current->first, false);
    }

    Vote vote{};
    vote.Id = m_nextId++;
    vote.TalkerId = aTalker.GetId();
    vote.NpcServerId = acRequest.NpcServerId;
    vote.NpcName = acRequest.NpcName;
    vote.Options = acRequest.Options;
    vote.Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(DialogueVotes::kVoteMilliseconds);
    for (Player* pVoter : VotersFor(aTalker))
        vote.Voters.push_back({pVoter->GetId(), pVoter->GetUsername(), -1});

    NotifyDialogueVote notify{};
    notify.Stage = NotifyDialogueVote::kOpen;
    notify.Options = vote.Options;
    notify.MillisecondsLeft = static_cast<uint32_t>(DialogueVotes::kVoteMilliseconds);
    Send(vote, notify);

    spdlog::info("Dialogue vote {}: {} talking to {} ({} options), {} voter(s)", vote.Id, aTalker.GetUsername().c_str(), vote.NpcName.c_str(), vote.Options.size(),
                 vote.Voters.size());
    // Alone: kOpen told the talker, their own pick goes
    if (vote.Voters.size() > 1)
        m_votes.emplace(vote.Id, std::move(vote));
}

void DialogueVoteService::Cast(Player& aVoter, const RequestDialogueVote& acRequest) noexcept
{
    const auto it = m_votes.find(acRequest.VoteId);
    if (it == m_votes.end())
        return;

    Vote& vote = it->second;
    const auto voter = std::find_if(vote.Voters.begin(), vote.Voters.end(), [&](const Voter& acVoter) { return acVoter.PlayerId == aVoter.GetId(); });
    if (voter == vote.Voters.end() || acRequest.Choice >= vote.Options.size() || !vote.Options[acRequest.Choice].Enabled)
        return;

    voter->Choice = acRequest.Choice;
    spdlog::info("Dialogue vote {}: {} votes for option {}", vote.Id, aVoter.GetUsername().c_str(), acRequest.Choice);

    std::vector<int> choices;
    for (const Voter& entry : vote.Voters)
        choices.push_back(entry.Choice);
    if (DialogueVotes::AllVoted(choices))
    {
        Finish(vote.Id, true);
        return;
    }

    NotifyDialogueVote notify{};
    notify.Stage = NotifyDialogueVote::kTally;
    notify.MillisecondsLeft = static_cast<uint32_t>(std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(vote.Deadline - std::chrono::steady_clock::now()).count()));
    Send(vote, notify);
}

void DialogueVoteService::Finish(uint32_t aVoteId, bool aDecide) noexcept
{
    const auto it = m_votes.find(aVoteId);
    if (it == m_votes.end())
        return;

    const Vote vote = std::move(it->second);
    m_votes.erase(it);

    NotifyDialogueVote notify{};
    if (aDecide)
    {
        std::vector<int> choices;
        int host = -1;
        for (size_t i = 0; i < vote.Voters.size(); ++i)
        {
            choices.push_back(vote.Voters[i].Choice);
            const Player* pPlayer = m_world.GetPlayerManager().GetById(vote.Voters[i].PlayerId);
            if (pPlayer && IsHost(*pPlayer))
                host = static_cast<int>(i);
        }
        const auto outcome = DialogueVotes::Decide(choices, vote.Options.size(), host, 0);
        notify.Stage = NotifyDialogueVote::kResult;
        notify.Result = static_cast<int8_t>(outcome.Choice);
        notify.HostBrokeTie = outcome.HostBrokeTie;
        spdlog::info("Dialogue vote {}: option {} chosen{}", vote.Id, outcome.Choice, outcome.HostBrokeTie ? " (the host broke a tie)" : "");
    }
    else
    {
        notify.Stage = NotifyDialogueVote::kClosed;
        spdlog::info("Dialogue vote {}: closed", vote.Id);
    }
    Send(vote, notify);
}

void DialogueVoteService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> due;
    for (const auto& [id, vote] : m_votes)
    {
        if (now >= vote.Deadline)
            due.push_back(id);
    }
    for (const uint32_t id : due)
        Finish(id, true);
}

void DialogueVoteService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (!acEvent.pPlayer)
        return;

    const uint32_t playerId = acEvent.pPlayer->GetId();
    std::vector<uint32_t> closed;
    std::vector<uint32_t> decided;
    for (auto& [id, vote] : m_votes)
    {
        if (vote.TalkerId == playerId)
        {
            closed.push_back(id);
            continue;
        }
        const auto before = vote.Voters.size();
        std::erase_if(vote.Voters, [playerId](const Voter& acVoter) { return acVoter.PlayerId == playerId; });
        if (vote.Voters.size() == before)
            continue;
        std::vector<int> choices;
        for (const Voter& voter : vote.Voters)
            choices.push_back(voter.Choice);
        // The ones left had all voted
        if (DialogueVotes::AllVoted(choices))
            decided.push_back(id);
    }
    for (const uint32_t id : closed)
        Finish(id, false);
    for (const uint32_t id : decided)
        Finish(id, true);
}

void DialogueVoteService::Send(const Vote& acVote, NotifyDialogueVote& aNotify) const noexcept
{
    aNotify.VoteId = acVote.Id;
    aNotify.TalkerId = acVote.TalkerId;
    aNotify.NpcServerId = acVote.NpcServerId;
    aNotify.NpcName = acVote.NpcName;
    aNotify.Voters.clear();
    for (const Voter& voter : acVote.Voters)
    {
        NotifyDialogueVote::Voter entry{};
        entry.PlayerId = voter.PlayerId;
        entry.Name = voter.Name;
        entry.Choice = static_cast<int8_t>(voter.Choice);
        aNotify.Voters.push_back(entry);
    }

    for (const Voter& voter : acVote.Voters)
    {
        if (const Player* pPlayer = m_world.GetPlayerManager().GetById(voter.PlayerId))
            pPlayer->Send(aNotify);
    }
}

std::vector<Player*> DialogueVoteService::VotersFor(Player& aTalker) noexcept
{
    std::vector<Player*> voters{&aTalker};

    const auto talkerCharacter = aTalker.GetCharacter();
    const auto* pTalkerMovement = talkerCharacter ? m_world.try_get<MovementComponent>(*talkerCharacter) : nullptr;
    const PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(&aTalker);
    if (!pTalkerMovement || !pParty)
        return voters;

    const CellIdComponent& talkerCell = aTalker.GetCellComponent();
    for (Player* pMember : pParty->Members)
    {
        if (!pMember || pMember == &aTalker)
            continue;

        // Inside: the same cell; outside: the same worldspace (cells next to each other are close too)
        const CellIdComponent& cell = pMember->GetCellComponent();
        const bool samePlace = talkerCell.WorldSpaceId ? cell.WorldSpaceId == talkerCell.WorldSpaceId : cell.Cell == talkerCell.Cell;
        const auto character = pMember->GetCharacter();
        const auto* pMovement = character ? m_world.try_get<MovementComponent>(*character) : nullptr;
        if (!samePlace || !pMovement)
            continue;

        if (glm::distance(pMovement->Position, pTalkerMovement->Position) <= DialogueVotes::kVoterRange)
            voters.push_back(pMember);
    }
    return voters;
}

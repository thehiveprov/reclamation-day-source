#pragma once

#include <Events/PacketEvent.h>

#include <Messages/RequestDialogueVote.h>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerLeaveEvent;
struct NotifyDialogueVote;

/**
 * @brief Co-op dialogue votes (Fallout 4, see docs/DIALOGUE_VOTE.md).
 *
 * The player talking to an NPC puts their dialogue options to a vote when they come up (RequestDialogueVote kOpen).
 * The voters are the talker and the members of their party within DialogueVotes::kVoterRange of them (in the same cell
 * inside, the same worldspace outside). Each votes (kCast, again to change it); after 30 seconds, or once everyone has
 * voted, the most votes win (a tie: the host's vote, then the talker's) and the talker's game picks that option
 * (NotifyDialogueVote kResult). A talker with no one near votes alone: kOpen tells them so and their own pick goes.
 */
class DialogueVoteService
{
public:
    DialogueVoteService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~DialogueVoteService() noexcept = default;

    TP_NOCOPYMOVE(DialogueVoteService);

private:
    struct Voter
    {
        uint32_t PlayerId{};
        String Name;
        int Choice = -1;
    };

    struct Vote
    {
        uint32_t Id{};
        uint32_t TalkerId{};
        uint32_t NpcServerId{};
        String NpcName;
        Vector<RequestDialogueVote::Option> Options;
        std::vector<Voter> Voters; // the talker first
        std::chrono::steady_clock::time_point Deadline;
    };

    void OnRequest(const PacketEvent<RequestDialogueVote>& acMessage) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    void Open(Player& aTalker, const RequestDialogueVote& acRequest) noexcept;
    void Cast(Player& aVoter, const RequestDialogueVote& acRequest) noexcept;
    // Ends the vote: kResult with the choice (Decide), or kClosed
    void Finish(uint32_t aVoteId, bool aDecide) noexcept;
    void Send(const Vote& acVote, NotifyDialogueVote& aNotify) const noexcept;
    [[nodiscard]] std::vector<Player*> VotersFor(Player& aTalker) noexcept;

    World& m_world;
    std::map<uint32_t, Vote> m_votes;
    uint32_t m_nextId{1};

    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_playerLeaveConnection;
};

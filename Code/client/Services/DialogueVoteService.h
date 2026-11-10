#pragma once

#include <Games/Fallout4/DialogueGame.h>

#include <string>
#include <vector>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct DialogueLockEvent;
struct NotifyDialogueVote;

/**
 * @brief Co-op dialogue (Fallout 4, docs/DIALOGUE_VOTE.md): the party votes on the dialogue options of the player who
 * talks to an NPC.
 *
 * The talker (the one holding the NPC's dialogue lock) sends their options to the server when they come up, as their
 * dialogue menu shows them (XDI's list, bundled with Reclamation Day; the game's four without it). The server makes the
 * talker and the party members within 30 m of them the voters (DialogueVoteService on the server). With other voters,
 * the talker's own pick in the dialogue menu is their vote and is held back (DialogueGame's pick handler); the others
 * vote on the overlay's card. After 30 seconds, or once everyone has voted, the most votes win (a tie: the host's vote,
 * then the talker's) and the talker's game picks it. Alone, nothing changes: their pick goes at once.
 */
struct DialogueVoteService
{
    DialogueVoteService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~DialogueVoteService() noexcept;

    TP_NOCOPYMOVE(DialogueVoteService);

    // The overlay's vote card: dialogueVote(request, payload): "cast" (payload: the option's index), "close" (the card
    // goes without a vote)
    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;

private:
    enum class Talk
    {
        kNone,   // no options up
        kAsked,  // the options went to the server, its answer not back yet (a pick waits for it)
        kVoting, // the party votes: the talker's picks are their votes
        kSolo,   // no one to vote with: picks go through
        kDone,   // this round's vote is over (picked, or nobody voted): picks go through
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDialogueLock(const DialogueLockEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnNotify(const NotifyDialogueVote& acMessage) noexcept;

    // From DialogueGame's hook: false holds the pick back (a vote)
    static bool OnPick(int32_t aOption) noexcept;
    bool HandlePick(int32_t aOption) noexcept;
    void NewOptions(uintptr_t aAction) noexcept;
    void EndTalk() noexcept;
    void SendCast(uint32_t aVoteId, uint8_t aChoice) noexcept;
    void SendClose() noexcept;
    [[nodiscard]] int IndexOf(int32_t aOption) const noexcept;

    void ShowCard(const NotifyDialogueVote& acMessage) noexcept;
    void CloseCard() noexcept;

    World& m_world;
    TransportService& m_transport;

    // the NPC the local player talks to (their dialogue lock)
    uint32_t m_npcFormId{};
    uintptr_t m_action{};
    bool m_wasAwaiting{};
    std::vector<DialogueGame::Option> m_options;
    Talk m_talk{Talk::kNone};
    uint32_t m_voteId{};
    int32_t m_pendingPick{-1};
    float m_askedFor{};

    // the vote on this player's card (0: none), its options (kOpen has them), and whether the card has the overlay's
    // input (a voter yet to vote)
    uint32_t m_cardVoteId{};
    bool m_cardTalker{};
    std::vector<DialogueGame::Option> m_cardOptions;
    bool m_cardInput{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_lockConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_notifyConnection;
};

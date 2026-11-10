#pragma once

#include <World.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>

struct NotifyQuestUpdate;
struct NotifyQuestTeam;
struct DisconnectedEvent;
struct UpdateEvent;

struct TESQuest;

/**
 * @brief Quest sync, Fallout 76 style: each player's quests are their own (saved in their own game) until they team up
 * on one. Any player invites another to do a quest together (overlay, Player manager > Quests); while teamed up on it,
 * the stages any participant reaches are set for the other participants, and someone who joins catches up to the
 * team's progress. Who already finished it (or got further on another path) helps without their progress changing.
 * Every stage the local player reaches goes to the server, which passes it on only to their teammates on that quest
 * (see the server's QuestService).
 */
class QuestService final : public BSTEventSink<TESQuestStartStopEvent>, BSTEventSink<TESQuestStageEvent>
{
public:
    QuestService(World&, entt::dispatcher&);
    ~QuestService() = default;

    static bool IsNonSyncableQuest(TESQuest* apQuest);
    static void DebugDumpQuests();
    static bool StopQuest(uint32_t aformId);

#if TP_FALLOUT4
    // From the overlay, quests(request, payload): "get"; "invite" "<quest>:<playerId>"; "inviteAll" "<quest>" (the party,
    // or everyone without one); "accept" and "decline" "<quest>:<inviterId>"; "leave" "<quest>"; "mainQuestSync" "1"/"0"
    // (party leader). quest: form id in hex. Answered with "questsState".
    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;

    // Faction quests together (FactionService): invites a player to do the quest together, or accepts their invite
    void TeamUp(uint32_t aQuestId, uint32_t aPlayerId, bool aInvite) const noexcept;
    // The first of these quests the local player is doing with others, 0 if none (quest items, see ObjectService)
    [[nodiscard]] uint32_t FindTeamedQuest(const Vector<uint32_t>& acQuestIds) const noexcept;
#endif

private:
    friend struct QuestEventHandler;

    void OnConnected(const ConnectedEvent&) noexcept;

    BSTEventResult OnEvent(const TESQuestStartStopEvent*, const EventDispatcher<TESQuestStartStopEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageEvent*, const EventDispatcher<TESQuestStageEvent>*) override;

    void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;

#if TP_FALLOUT4
    struct Team
    {
        Vector<uint32_t> Participants;
        Vector<uint32_t> Helpers;
    };

    struct Invite
    {
        uint32_t InviterId;
        uint32_t QuestId; // form id
    };

    void OnQuestTeam(const NotifyQuestTeam&) noexcept;
    void OnMainQuestSync(bool aSync) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    // New quests in the Pip-Boy: the overlay offers to do each one together ("questStarted")
    void OnUpdate(const UpdateEvent&) noexcept;

    void SendTeamRequest(uint8_t aAction, uint32_t aQuestId, uint32_t aPlayerId) const noexcept;
    // Invites the party to the quest, or everyone without one
    void InviteAll(uint32_t aQuestId) const noexcept;
    // Main quest sync is on: our progress in each main quest, for the party's teams
    void ReportMainQuests() noexcept;
    // Sets the stages of the team's progress the local player doesn't have yet, in order
    void CatchUp(TESQuest* apQuest, const Vector<uint16_t>& acStages) const noexcept;
    [[nodiscard]] bool IsParticipant(uint32_t aQuestId) const noexcept;
    [[nodiscard]] std::string GetPlayerName(uint32_t aPlayerId) const noexcept;
    void SendUiState() const noexcept;

    TiltedPhoques::Map<uint32_t, Team> m_teams; // by quest form id
    Vector<Invite> m_invites;

    bool m_mainQuestSync = false;                       // our party shares every main quest
    TiltedPhoques::Set<uint32_t> m_mainQuestsJoined{};  // reported to the party's teams since it was switched on

    TiltedPhoques::Set<uint32_t> m_seenQuests{}; // in the Pip-Boy already, no offer to do them together
    bool m_seenQuestsReady = false;              // filled with what was there when we connected
    std::chrono::steady_clock::time_point m_lastQuestScan{};

    entt::scoped_connection m_questTeamConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_updateConnection;
#endif

    World& m_world;

    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_leftConnection;
    entt::scoped_connection m_questUpdateConnection;
};

#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

struct World;
struct Player;
struct PlayerLeaveEvent;
struct RequestQuestUpdate;
struct RequestQuestTeam;

/**
 * @brief Quest team-ups, like Fallout 76: each player's quests are their own (saved in their own game) until one
 * invites another to do a quest together. While teamed up on it, the stages any participant reaches are set for the
 * other participants too, and someone who joins catches up to the team's progress. Someone who already finished the
 * quest, or got further on another path, joins as a helper: their own progress stays as it is. Quest items for a
 * team's quest only exist once among its participants (see ObjectService). Teams last until the server stops.
 *
 * Main quest sync, the party leader's switch: every main quest is shared within the party, without invites. Each
 * member reports their progress in the main quests; everyone catches up to whoever is furthest (finished included),
 * except who went another way, who helps.
 */
class QuestService
{
public:
    struct Team
    {
        GameId QuestId{};
        Vector<uint32_t> Participants{};
        Vector<uint32_t> Helpers{};
        Vector<uint16_t> Stages{};             // done, ascending
        std::optional<uint32_t> PartyId{};     // a party's main quest team (main quest sync)
    };

    QuestService(World& aWorld, entt::dispatcher& aDispatcher);

    // The team the player is on for this quest, null if none
    [[nodiscard]] const Team* GetTeam(const GameId& acQuestId, uint32_t aPlayerId) const noexcept;

    // From PartyService: a player joined the party, or is leaving it
    void OnJoinedParty(uint32_t aPartyId, Player* apPlayer) noexcept;
    void OnLeavingParty(uint32_t aPartyId, Player* apPlayer) noexcept;

private:
    struct Invite
    {
        uint32_t InviterId{};
        uint32_t InviteeId{};
        GameId QuestId{};
        uint64_t ExpiryTick{};
        uint8_t InviterProgress{};
        Vector<uint16_t> InviterStages{}; // kept up to date until it's accepted: the team starts with them
    };

    void OnQuestChanges(const PacketEvent<RequestQuestUpdate>& aChanges) noexcept;
    void OnQuestTeam(const PacketEvent<RequestQuestTeam>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    void OnInvite(Player* apInviter, const RequestQuestTeam& acMessage) noexcept;
    void OnAccept(Player* apInvitee, const RequestQuestTeam& acMessage) noexcept;
    void OnDecline(Player* apInvitee, const RequestQuestTeam& acMessage) noexcept;
    void OnSetMainQuestSync(Player* apPlayer, bool aSync) noexcept;
    void OnJoinMainQuest(Player* apPlayer, const RequestQuestTeam& acMessage) noexcept;

    // Puts the player on the team by their progress: participant if they're behind on the team's path (they catch up)
    // or ahead on it (the team takes theirs), helper otherwise. Who finished the quest leads only if aFinishedLeads.
    // Returns whether they're a participant.
    bool Place(Team& aTeam, uint32_t aPlayerId, uint8_t aProgress, const Vector<uint16_t>& acStages, bool aFinishedLeads) noexcept;
    // Ends a team: its members are told, their progress stays as it is
    void EndTeam(uint32_t aTeamId) noexcept;

    Team* FindTeam(const GameId& acQuestId, uint32_t aPlayerId) noexcept;
    // Takes the player off the team, ends the team when nobody is left to share progress with
    void RemoveFromTeam(uint32_t aTeamId, uint32_t aPlayerId) noexcept;
    void SendTeam(const Team& acTeam) const noexcept;

    World& m_world;
    TiltedPhoques::Map<uint32_t, Team> m_teams;
    uint32_t m_nextTeamId = 1;
    Vector<Invite> m_invites;

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_questTeamConnection;
    entt::scoped_connection m_playerLeaveConnection;
};

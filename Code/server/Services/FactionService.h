#pragma once

#include <Events/PacketEvent.h>

#include <Messages/RequestFactionStanding.h>
#include <Messages/RequestSharedQuest.h>

struct World;
struct Player;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;

/**
 * @brief Factions (Fallout 4, see docs/FACTIONS.md): each player's standing with the Commonwealth's factions, as their
 * own game has it, relayed to everyone (their character in the other games carries it) and given to players who join.
 * Kept only while they're connected: it's their save's.
 *
 * Faction quests together: a player who got one of a faction's radiant quests (RequestSharedQuest, with what its
 * aliases were filled with) has it passed on to the members of their party who are in the same faction, to start it
 * with the same places and targets (NotifySharedQuest).
 */
class FactionService
{
public:
    FactionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~FactionService() noexcept = default;

    TP_NOCOPYMOVE(FactionService);

    // A connected player's standing, null if their game hasn't said yet
    [[nodiscard]] const FactionStanding* GetStanding(uint32_t aPlayerId) const noexcept;

private:
    void OnStanding(const PacketEvent<RequestFactionStanding>& acMessage) noexcept;
    void OnSharedQuest(const PacketEvent<RequestSharedQuest>& acMessage) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    World& m_world;
    TiltedPhoques::Map<uint32_t, FactionStanding> m_standings; // by player id

    entt::scoped_connection m_standingConnection;
    entt::scoped_connection m_sharedQuestConnection;
    entt::scoped_connection m_joinConnection;
    entt::scoped_connection m_leaveConnection;
};

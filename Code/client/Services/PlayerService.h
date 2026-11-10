#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>

struct World;
struct TransportService;

struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct ServerSettings;
struct GridCellChangeEvent;
struct CellChangeEvent;
struct PlayerDialogueEvent;
struct PlayerLevelEvent;
struct PartyJoinedEvent;
struct PartyLeftEvent;

struct NotifyPlayerRespawn;

/**
 * @brief Handles logic related to the local player.
 */
struct PlayerService : BSTEventSink<TESLoadGameEvent>
{
    PlayerService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~PlayerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnServerSettingsReceived(const ServerSettings& acSettings) noexcept;
    void OnNotifyPlayerRespawn(const NotifyPlayerRespawn& acMessage) const noexcept;
    void OnGridCellChangeEvent(const GridCellChangeEvent& acEvent) const noexcept;
    void OnCellChangeEvent(const CellChangeEvent& acEvent) const noexcept;
    void OnPlayerDialogueEvent(const PlayerDialogueEvent& acEvent) const noexcept;
    void OnPlayerLevelEvent(const PlayerLevelEvent& acEvent) const noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;
    // Fallout 4: a save loaded with the death system off doesn't bring its respawn mode back
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;

private:
    /**
     * @brief Run the respawn timer, and if it hits 0, respawn the player.
     */
    void RunRespawnUpdates(const double acDeltaTime) noexcept;
    void RunPostDeathUpdates(const double acDeltaTime) noexcept;
    /**
     * @brief Make sure difficulty doesn't get changed while connected
     */
    void RunDifficultyUpdates() noexcept;
    void RunLevelUpdates() const noexcept;
    void RunBeastFormDetection() const noexcept;

    void ToggleDeathSystem(bool aSet) noexcept;
    // Nothing of a respawn left behind once the death system is off: countdown, fade, god mode; a player still down
    // gets up where it is
    void CancelRespawn() noexcept;

#if TP_FALLOUT4
    /**
     * @brief Remember where the (alive) player has been standing, a spot every few metres walked, in the current worldspace
     * or interior cell, so a respawn can put them 100-120 m from where they went down on ground they stood on.
     */
    void RecordRespawnPosition(const double acDeltaTime) noexcept;
    struct RespawnSpot
    {
        NiPoint3 position{};
        std::chrono::steady_clock::time_point at{};
    };
    // nullptr: nowhere they stood at all. apInBand: whether it's 100-120 m away (else the farthest they've been, under 100 m)
    const RespawnSpot* PickRespawnPosition(const NiPoint3& acDeathPosition, bool* apInBand = nullptr) const noexcept;
    /**
     * @brief Decide whether this game may start random encounters, so players near each other don't each spawn a copy.
     */
    void RunRandomEncounterUpdates(const double acDeltaTime) noexcept;
    [[nodiscard]] bool OutranksForEncounters(uint32_t aPlayerId) const noexcept;
#endif

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    // Real time, not frame time: frames stutter or stop around the respawn's loading screen (upstream 2e52a704)
    std::chrono::steady_clock::time_point m_respawnDeadline{};
    int32_t m_serverDifficulty = 6;
    int32_t m_previousDifficulty = 6;
    // what RunDifficultyUpdates last set, and when
    int32_t m_appliedDifficulty = 6;
    std::chrono::steady_clock::time_point m_lastDifficultyUpdate{};

    // Only while on a server that uses it (its settings turn it on); off in single player and before joining
    bool m_isDeathSystemEnabled = false;
    bool m_respawnCountdown = false;

    bool m_knockdownStart = false;
    std::chrono::steady_clock::time_point m_knockdownDeadline{};

    bool m_godmodeStart = false;
    std::chrono::steady_clock::time_point m_godmodeDeadline{};

    uint32_t m_cachedMainSpellId = 0;
    uint32_t m_cachedSecondarySpellId = 0;
    uint32_t m_cachedPowerId = 0;

#if TP_FALLOUT4
    // Looked at twice a second; a spot kept when they're on the ground this far from the last one kept (3 m), so standing
    // or fighting in one place doesn't push the way they came out of it: 300 spots is about 900 m of where they've been
    // (a respawn is 100 to 120 m back along it)
    static constexpr double kRespawnHistoryEvery = 0.5;
    static constexpr float kRespawnSpotApart = 210.f;
    static constexpr size_t kRespawnHistorySize = 300;
    std::array<RespawnSpot, kRespawnHistorySize> m_respawnHistory{};
    size_t m_respawnHistoryCount = 0; // valid samples
    size_t m_respawnHistoryNext = 0;  // ring buffer write index
    uint32_t m_respawnHistoryPlace = 0; // the worldspace, or the interior cell, the samples are in
    double m_respawnHistoryTimer = 0.0;
    double m_randomEncounterTimer = 0.0;
#endif

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_settingsConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_gridCellChangeConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
};

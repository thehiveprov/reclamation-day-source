#pragma once

#include <Events/PacketEvent.h>

#include <Nemesis/NemesisEngine.h>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct CharacterRemoveEvent;
struct RequestNemesis;
struct NotifyNemesis;

/**
 * @brief Nemeses (Fallout 4, see docs/NEMESIS.md): the nemesis world of this server, one for everyone in it.
 *
 * Holds Nemesis::Engine and feeds it: the players' games report deeds, deaths, the end of encounters, faces and where
 * their player is (RequestNemesis); the engine decides. A few times a second it runs the world up to the game's clock
 * (the server's calendar, in game minutes), rolls an encounter for each player who can be met, and sends what the
 * engine queued: the encounter to its player's game, names of nemeses to every game, orders to the game running a man,
 * karma changes and notices to their player (NotifyNemesis). The pages of the Pip-Boy's NEMESIS tab and the questioning
 * card are asked for through here too.
 *
 * Kept in nemeses.txt in the world's folder (RD_WORLD_DATA, as settlements.txt), written through a temporary file at
 * most every 10 s while something changed, and when the server ends. The world starts the first time a game says
 * which campaign it is (the host's, or the first player's): its gangs are that campaign's.
 */
class NemesisService
{
public:
    NemesisService(World& aWorld, entt::dispatcher& aDispatcher);
    ~NemesisService() noexcept;

    TP_NOCOPYMOVE(NemesisService);

private:
    void OnRequest(const PacketEvent<RequestNemesis>&) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent&) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent&) noexcept;
    void OnCharacterRemove(const CharacterRemoveEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;

    void OnHello(Player& aPlayer, const RequestNemesis& acMessage) noexcept;
    void OnUi(Player& aPlayer, const std::string& acRequest, const std::string& acPayload) noexcept;
    void ApplySettings(const std::string& acSettings) noexcept;

    // The game's clock as the engine counts it (Nemesis::Text::ToMinutes), 0 until a player's game has set it
    [[nodiscard]] int64_t GameMinutes() const noexcept;
    // The names of the players in this one's party (them included)
    [[nodiscard]] std::vector<std::string> PartyOf(Player& aPlayer) const noexcept;
    [[nodiscard]] static std::string NameOf(const Player& acPlayer) noexcept;
    [[nodiscard]] static bool IsHost(const Player& acPlayer) noexcept;

    // Sends what the engine queued
    void Flush() noexcept;
    void SendSpawn(const Nemesis::SpawnPlan& acPlan) noexcept;
    void SendIdentity(uint32_t aActorServerId, const Nemesis::Record& acRecord, Player* apTo = nullptr) const noexcept;
    void SendUi(Player& aPlayer, const char* acpEvent, const std::string& acJson) const noexcept;
    void SendToName(const std::string& acPlayer, const NotifyNemesis& acMessage) const noexcept;

    void Load() noexcept;
    void Save() noexcept;

    // Where a player is, as their game last said (RequestNemesis::kStatus)
    struct Spot
    {
        Nemesis::PlayerSpot Where;
        std::chrono::steady_clock::time_point At;
    };

    World& m_world;
    Nemesis::Engine m_engine;
    TiltedPhoques::Map<uint32_t, Spot> m_spots;
    std::string m_campaign;
    std::vector<std::string> m_plugins;
    std::filesystem::path m_file;
    std::chrono::steady_clock::time_point m_tickAt;
    std::chrono::steady_clock::time_point m_savedAt;

    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_characterRemoveConnection;
    entt::scoped_connection m_updateConnection;
};

#pragma once

#include <Events/PacketEvent.h>

struct World;
struct Player;
struct RequestWeatherChange;
struct RequestCurrentWeather;
struct PlayerLeaveEvent;

/**
 * @brief Weather like Fallout 76: one weather per worldspace for the whole server. Of the players outside in a
 * worldspace, the one who joined first (the host, in a hosted world) leads it: their game's weather runs as in single
 * player and the others there see the same.
 */
struct WeatherService
{
    WeatherService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~WeatherService() noexcept = default;

    TP_NOCOPYMOVE(WeatherService);

protected:
    void OnWeatherChange(const PacketEvent<RequestWeatherChange>& acMessage) noexcept;
    void OnRequestCurrentWeather(const PacketEvent<RequestCurrentWeather>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

private:
    // Where a player is (empty: inside) and the weather there in their game
    struct Report
    {
        GameId WorldSpaceId{};
        GameId WeatherId{};
    };

    struct WorldWeather
    {
        uint32_t LeaderId = 0; // player id, 0: nobody outside there
        GameId WeatherId{};
    };

    // Picks the worldspace's leader again and tells everyone there what changed
    void Update(const GameId& acWorldSpaceId) noexcept;
    void Send(Player* apPlayer, const GameId& acWorldSpaceId, const WorldWeather& acWeather) const noexcept;

    World& m_world;

    TiltedPhoques::Map<uint32_t, Report> m_reports; // by player id
    TiltedPhoques::Map<GameId, WorldWeather> m_worlds;

    entt::scoped_connection m_weatherChangeConnection;
    entt::scoped_connection m_currentWeatherConnection;
    entt::scoped_connection m_playerLeaveConnection;
};

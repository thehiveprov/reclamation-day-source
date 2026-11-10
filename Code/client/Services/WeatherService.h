#pragma once

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyWeatherChange;

/**
 * @brief Weather like Fallout 76: one weather per worldspace for everyone on the server. The server picks who leads
 * each worldspace (who joined first, of the players outside there): the leader's game runs its weather as in single
 * player and reports it, the others show it. Inside, nothing is shared (no weather there).
 */
struct WeatherService
{
    WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~WeatherService() noexcept = default;

    TP_NOCOPYMOVE(WeatherService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept;

private:
    // Tells the server where the local player is and the weather here
    void Report(uint32_t aWeatherId) noexcept;
    // Shows the weather the leader has (aInstant: without a transition, on arrival)
    void Show(bool aInstant) noexcept;
    // Back to the game's own weather
    void Release() noexcept;

    World& m_world;
    TransportService& m_transport;

    uint32_t m_worldSpaceId = 0; // where the local player is as last reported, 0: inside
    uint32_t m_reportedWeatherId = 0;
    bool m_lead = false;
    uint32_t m_sharedWeatherId = 0; // the leader's weather, shown here (0: none yet)
    bool m_overriding = false;
    std::chrono::steady_clock::time_point m_lastCheck{};
    std::chrono::steady_clock::time_point m_lastShow{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_weatherChangeConnection;
};

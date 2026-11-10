#include <Services/WeatherService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PlayerLeaveEvent.h>

#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Messages/RequestCurrentWeather.h>

WeatherService::WeatherService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_weatherChangeConnection = aDispatcher.sink<PacketEvent<RequestWeatherChange>>().connect<&WeatherService::OnWeatherChange>(this);
    m_currentWeatherConnection = aDispatcher.sink<PacketEvent<RequestCurrentWeather>>().connect<&WeatherService::OnRequestCurrentWeather>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&WeatherService::OnPlayerLeave>(this);
}

void WeatherService::OnWeatherChange(const PacketEvent<RequestWeatherChange>& acMessage) noexcept
{
    const uint32_t playerId = acMessage.pPlayer->GetId();
    const auto& request = acMessage.Packet;

    auto& report = m_reports[playerId];
    const GameId previousWorldSpace = report.WorldSpaceId;
    report.WorldSpaceId = request.WorldSpaceId;
    report.WeatherId = request.Id;

    if (previousWorldSpace && previousWorldSpace != request.WorldSpaceId)
        Update(previousWorldSpace);

    if (!request.WorldSpaceId)
        return;

    const auto worldIt = m_worlds.find(request.WorldSpaceId);
    const bool known = worldIt != m_worlds.end() && worldIt->second.LeaderId != 0;
    Update(request.WorldSpaceId);

    // Arriving where the weather is already decided: Update only told the others
    if (known && previousWorldSpace != request.WorldSpaceId)
    {
        const auto& weather = m_worlds[request.WorldSpaceId];
        if (weather.LeaderId != playerId)
            Send(acMessage.pPlayer, request.WorldSpaceId, weather);
    }
}

void WeatherService::OnRequestCurrentWeather(const PacketEvent<RequestCurrentWeather>& acMessage) noexcept
{
    const auto reportIt = m_reports.find(acMessage.pPlayer->GetId());
    if (reportIt == m_reports.end() || !reportIt->second.WorldSpaceId)
        return;

    const auto worldIt = m_worlds.find(reportIt->second.WorldSpaceId);
    if (worldIt != m_worlds.end())
        Send(acMessage.pPlayer, worldIt->first, worldIt->second);
}

void WeatherService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    const auto reportIt = m_reports.find(acEvent.pPlayer->GetId());
    if (reportIt == m_reports.end())
        return;

    const GameId worldSpaceId = reportIt->second.WorldSpaceId;
    m_reports.erase(reportIt);

    if (worldSpaceId)
        Update(worldSpaceId);
}

void WeatherService::Update(const GameId& acWorldSpaceId) noexcept
{
    auto& playerManager = m_world.GetPlayerManager();

    // Who joined first leads: player ids count up from the first to join
    uint32_t leaderId = 0;
    GameId leaderWeather{};
    for (const auto& [playerId, report] : m_reports)
    {
        if (report.WorldSpaceId != acWorldSpaceId || !playerManager.GetById(playerId))
            continue;

        if (leaderId == 0 || playerId < leaderId)
        {
            leaderId = playerId;
            leaderWeather = report.WeatherId;
        }
    }

    auto& weather = m_worlds[acWorldSpaceId];
    const bool newLeader = weather.LeaderId != leaderId;
    // A leader who has no weather yet (just arrived) keeps the one already shown there
    const bool newWeather = leaderWeather && weather.WeatherId != leaderWeather;

    weather.LeaderId = leaderId;
    if (newWeather)
        weather.WeatherId = leaderWeather;

    if (leaderId == 0 || (!newLeader && !newWeather))
        return;

    if (newLeader)
        spdlog::info("Weather of worldspace {:X}:{:X} is now decided by player {}", acWorldSpaceId.ModId, acWorldSpaceId.BaseId, leaderId);

    for (const auto& [playerId, report] : m_reports)
    {
        if (report.WorldSpaceId != acWorldSpaceId)
            continue;

        // The leader only needs to hear that they lead
        if (playerId == leaderId && !newLeader)
            continue;

        Send(playerManager.GetById(playerId), acWorldSpaceId, weather);
    }
}

void WeatherService::Send(Player* apPlayer, const GameId& acWorldSpaceId, const WorldWeather& acWeather) const noexcept
{
    if (!apPlayer)
        return;

    NotifyWeatherChange notify{};
    notify.WorldSpaceId = acWorldSpaceId;
    notify.Id = acWeather.WeatherId;
    notify.Lead = apPlayer->GetId() == acWeather.LeaderId;

    apPlayer->Send(notify);
}

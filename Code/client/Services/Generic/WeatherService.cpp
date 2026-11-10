#include <Services/WeatherService.h>

#include <World.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>

#include <PlayerCharacter.h>
#include <Sky/Sky.h>
#include <Forms/TESWeather.h>
#include <Forms/TESWorldSpace.h>
#include <PerfProbe.h>

namespace
{
// The weather of the world map, not a real weather (same id in Skyrim and Fallout 4)
constexpr uint32_t kWorldMapWeather = 0xA6858;
} // namespace

WeatherService::WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WeatherService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&WeatherService::OnDisconnected>(this);
    m_weatherChangeConnection = aDispatcher.sink<NotifyWeatherChange>().connect<&WeatherService::OnWeatherChange>(this);
}

void WeatherService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Weather");
    if (!m_transport.IsOnline())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastCheck < 250ms)
        return;
    m_lastCheck = now;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    Sky* pSky = Sky::Get();
    if (!pPlayer || !pSky)
        return;

    const TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
    const uint32_t worldSpaceId = pWorldSpace ? pWorldSpace->formID : 0;

    const TESWeather* pWeather = pSky->GetWeather();
    uint32_t weatherId = pWeather ? pWeather->formID : 0;
    if (weatherId == kWorldMapWeather)
        weatherId = m_reportedWeatherId;

    if (worldSpaceId != m_worldSpaceId)
    {
        // Somewhere else: who leads the weather there is decided again
        if (m_overriding)
            Release();

        m_lead = false;
        m_sharedWeatherId = 0;
        m_worldSpaceId = worldSpaceId;
        Report(weatherId);
        return;
    }

    if (worldSpaceId == 0)
        return;

    if (m_lead)
    {
        if (weatherId && weatherId != m_reportedWeatherId)
            Report(weatherId);
        return;
    }

    // Showing the leader's weather: a script (or the game) can set another one, then it's shown again. Not every
    // check: SetWeather only changes it on the game's next weather update.
    if (m_sharedWeatherId && now - m_lastShow > 5s)
    {
        const TESWeather* pOverride = pSky->GetOverrideWeather();
        if (weatherId != m_sharedWeatherId || !pOverride || pOverride->formID != m_sharedWeatherId)
            Show(false);
    }
}

void WeatherService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    if (m_overriding)
        Release();

    m_worldSpaceId = 0;
    m_reportedWeatherId = 0;
    m_lead = false;
    m_sharedWeatherId = 0;
}

void WeatherService::OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    // Sent for where the player was: already somewhere else
    if (modSystem.GetGameId(acMessage.WorldSpaceId) != m_worldSpaceId || m_worldSpaceId == 0)
        return;

    if (acMessage.Lead)
    {
        if (!m_lead)
            spdlog::info("Weather: this game decides the weather of worldspace {:X}", m_worldSpaceId);

        m_lead = true;
        m_sharedWeatherId = 0;
        if (m_overriding)
            Release();

        // What it is now (the one shown until now): the others keep it until this game changes it
        if (const Sky* pSky = Sky::Get(); pSky && pSky->GetWeather() && pSky->GetWeather()->formID != kWorldMapWeather)
            Report(pSky->GetWeather()->formID);
        return;
    }

    const uint32_t weatherId = modSystem.GetGameId(acMessage.Id);
    if (!weatherId)
        return;

    const bool arriving = m_sharedWeatherId == 0;
    m_lead = false;
    m_sharedWeatherId = weatherId;
    Show(arriving);
}

void WeatherService::Report(uint32_t aWeatherId) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    RequestWeatherChange request{};
    if (m_worldSpaceId && !modSystem.GetServerModId(m_worldSpaceId, request.WorldSpaceId))
    {
        spdlog::warn("{}: worldspace {:X} has no server id", __FUNCTION__, m_worldSpaceId);
        return;
    }
    if (aWeatherId && !modSystem.GetServerModId(aWeatherId, request.Id))
        spdlog::warn("{}: weather {:X} has no server id", __FUNCTION__, aWeatherId);

    m_reportedWeatherId = aWeatherId;
    m_transport.Send(request);
}

void WeatherService::Show(bool aInstant) noexcept
{
    m_lastShow = std::chrono::steady_clock::now();

    Sky* pSky = Sky::Get();
    TESWeather* pWeather = Cast<TESWeather>(TESForm::GetById(m_sharedWeatherId));
    if (!pSky || !pWeather)
    {
        spdlog::warn("{}: weather {:X} not found", __FUNCTION__, m_sharedWeatherId);
        return;
    }

    if (aInstant)
        pSky->ForceWeather(pWeather);
    else
        pSky->SetWeather(pWeather, true, false);

    m_overriding = true;
}

void WeatherService::Release() noexcept
{
    if (Sky* pSky = Sky::Get())
        pSky->ReleaseWeatherOverride();

    m_overriding = false;
}

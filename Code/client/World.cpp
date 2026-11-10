#include <HivemindPCH.h>

#include "World.h"

#include <Services/DiscoveryService.h>
#include <Services/InputService.h>
#include <Services/TransportService.h>
#include <Services/RunnerService.h>
#include <Services/ImguiService.h>
#include <Services/PapyrusService.h>
#include <Services/DiscordService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/CommandService.h>
#include <Services/CalendarService.h>
#include <Services/StringCacheService.h>
#include <Services/PlayerService.h>
#include <Services/CombatService.h>
#include <Services/WeatherService.h>
#include <Services/MapService.h>
#include <Services/MovementService.h>
#include <Services/ParkourService.h>
#include <Services/TacticsService.h>
#if TP_FALLOUT4
#include <Services/WorkshopService.h>
#include <Services/FactionService.h>
#include <Services/BuildToolsService.h>
#include <Services/PipboyMapService.h>
#include <Services/CompanionService.h>
#include <Services/ActorBarsService.h>
#include <Services/Vats76Service.h>
#include <Services/NemesisService.h>
#include <Services/HudService.h>
#include <Services/BikeService.h>
#include <Services/CaravanService.h>
#include <Services/GrappleService.h>
#include <Services/DualWieldService.h>
#include <Services/SteamService.h>
#include <Services/EmoteService.h>
#include <Services/BetterInteractService.h>
#include <Services/DialogueVoteService.h>
#include <Services/CinematicService.h>
#include <Services/WinterOfAtomService.h>
#endif

#include <Events/PreUpdateEvent.h>
#include <Events/UpdateEvent.h>

#include <PerfProbe.h>
#include <PerfSampler.h>

World::World()
    : m_runner(m_dispatcher)
    , m_transport(*this, m_dispatcher)
    , m_modSystem(m_dispatcher)
    , m_lastFrameTime{std::chrono::high_resolution_clock::now()}
{
    ctx().emplace<ImguiService>();
    ctx().emplace<DiscoveryService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_transport, m_dispatcher);
    ctx().emplace<InputService>(ctx().at<OverlayService>());
    ctx().emplace<CharacterService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DebugService>(m_dispatcher, *this, m_transport, ctx().at<ImguiService>());
    ctx().emplace<PapyrusService>(m_dispatcher);
    ctx().emplace<DiscordService>(m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CalendarService>(*this, m_dispatcher, m_transport);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<PartyService>(*this, m_dispatcher, m_transport);
    ctx().emplace<ActorValueService>(*this, m_dispatcher, m_transport);
    ctx().emplace<InventoryService>(*this, m_dispatcher, m_transport);
    ctx().emplace<MagicService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CommandService>(*this, m_transport, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher, m_transport);
    ctx().emplace<StringCacheService>(m_dispatcher);
    ctx().emplace<CombatService>(*this, m_transport, m_dispatcher);
    ctx().emplace<WeatherService>(*this, m_transport, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher, m_transport);
#if TP_FALLOUT4
    ctx().emplace<MovementService>(*this, m_dispatcher);
    ctx().emplace<ParkourService>(*this, m_dispatcher);
    ctx().emplace<TacticsService>(*this, m_dispatcher);
    ctx().emplace<WorkshopService>(*this, m_dispatcher, m_transport);
    ctx().emplace<BuildToolsService>(*this, m_dispatcher);
    ctx().emplace<PipboyMapService>(*this, m_dispatcher);
    ctx().emplace<CompanionService>(*this, m_dispatcher);
    ctx().emplace<ActorBarsService>(*this, m_dispatcher);
    ctx().emplace<Vats76Service>(*this, m_dispatcher);
    ctx().emplace<NemesisService>(*this, m_dispatcher, m_transport);
    ctx().emplace<HudService>(*this, m_dispatcher);
    ctx().emplace<BikeService>(*this, m_dispatcher);
    ctx().emplace<CaravanService>(*this, m_dispatcher);
    ctx().emplace<GrappleService>(*this, m_dispatcher);
    ctx().emplace<DualWieldService>(*this, m_dispatcher);
    ctx().emplace<SteamService>(*this, m_dispatcher, m_transport);
    ctx().emplace<EmoteService>(*this, m_dispatcher);
    ctx().emplace<BetterInteractService>(*this, m_dispatcher);
    ctx().emplace<DialogueVoteService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CinematicService>(*this, m_dispatcher);
    ctx().emplace<WinterOfAtomService>(*this, m_dispatcher);
    ctx().emplace<FactionService>(*this, m_dispatcher, m_transport);
#endif
}

World::~World() = default;

void World::Update() noexcept
{
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(cDelta).count();

    {
        PerfProbe::Section _("PreUpdate");
        m_dispatcher.trigger(PreUpdateEvent(cDeltaSeconds));
    }

    // Force run this before so we get the tasks scheduled to run
    {
        PerfProbe::Section _("Runner");
        m_runner.OnUpdate(UpdateEvent(cDeltaSeconds));
    }
    m_dispatcher.trigger(UpdateEvent(cDeltaSeconds));

    // the frame, and our share of it (our updates, not our hooks inside the game's own frame); a slow one names its
    // biggest parts (the messages that arrived, our updates)
    if (PerfProbe::Enabled())
    {
        if (const std::string slow = PerfProbe::Frame(cDeltaSeconds, std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - cNow).count());
            !slow.empty())
            spdlog::info("{}", slow);
        if (const std::string report = PerfProbe::Report(false); !report.empty())
            spdlog::info("{}", report);
    }
    PerfSampler::Frame(cDeltaSeconds * 1000.0);
}

RunnerService& World::GetRunner() noexcept
{
    return m_runner;
}

TransportService& World::GetTransport() noexcept
{
    return m_transport;
}

ModSystem& World::GetModSystem() noexcept
{
    return m_modSystem;
}

uint64_t World::GetTick() const noexcept
{
    return m_transport.GetClock().GetCurrentTick();
}

void World::Create() noexcept
{
    if (!entt::locator<World>::has_value())
    {
        entt::locator<World>::emplace();
    }
}

World& World::Get() noexcept
{
    return entt::locator<World>::value();
}

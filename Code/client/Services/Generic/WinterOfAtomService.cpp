#include <Services/WinterOfAtomService.h>

#if TP_FALLOUT4

#include <Events/UpdateEvent.h>
#include <World.h>
#include <PlayerCharacter.h>
#include <Forms/TESGlobal.h>
#include <Components.h>
#include <WinterOfAtom.h>
#include <Services/TransportService.h>
#include <Interface/MainMenuIntegration.h>
#include <Messages/SetTimeCommandRequest.h>
#include <PerfProbe.h>

namespace
{
// Fallout4.esm's Pip-Boys: the vault one, the dusty one off the vault skeleton, Vault 81's
constexpr uint32_t kPipboys[] = {0x00021B3B, 0x0006D7B8, 0x001B9B87};
// the prologue's hour (at night, in a blizzard; the blizzard is the prologue script's)
constexpr uint8_t kPrologueHour = 22;
}

WinterOfAtomService::WinterOfAtomService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WinterOfAtomService::OnUpdate>(this);
}

void WinterOfAtomService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("WinterOfAtom");
    m_sinceUpdate += acEvent.Delta;
    if (m_sinceUpdate < 1.0)
        return;
    m_sinceUpdate = 0.0;

    UpdatePipboysOnTable();
    UpdatePrologueNight();
    WinterOfAtom::UpdateSubtitles();
}

void WinterOfAtomService::UpdatePrologueNight() noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsConnected())
    {
        m_nightSet = false;
        return;
    }
    if (m_nightSet || !IsHostingWorld() || !WinterOfAtom::IsPrequelLoaded())
        return;

    const TESGlobal* pStartDone = WinterOfAtom::GetGlobal(WinterOfAtom::kStartDone);
    const TESGlobal* pPrologueOver = WinterOfAtom::GetGlobal(WinterOfAtom::kTrainJobReady);
    if (!pStartDone || !pPrologueOver || pStartDone->f != 1.f || pPrologueOver->f != 0.f)
        return;

    SetTimeCommandRequest request{};
    request.Hours = kPrologueHour;
    request.Minutes = 0;
    transport.Send(request);
    m_nightSet = true;
    spdlog::info("WinterOfAtomService: the prologue runs at night, the server's time set to {}:00", kPrologueHour);
}

void WinterOfAtomService::UpdatePipboysOnTable() noexcept
{
    TESGlobal* pGlobal = WinterOfAtom::GetGlobal(WinterOfAtom::kPipboysOnTable);
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pGlobal || !pPlayer)
        return;

    auto& modSystem = m_world.GetModSystem();
    Vector<GameId> pipboys;
    for (const uint32_t formId : kPipboys)
    {
        GameId id{};
        if (modSystem.GetServerModId(formId, id))
            pipboys.push_back(id);
    }
    const auto wearsPipboy = [&pipboys](const Actor* apActor)
    {
        for (const auto& entry : apActor->GetEquipment().Entries)
        {
            if (std::find(pipboys.begin(), pipboys.end(), entry.BaseId) != pipboys.end())
                return true;
        }
        return false;
    };

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        const Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (pActor && wearsPipboy(pActor))
            m_hadPipboy.insert(view.get<PlayerComponent>(entity).Id);
    }

    int count = wearsPipboy(pPlayer) ? 0 : 1;
    for (const auto& [playerId, name] : m_world.GetPartyService().GetPlayers())
    {
        if (!m_hadPipboy.contains(playerId))
            ++count;
    }

    if (pGlobal->f != static_cast<float>(count))
    {
        spdlog::info("WinterOfAtomService: {} Pip-Boy{} on the prologue trader's table", count, count == 1 ? "" : "s");
        pGlobal->f = static_cast<float>(count);
    }
}

#endif

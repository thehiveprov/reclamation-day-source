#if TP_FALLOUT4

#include <Services/BikeService.h>

#include <Components.h>
#include <Events/OverlayRestartedEvent.h>
#include <Events/UpdateEvent.h>
#include <Forms/TESForm.h>
#include <Games/Fallout4/BikeLink.h>
#include <Interface/GameHud.h>
#include <Messages/NotifyMovementAction.h>
#include <Messages/RequestMovementAction.h>
#include <Services/CompanionService.h>
#include <Services/OverlayService.h>
#include <Services/TransportService.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <Utils.h>
#include <World.h>

namespace
{
constexpr float kRideInterval = 1.f / 15.f; // seconds between the states of a ridden bike sent to the others
constexpr float kParkedInterval = 5.f;      // and where a parked one stands
constexpr double kRiderGone = 3.0;          // nothing heard for this long: they're off it, or out of range
constexpr double kParkedGone = 20.0;        // they've left

// What the plugin asks, so the bike doesn't knock other players down from this game
bool IsRemotePlayer(uint32_t aFormId)
{
    TESForm* pForm = TESForm::GetById(aFormId);
    Actor* pActor = pForm ? Cast<Actor>(pForm) : nullptr;
    return pActor && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer();
}
} // namespace

BikeService::BikeService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&BikeService::OnUpdate>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&BikeService::OnNotifyMovementAction>(this);
    m_overlayRestartedConnection = aDispatcher.sink<OverlayRestartedEvent>().connect<&BikeService::OnOverlayRestarted>(this);
}

void BikeService::OnOverlayRestarted(const OverlayRestartedEvent&) noexcept
{
    m_gauges.clear();
}

void BikeService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (!BikeLink::IsPresent())
        return;
    const float delta = std::clamp(static_cast<float>(acEvent.Delta), 0.f, 0.1f);
    m_time += delta;
    if (!m_linked)
    {
        m_linked = true;
        // The attack and reload buttons stay live while riding for the left hand slot (DualWieldService keeps them from
        // the game), and the plugin knows the map's key is ours
        BikeLink::SetAttackInput(true);
        BikeLink::SetRemotePlayerTest(&IsRemotePlayer);
    }
    // Which dog rides in the sidecar: once a second is plenty
    if ((m_dogTimer -= delta) <= 0.f)
    {
        m_dogTimer = 1.f;
        const Actor* pDog = m_world.ctx().at<CompanionService>().GetDogmeatSlotActor();
        BikeLink::SetDog(pDog ? pDog->formID : 0u);
    }
    SendGauges();
    SendRide(delta);
    SendParked(delta);
    DropOld();
}

// Speed in whole km/h, fuel in whole hundredths: a new event only when one of them shows a change
void BikeService::SendGauges() noexcept
{
    BikeLink::Hud hud{};
    std::string state = "null";
    // (not over a menu: the compass tells when the game's HUD is up at all)
    if (BikeLink::GetHud(hud) && GameHud::WouldShow(GameHud::Piece::kCompass))
    {
        const float share = hud.Tank > 0.f ? std::clamp(hud.Fuel / hud.Tank, 0.f, 1.f) : 0.f;
        state = fmt::format("{{\"kmh\":{},\"fuel\":{:.2f},\"on\":{},\"lamp\":{},\"gear\":{}}}", static_cast<int>(hud.SpeedKmh + 0.5f), share, hud.EngineOn ? "true" : "false",
                            hud.Headlight ? "true" : "false", hud.Gear);
    }
    if (state != m_gauges)
    {
        m_gauges = state;
        m_world.ctx().at<OverlayService>().SendEvent("bike", state);
    }
}

void BikeService::SendRide(float aDelta) noexcept
{
    auto& transport = m_world.GetTransport();
    BikeLink::NetState state{};
    const bool onBike = BikeLink::GetNetState(state);
    if (!transport.IsOnline())
    {
        m_wasOnBike = false;
        return;
    }
    if (!onBike && !m_wasOnBike)
        return;
    m_rideTimer -= aDelta;
    if (onBike && m_wasOnBike && m_rideTimer > 0.f)
        return;
    m_rideTimer = kRideInterval;
    m_wasOnBike = onBike;

    RequestMovementAction request{};
    request.Type = RequestMovementAction::kBike;
    if (onBike)
    {
        auto& out = request.Bike;
        out.Stage = state.Stage;
        out.Side = state.Side;
        out.Paint = state.Paint;
        out.Flags = state.Flags;
        out.Heading = state.Heading;
        out.Lean = state.Lean;
        out.Pitch = state.Pitch;
        out.Steer = state.Steer;
        out.Suspension = state.Suspension;
        out.WheelSpeed = state.WheelSpeed;
        out.Rpm = state.Rpm;
        out.Throttle = state.Throttle;
        out.StageTime = state.StageTime;
        out.Stand = state.Stand;
        out.StandGround = state.StandGround;
    }
    transport.Send(request);
}

void BikeService::SendParked(float aDelta) noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
    {
        m_hadParked = false;
        m_parkedTimer = 0.f;
        return;
    }
    if ((m_parkedTimer -= aDelta) > 0.f)
        return;
    m_parkedTimer = kParkedInterval;

    BikeLink::Parked parked{};
    const bool has = BikeLink::GetParked(parked);
    if (!has && !m_hadParked)
        return;
    m_hadParked = has;

    RequestMovementAction request{};
    request.Type = RequestMovementAction::kBikeParked;
    request.Parked.Shown = has;
    if (has)
    {
        request.Parked.Paint = parked.Paint;
        std::copy(std::begin(parked.At), std::end(parked.At), std::begin(request.Parked.At));
        request.Parked.Heading = parked.Heading;
        request.Parked.Roll = parked.Roll;
        World::Get().GetModSystem().GetServerModId(parked.CellOrWorld, request.Parked.CellOrWorld);
    }
    transport.Send(request);
}

void BikeService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    if (acMessage.Type == RequestMovementAction::kBike)
    {
        Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
        if (!pActor)
            return;
        const auto& in = acMessage.Bike;
        if (in.Stage == 0)
        {
            BikeLink::SetRemote(pActor->formID, nullptr);
            std::erase_if(m_riders, [pActor](const RemoteRider& acRider) { return acRider.ActorId == pActor->formID; });
            return;
        }
        BikeLink::NetState state{};
        state.Stage = in.Stage;
        state.Side = in.Side;
        state.Paint = in.Paint;
        state.Flags = in.Flags;
        state.Heading = in.Heading;
        state.Lean = in.Lean;
        state.Pitch = in.Pitch;
        state.Steer = in.Steer;
        state.Suspension = in.Suspension;
        state.WheelSpeed = in.WheelSpeed;
        state.Rpm = in.Rpm;
        state.Throttle = in.Throttle;
        state.StageTime = in.StageTime;
        state.Stand = in.Stand;
        state.StandGround = in.StandGround;
        BikeLink::SetRemote(pActor->formID, &state);
        auto it = std::find_if(m_riders.begin(), m_riders.end(), [pActor](const RemoteRider& acRider) { return acRider.ActorId == pActor->formID; });
        if (it == m_riders.end())
            m_riders.push_back({pActor->formID, m_time});
        else
            it->LastSeen = m_time;
    }
    else if (acMessage.Type == RequestMovementAction::kBikeParked)
    {
        BikeLink::Parked parked{};
        const auto& in = acMessage.Parked;
        if (in.Shown)
        {
            parked.Paint = in.Paint;
            std::copy(std::begin(in.At), std::end(in.At), std::begin(parked.At));
            parked.Heading = in.Heading;
            parked.Roll = in.Roll;
            parked.CellOrWorld = World::Get().GetModSystem().GetGameId(in.CellOrWorld);
        }
        BikeLink::SetRemoteParked(acMessage.ServerId, parked, in.Shown);
        auto it = std::find_if(m_parked.begin(), m_parked.end(), [&acMessage](const RemoteParked& acParked) { return acParked.ServerId == acMessage.ServerId; });
        if (!in.Shown)
        {
            if (it != m_parked.end())
                m_parked.erase(it);
        }
        else if (it == m_parked.end())
            m_parked.push_back({acMessage.ServerId, m_time});
        else
            it->LastSeen = m_time;
    }
}

void BikeService::DropOld() noexcept
{
    for (auto it = m_riders.begin(); it != m_riders.end();)
    {
        if (m_time - it->LastSeen > kRiderGone)
        {
            BikeLink::SetRemote(it->ActorId, nullptr);
            it = m_riders.erase(it);
        }
        else
            ++it;
    }
    for (auto it = m_parked.begin(); it != m_parked.end();)
    {
        if (m_time - it->LastSeen > kParkedGone)
        {
            BikeLink::SetRemoteParked(it->ServerId, BikeLink::Parked{}, false);
            it = m_parked.erase(it);
        }
        else
            ++it;
    }
}

#endif

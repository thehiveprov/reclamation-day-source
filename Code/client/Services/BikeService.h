#pragma once

struct World;
struct UpdateEvent;
struct OverlayRestartedEvent;
struct NotifyMovementAction;

/**
 * @brief The motorcycle (the RDVehicles plugin, Games/Fallout4/BikeLink.h) in Reclamation Day: its gauges in the overlay
 * ("bike" event, drawn by the HUD), and other players seeing it: the ridden bike's state and where a parked one stands go
 * out as RequestMovementAction kBike and kBikeParked, and what comes in goes to the plugin, which draws them.
 * Fallout 4 only; RDVehicles' docs/BIKE_FINISH.md.
 */
class BikeService
{
public:
    BikeService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~BikeService() noexcept = default;

    TP_NOCOPYMOVE(BikeService);

private:
    struct RemoteRider
    {
        uint32_t ActorId{};
        double LastSeen{};
    };
    struct RemoteParked
    {
        uint32_t ServerId{};
        double LastSeen{};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnOverlayRestarted(const OverlayRestartedEvent&) noexcept;
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;
    void SendGauges() noexcept;
    void SendRide(float aDelta) noexcept;
    void SendParked(float aDelta) noexcept;
    void DropOld() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_overlayRestartedConnection;
    entt::scoped_connection m_movementActionConnection;

    double m_time{0.0};
    bool m_linked{false};
    std::string m_gauges;
    float m_dogTimer{0.f};
    float m_rideTimer{0.f};
    bool m_wasOnBike{false};
    float m_parkedTimer{0.f};
    bool m_hadParked{false};
    Vector<RemoteRider> m_riders;
    Vector<RemoteParked> m_parked;
};

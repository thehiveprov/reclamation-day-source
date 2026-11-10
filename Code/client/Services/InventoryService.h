#pragma once

struct World;
struct TransportService;

struct UpdateEvent;
struct NotifyObjectInventoryChanges;
struct NotifyInventoryChanges;
struct InventoryChangeEvent;
struct EquipmentChangeEvent;
struct NotifyEquipmentChanges;
struct Actor;
struct RemoteComponent;
struct LocalComponent;
struct NotifyActorTrade;

#include <Structs/Inventory.h>

/**
 * @brief Manages inventories of actors and containers.
 *
 * The initial contents of actor inventories are synced through spawn messages through CharacterService
 * @see CharacterService
 * The initial contents of container inventories are synced through the ObjectService
 * @see ObjectService
 * The InventoryService manages any changes to both the current equipment of actors,
 * and the contents of both actor and object inventories.
 */
struct InventoryService
{
    InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~InventoryService() noexcept = default;

    TP_NOCOPYMOVE(InventoryService);

    /**
     * @brief Check weapon state.
     * @see RunWeaponStateUpdates
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;
    /**
     * @brief Sends out inventory changes made by the local client.
     */
    void OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept;
    /**
     * @brief Sends out equipment changes made by the local client.
     */
    void OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept;

    /**
     * @brief Applies inventory changes sent by the server.
     */
    void OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept;
    /**
     * @brief Applies equipment changes sent by the server.
     */
    void OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept;

#if TP_FALLOUT4
    /**
     * The character the local player trades with right now when another player's game runs it (form id, 0 when
     * none): what it's given to wear in the trade menu is let through on this copy (EquipHook), see RunTradeUpdates.
     */
    static uint32_t GetTradePartnerId() noexcept { return s_tradePartnerId; }
    /**
     * @brief Makes a trade another player made with a character this game owns (see RequestActorTrade).
     */
    void OnNotifyActorTrade(const NotifyActorTrade& acMessage) noexcept;
#endif

private:
    /**
     * Checks whether local actors their weapon draw states have changed,
     * and if so, send the new states to the server.
     */
    void RunWeaponStateUpdates() noexcept;

#if TP_FALLOUT4
    /**
     * Fallout 4 equips/unequips through many paths (Pip-Boy, workbenches, scripts, armor swaps), so instead of relying
     * on equip events, periodically compare the local player's worn set to the last one sent, and send the complete set
     * when it changed. Receivers reconcile to it (see ReconcileEquipment). The other characters this game runs
     * (companions, settlers, enemies) send theirs after they put something on or took something off.
     */
    void RunEquipmentStateUpdates() noexcept;
    void SendEquipmentState(const LocalComponent& acLocal, const Inventory& acEquipment) const noexcept;
    /**
     * Makes a character that another player's game runs (that player's own, or one it owns) wear what it wears there:
     * takes off what it doesn't, puts on what the copy lacks (giving it the item if it doesn't have it).
     */
    static void ReconcileEquipment(Actor* apActor, RemoteComponent& arRemote, const Inventory& acWanted) noexcept;
    // While a remote copy is in power armor: its frame pieces (helmet included) by item, see ReconcileEquipment
    static void SyncPowerArmorPieces(Actor* apActor, const Inventory& acWanted) noexcept;

    Inventory m_lastSentEquipment{};
    bool m_hasSentEquipment = false;
    // The characters of this game, other than the player, whose worn set changed and hasn't been sent yet: by form id,
    // with the time of their last change
    TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> m_changedEquipment{};

    /**
     * Trading with a companion or settler that another player's game runs: copies of a character take no item
     * changes, so what the trade menu changed on this copy (items in and out, what it wears) is sent to its owner when
     * the menu closes (RequestActorTrade).
     */
    void RunTradeUpdates() noexcept;
    void FinishTrade() noexcept;

    struct Trade
    {
        uint32_t ActorId{};
        uint32_t ServerId{};
        uint32_t OwnershipEpoch{};
        Inventory Before{};
        Inventory WornBefore{};
    };
    Trade m_trade{};
    bool m_tradeMenuSeen = false;
    static inline uint32_t s_tradePartnerId = 0;
#endif

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_inventoryConnection;
    entt::scoped_connection m_equipmentConnection;
    entt::scoped_connection m_inventoryChangeConnection;
    entt::scoped_connection m_equipmentChangeConnection;
    entt::scoped_connection m_actorTradeConnection;
};

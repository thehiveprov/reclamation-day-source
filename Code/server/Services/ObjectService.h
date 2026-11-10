#pragma once

#include <Events/PacketEvent.h>
#include <Messages/NotifyItemDropped.h>

struct World;
struct PlayerLeaveCellEvent;
struct ActivateRequest;
struct LockChangeRequest;
struct AssignObjectsRequest;
struct ScriptAnimationRequest;
struct RequestQuestItemTaken;
struct RequestDialogueLock;
struct PlayerLeaveEvent;
struct PlayerJoinEvent;
struct RequestPowerArmor;
struct RequestDropItem;
struct RequestItemPickedUp;
struct RequestTerminalItemRun;
struct RequestDestructionStage;
struct RequestPipboyLight;

/**
 * @brief Manages (interactive) objects and relays interactions with said objects.
 */
class ObjectService
{
public:
    ObjectService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    void OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept;
    void OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>&) noexcept;
    void OnActivate(const PacketEvent<ActivateRequest>&) const noexcept;
    void OnLockChange(const PacketEvent<LockChangeRequest>&) const noexcept;
    void OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>&) noexcept;
    void OnQuestItemTaken(const PacketEvent<RequestQuestItemTaken>&) const noexcept;
    void OnDialogueLock(const PacketEvent<RequestDialogueLock>&) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent&) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent&) noexcept;
    void OnPowerArmor(const PacketEvent<RequestPowerArmor>&) noexcept;
    void OnDropItem(const PacketEvent<RequestDropItem>&) noexcept;
    void OnItemPickedUp(const PacketEvent<RequestItemPickedUp>&) noexcept;
    // A terminal menu item a player ran: run again by the players near them
    void OnTerminalItemRun(const PacketEvent<RequestTerminalItemRun>&) const noexcept;
    // A destructible object's stage went up in a player's game: passed to the players near them
    void OnDestructionStage(const PacketEvent<RequestDestructionStage>&) const noexcept;
    // A player's Pip-Boy light turned on or off: passed to everyone else, like power armor
    void OnPipboyLight(const PacketEvent<RequestPipboyLight>&) const noexcept;

    // Shared world items dropped by players, by item id (the drop as sent to everyone)
    TiltedPhoques::Map<uint32_t, NotifyItemDropped> m_worldItems;
    uint32_t m_nextWorldItemId = 1;

    // NPC server id -> id of the player whose conversation with it is running
    TiltedPhoques::Map<uint32_t, uint32_t> m_dialogueLocks;

    // Power armor frame (GameId as ModId << 32 | BaseId) -> id of the player who claimed it
    TiltedPhoques::Map<uint64_t, uint32_t> m_powerArmorClaims;

    World& m_world;

    entt::scoped_connection m_leaveCellConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_scriptAnimationConnection;
    entt::scoped_connection m_questItemTakenConnection;
    entt::scoped_connection m_dialogueLockConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_powerArmorConnection;
    entt::scoped_connection m_dropItemConnection;
    entt::scoped_connection m_itemPickedUpConnection;
    entt::scoped_connection m_terminalItemRunConnection;
    entt::scoped_connection m_destructionStageConnection;
    entt::scoped_connection m_pipboyLightConnection;
};

#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/NotifyItemDropped.h>

struct ServerTimeSettings;
struct DisconnectedEvent;
struct World;
struct ActivateEvent;
struct TransportService;
struct NotifyActivate;
struct LockChangeEvent;
struct NotifyLockChange;
struct CellChangeEvent;
struct ScriptAnimationEvent;
struct AssignObjectsResponse;
struct NotifyScriptAnimation;
struct UpdateEvent;
struct NotifyQuestItemTaken;
struct NotifyPowerArmor;
struct DropItemEvent;
struct NotifyItemDropped;
struct NotifyItemPickedUp;
struct TerminalItemRunEvent;
struct NotifyTerminalItemRun;
struct DestructionStageEvent;
struct NotifyDestructionStage;
struct NotifyPipboyLight;

/**
 * @brief Handles objects in the environment.
 */
class ObjectService final : public BSTEventSink<TESActivateEvent>
{
public:
    ObjectService(World&, entt::dispatcher&, TransportService&);

private:
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCellChange(const CellChangeEvent&) noexcept;
    void OnAssignObjectsResponse(const AssignObjectsResponse&) noexcept;
    void OnActivate(const ActivateEvent&) noexcept;
    void OnActivateNotify(const NotifyActivate&) noexcept;
    void OnLockChange(const LockChangeEvent&) noexcept;
    void OnLockChangeNotify(const NotifyLockChange&) noexcept;
    void OnScriptAnimationEvent(const ScriptAnimationEvent&) noexcept;
    void OnNotifyScriptAnimation(const NotifyScriptAnimation&) noexcept;

#if TP_FALLOUT4
    /**
     * Quest items. Each player's quests are their own, so everyone keeps their own copy of a quest item (in the
     * world, in containers and on bodies), except players teamed up on the item's quest: among them it only exists
     * once. While the local player has a container/body open (we see that activation), watch for quest items
     * appearing in their inventory; for one of a quest done together, tell the teammates to remove it from their copy
     * of that container/body. Quest items picked up from the world: see OnActivate.
     */
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnNotifyQuestItemTaken(const NotifyQuestItemTaken&) noexcept;
    // A quest item the local player took: removed for the teammates on its quest, if it's for one done together
    void ShareQuestItemTaken(uint32_t aSourceId, uint32_t aItemId, uint32_t aCount, const Vector<uint32_t>& acQuests) noexcept;

    // Objects whose activation we just applied for another player, to not send it back (see OnActivate)
    TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> m_remotelyActivated{};

    uint32_t m_lootSourceId = 0;
    std::chrono::steady_clock::time_point m_lootSourceOpened{};
    TiltedPhoques::Map<uint32_t, uint32_t> m_questItemsBeforeLooting{}; // counts by base form id

    /**
     * Power armor: when the local player gets into or out of a frame, other players put their copy of the character
     * into or out of their own copy of that frame, so it uses the power armor skeleton and animations and the frame
     * doesn't stay standing around. Frames are claimed by the first player to use them, like in Fallout 76: the other
     * players can't activate them.
     */
    void RunPowerArmorUpdates() noexcept;
    void OnNotifyPowerArmor(const NotifyPowerArmor&) noexcept;
    void ApplyPowerArmorClaims() noexcept;

    uint32_t m_lastFrameId = 0; // last power armor frame the local player activated
    uint32_t m_powerArmorFrameId = 0; // frame the local player is in, 0 when not in power armor
    bool m_inPowerArmor = false;
    std::chrono::steady_clock::time_point m_lastPowerArmorSend{};
    // Frame (server GameId as ModId << 32 | BaseId) -> player id of its owner
    TiltedPhoques::Map<uint64_t, uint32_t> m_powerArmorClaims{};

    /**
     * Items players drop are shared world items: one id on the server, a copy in each player's world (dropped by the
     * local copy of the dropper's character), and picking one up removes it everywhere.
     */
    void OnDropItemEvent(const DropItemEvent&) noexcept;
    void OnNotifyItemDropped(const NotifyItemDropped&) noexcept;
    void OnNotifyItemPickedUp(const NotifyItemPickedUp&) noexcept;
    bool SpawnWorldItem(const NotifyItemDropped&) noexcept;
    void RunWorldItemUpdates() noexcept;

    TiltedPhoques::Map<uint32_t, uint32_t> m_worldItemRefs{};   // item id -> reference form id here
    TiltedPhoques::Map<uint32_t, uint32_t> m_refWorldItems{};   // reference form id -> item id
    TiltedPhoques::Map<uint32_t, NotifyItemDropped> m_pendingWorldItems{}; // dropper's character not here yet

    /**
     * Terminals: a menu item the local player runs (see Games/Fallout4/Terminals.h) is run by the players nearby on
     * their copy of the terminal, so what its script does to the world (turrets, doors, robots) happens everywhere.
     */
    void OnTerminalItemRun(const TerminalItemRunEvent&) noexcept;
    void OnNotifyTerminalItemRun(const NotifyTerminalItemRun&) noexcept;

    /**
     * Destructible objects: a stage that goes up here (see Games/Fallout4/Destruction.h) is sent with the object's
     * health, and the players nearby damage their copy down to it, which runs the same stage in their game.
     */
    void OnDestructionStage(const DestructionStageEvent&) noexcept;
    void OnNotifyDestructionStage(const NotifyDestructionStage&) noexcept;

    /**
     * The Pip-Boy light: the local player's is sent when it turns on or off (and now and then while it's on, for
     * copies of the character spawned since on other screens); the other players' characters get theirs here (see
     * Games/Fallout4/PipboyLights.h).
     */
    void RunPipboyLightUpdates() noexcept;
    void OnNotifyPipboyLight(const NotifyPipboyLight&) noexcept;

    bool m_pipboyLightOn = false;
    std::chrono::steady_clock::time_point m_lastPipboyLightSend{};
    // Other players' characters whose light is on, by server id, with when it was last heard of
    TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> m_pipboyLights{};
#endif

    BSTEventResult OnEvent(const TESActivateEvent*, const EventDispatcher<TESActivateEvent>*) override;

    entt::entity CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_onActivateConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_lockChangeNotifyConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_scriptAnimationConnection;
    entt::scoped_connection m_scriptAnimationNotifyConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_questItemTakenConnection;
    entt::scoped_connection m_powerArmorConnection;
    entt::scoped_connection m_dropItemConnection;
    entt::scoped_connection m_itemDroppedConnection;
    entt::scoped_connection m_itemPickedUpConnection;
    entt::scoped_connection m_terminalItemRunConnection;
    entt::scoped_connection m_terminalItemRunNotifyConnection;
    entt::scoped_connection m_destructionStageConnection;
    entt::scoped_connection m_destructionStageNotifyConnection;
    entt::scoped_connection m_pipboyLightConnection;
};

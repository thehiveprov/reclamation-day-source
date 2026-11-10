#pragma once
#include "Structs/Inventory.h"
#include "Structs/ActorData.h"

struct ActorAddedEvent;
struct ActorRemovedEvent;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct EquipmentChangeEvent;
struct FormIdComponent;
struct ActionEvent;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct TESObjectCELL;
struct ServerReferencesMoveRequest;
struct NotifyInventoryChanges;
struct NotifyFactionsChanges;
struct NotifyRemoveCharacter;
struct NotifyOwnershipTransfer;
struct SpellCastEvent;
struct NotifySpellCast;
struct InterruptCastEvent;
struct NotifyInterruptCast;
struct AddTargetEvent;
struct NotifyAddTarget;
struct ProjectileLaunchedEvent;
struct NotifyProjectileLaunch;
struct MountEvent;
struct NotifyMount;
struct InitPackageEvent;
struct NotifyNewPackage;
struct NotifyRespawn;
struct BeastFormChangeEvent;
struct AddExperienceEvent;
struct NotifySyncExperience;
struct DialogueEvent;
struct NotifyDialogue;
struct SubtitleEvent;
struct NotifySubtitle;
struct DialogueLockEvent;
struct NotifyDialogueLock;
struct NotifyActorTeleport;
struct PartyJoinedEvent;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerCellChanged;

struct Actor;
struct World;
struct TransportService;

/**
 * @brief Handles actors and players.
 */
struct CharacterService
{
    CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~CharacterService() noexcept = default;

    TP_NOCOPYMOVE(CharacterService);

    static void DeleteTempActor(const uint32_t aFormId) noexcept;

    bool RequestOwnership(uint32_t aFormId, uint32_t aServerId, entt::entity aEntity) const noexcept;

    // Whoever starts a conversation with an NPC makes the choices; true if another player is talking to this NPC,
    // in which case the local player can't start a conversation with it and just follows along.
    bool IsInDialogueWithOtherPlayer(uint32_t aFormId) const noexcept;

#if TP_FALLOUT4
    // Puts a remote character's weapon away and draws it again: one put in its hands while it held one out is only shown,
    // and held right, once its own draw animation plays
    void RedrawWeapon(Actor* apActor) noexcept;
#endif

    void OnActorAdded(const ActorAddedEvent& acEvent) noexcept;
    void OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;
    void OnConnected(const ConnectedEvent& acConnectedEvent) const noexcept;
    void OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) const noexcept;
    void OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept;
    void OnCharacterSpawn(const CharacterSpawnRequest& acMessage) const noexcept;
    void OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept;
    void OnActionEvent(const ActionEvent& acActionEvent) const noexcept;
    void OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept;
    void OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) noexcept;
    void OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept;
    void OnMountEvent(const MountEvent& acEvent) const noexcept;
    void OnNotifyMount(const NotifyMount& acMessage) const noexcept;
    void OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept;
    void OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept;
    void OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept;
    void OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept;
    void OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept;
    void OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept;
    void OnDialogueEvent(const DialogueEvent& acEvent) noexcept;
    void OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept;
    void OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept;
    void OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept;
    void OnDialogueLockEvent(const DialogueLockEvent& acEvent) noexcept;
    void OnNotifyDialogueLock(const NotifyDialogueLock& acMessage) noexcept;
    void OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnNotifyPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept;
    void OnNotifyPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void OnNotifyPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept;

    void ProcessNewEntity(entt::entity aEntity) const noexcept;

private:
    void MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept;

    void RequestServerAssignment(entt::entity aEntity) const noexcept;
    void CancelServerAssignment(entt::entity aEntity, uint32_t aFormId) const noexcept;
    void DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept;
    void DeclineOwnership(uint32_t aServerId, uint32_t aOwnershipEpoch) const noexcept;
    void ReconcileActorData(entt::entity aEntity, Actor* apActor, uint32_t aOwnershipEpoch, const ActorData& acActorData, bool aApplyInventory, bool aIsLocalOwner) noexcept;

    Actor* CreateCharacterForEntity(entt::entity aEntity, TESObjectCELL* apCell) const noexcept;
    // The cell a copy we make at this position goes in, null while the game can't take it in yet (see the .cpp)
    TESObjectCELL* GetCellForCopy(const glm::vec3& acPosition) const noexcept;
    ActorData BuildActorData(Actor* apActor) const noexcept;

    void RunLocalUpdates() const noexcept;
    // No other player in our worldspace (or, inside, our cell): nobody's game gets our characters' movement then
    [[nodiscard]] bool IsAloneForSync() const noexcept;
    void RunRemoteUpdates() noexcept;
    void RunFactionsUpdates() const noexcept;
    void RunSpawnUpdates() const noexcept;
    void RunExperienceUpdates() noexcept;
    void ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    float m_cachedExperience = 0.f;

    // TODO: revamp this, read the local anim var like vampire lord?
    struct WeaponDrawData
    {
        WeaponDrawData() = default;
        WeaponDrawData(bool aDrawWeapon)
            : m_drawWeapon(aDrawWeapon)
        {
        }

        double m_timer = 0.0;
        bool m_drawWeapon = false;
        bool m_isFirstPass = true;
    };

    Map<uint32_t, WeaponDrawData> m_weaponDrawUpdates{};
    // Remote characters whose weapon is out but hidden as put away, since when (RunRemoteUpdates' tick)
    Map<uint32_t, uint64_t> m_hiddenDrawnWeapons{};
    // Remote characters putting their weapon away to draw it again (RedrawWeapon), since when (RunRemoteUpdates' tick)
    Map<uint32_t, uint64_t> m_weaponRedraws{};
    // Where the player is (worldspace, or cell inside) and since when (GetTickCount64), for GetCellForCopy
    mutable uint32_t m_copySpace{0};
    mutable uint64_t m_copySpaceSince{0};

    entt::scoped_connection m_referenceAddedConnection;
    entt::scoped_connection m_referenceRemovedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_actionConnection;
    entt::scoped_connection m_factionsConnection;
    entt::scoped_connection m_ownershipTransferConnection;
    entt::scoped_connection m_removeCharacterConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_assignCharacterConnection;
    entt::scoped_connection m_characterSpawnConnection;
    entt::scoped_connection m_referenceMovementSnapshotConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_notifyMountConnection;
    entt::scoped_connection m_initPackageConnection;
    entt::scoped_connection m_newPackageConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_beastFormChangeConnection;
    entt::scoped_connection m_addExperienceEventConnection;
    entt::scoped_connection m_syncExperienceConnection;
    entt::scoped_connection m_dialogueEventConnection;
    entt::scoped_connection m_dialogueSyncConnection;
    entt::scoped_connection m_subtitleEventConnection;
    entt::scoped_connection m_subtitleSyncConnection;
    entt::scoped_connection m_dialogueLockEventConnection;
    entt::scoped_connection m_dialogueLockNotifyConnection;
    entt::scoped_connection m_playerJoinedConnection;

    // Player id -> name chosen on the server, shown on that player's character in the world
    Map<uint32_t, String> m_playerNames{};
    // Player id -> where that player is (the server's worldspace and cell ids), for IsAloneForSync; ours is in it too
    struct PlayerPlace
    {
        GameId WorldSpaceId{};
        GameId CellId{};
    };
    mutable Map<uint32_t, PlayerPlace> m_playerPlaces{};
    // Whether RunLocalUpdates last found us alone (it logs the changes)
    mutable bool m_syncAlone{false};
    entt::scoped_connection m_playerLeftConnection;
    entt::scoped_connection m_playerCellChangedConnection;

    // NPC server id -> player id of the conversation running with it
    TiltedPhoques::Map<uint32_t, uint32_t> m_dialogueLocks;
    entt::scoped_connection m_actorTeleportConnection;
    entt::scoped_connection m_partyJoinedConnection;
};

#include <Services/ObjectService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/PlayerLeaveCellEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/RequestQuestItemTaken.h>
#include <Messages/NotifyQuestItemTaken.h>
#include <Messages/RequestDialogueLock.h>
#include <Messages/NotifyDialogueLock.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Messages/RequestPowerArmor.h>
#include <Messages/NotifyPowerArmor.h>
#include <Messages/RequestDropItem.h>
#include <Messages/RequestItemPickedUp.h>
#include <Messages/NotifyItemPickedUp.h>
#include <Messages/RequestTerminalItemRun.h>
#include <Messages/NotifyTerminalItemRun.h>
#include <Messages/RequestDestructionStage.h>
#include <Messages/NotifyDestructionStage.h>
#include <Messages/RequestPipboyLight.h>
#include <Messages/NotifyPipboyLight.h>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&ObjectService::OnPlayerLeaveCellEvent>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&ObjectService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ObjectService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&ObjectService::OnLockChange>(this);
    m_scriptAnimationConnection = aDispatcher.sink<PacketEvent<ScriptAnimationRequest>>().connect<&ObjectService::OnScriptAnimationRequest>(this);
    m_questItemTakenConnection = aDispatcher.sink<PacketEvent<RequestQuestItemTaken>>().connect<&ObjectService::OnQuestItemTaken>(this);
    m_dialogueLockConnection = aDispatcher.sink<PacketEvent<RequestDialogueLock>>().connect<&ObjectService::OnDialogueLock>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&ObjectService::OnPlayerLeave>(this);
    m_playerJoinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&ObjectService::OnPlayerJoin>(this);
    m_powerArmorConnection = aDispatcher.sink<PacketEvent<RequestPowerArmor>>().connect<&ObjectService::OnPowerArmor>(this);
    m_dropItemConnection = aDispatcher.sink<PacketEvent<RequestDropItem>>().connect<&ObjectService::OnDropItem>(this);
    m_itemPickedUpConnection = aDispatcher.sink<PacketEvent<RequestItemPickedUp>>().connect<&ObjectService::OnItemPickedUp>(this);
    m_terminalItemRunConnection = aDispatcher.sink<PacketEvent<RequestTerminalItemRun>>().connect<&ObjectService::OnTerminalItemRun>(this);
    m_destructionStageConnection = aDispatcher.sink<PacketEvent<RequestDestructionStage>>().connect<&ObjectService::OnDestructionStage>(this);
    m_pipboyLightConnection = aDispatcher.sink<PacketEvent<RequestPipboyLight>>().connect<&ObjectService::OnPipboyLight>(this);
}

void ObjectService::OnDropItem(const PacketEvent<RequestDropItem>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;

    NotifyItemDropped notify;
    notify.ItemId = m_nextWorldItemId++;
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.ActorServerId = message.ActorServerId;
    notify.Item = message.Item;
    notify.LocalRefId = message.LocalRefId;
    notify.CellId = message.CellId;
    notify.WorldSpaceId = message.WorldSpaceId;
    notify.Position = message.Position;
    notify.Rotation = message.Rotation;

    m_worldItems[notify.ItemId] = notify;

    // Everyone, the dropper included: it learns the item id of its own copy
    GameServer::Get()->SendToPlayers(notify);

    spdlog::info("Player {} dropped {:X}:{:X} x{}, world item {}", notify.PlayerId, message.Item.BaseId.ModId, message.Item.BaseId.BaseId, message.Item.Count, notify.ItemId);
}

void ObjectService::OnItemPickedUp(const PacketEvent<RequestItemPickedUp>& acMessage) noexcept
{
    const uint32_t itemId = acMessage.Packet.ItemId;

    // Two players picking it up at the same time: only the first one counts, the second one's copy was a duplicate
    if (!m_worldItems.erase(itemId))
    {
        spdlog::warn("Player {} picked up world item {}, which was already taken", acMessage.pPlayer->GetId(), itemId);
        return;
    }

    NotifyItemPickedUp notify;
    notify.ItemId = itemId;
    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);

    spdlog::info("Player {} picked up world item {}", acMessage.pPlayer->GetId(), itemId);
}

namespace
{
uint64_t ToKey(const GameId& acId) noexcept
{
    return (static_cast<uint64_t>(acId.ModId) << 32) | acId.BaseId;
}

GameId FromKey(uint64_t aKey) noexcept
{
    return GameId(static_cast<uint32_t>(aKey >> 32), static_cast<uint32_t>(aKey & 0xFFFFFFFF));
}
} // namespace

void ObjectService::OnPowerArmor(const PacketEvent<RequestPowerArmor>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    const uint32_t playerId = acMessage.pPlayer->GetId();
    const bool hasFrame = message.FrameId != GameId{};

    NotifyPowerArmor notify;
    notify.ServerId = message.ServerId;
    notify.PlayerId = playerId;
    notify.FrameId = message.FrameId;

    if (message.Type == RequestPowerArmor::kEnter)
    {
        if (hasFrame)
        {
            const auto it = m_powerArmorClaims.find(ToKey(message.FrameId));
            if (it != m_powerArmorClaims.end() && it->second != playerId)
            {
                notify.Type = NotifyPowerArmor::kDenied;
                notify.PlayerId = it->second;
                acMessage.pPlayer->Send(notify);
                spdlog::info("Player {} denied power armor {:X}:{:X}, it belongs to player {}", playerId, message.FrameId.ModId, message.FrameId.BaseId, it->second);
                return;
            }

            if (it == m_powerArmorClaims.end())
                spdlog::info("Player {} claimed power armor {:X}:{:X}", playerId, message.FrameId.ModId, message.FrameId.BaseId);

            m_powerArmorClaims[ToKey(message.FrameId)] = playerId;
        }

        notify.Type = NotifyPowerArmor::kEnter;
    }
    else
    {
        notify.Type = NotifyPowerArmor::kExit;
    }

    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
}

void ObjectService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    // Existing claims, so the new player can't use frames that are already someone's
    for (const auto& [key, ownerId] : m_powerArmorClaims)
    {
        NotifyPowerArmor notify;
        notify.Type = NotifyPowerArmor::kClaim;
        notify.PlayerId = ownerId;
        notify.FrameId = FromKey(key);
        acEvent.pPlayer->Send(notify);
    }

    // Items players dropped that are still lying around
    for (const auto& [itemId, item] : m_worldItems)
    {
        NotifyItemDropped notify = item;
        notify.LocalRefId = 0;
        acEvent.pPlayer->Send(notify);
    }
}

void ObjectService::OnDialogueLock(const PacketEvent<RequestDialogueLock>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    const uint32_t playerId = acMessage.pPlayer->GetId();

    const auto it = m_dialogueLocks.find(message.NpcServerId);
    const bool heldByOther = it != m_dialogueLocks.end() && it->second != playerId;

    NotifyDialogueLock notify;
    notify.NpcServerId = message.NpcServerId;

    if (message.Lock)
    {
        if (heldByOther)
        {
            // Lost a race: tell the requester who is talking so its client stops treating the NPC as its own
            notify.PlayerId = it->second;
            notify.Locked = true;
            acMessage.pPlayer->Send(notify);
            return;
        }

        m_dialogueLocks[message.NpcServerId] = playerId;
        notify.PlayerId = playerId;
        notify.Locked = true;

        m_world.GetCharacterService().GrantConversationOwnership(acMessage.pPlayer, message.NpcServerId);
    }
    else
    {
        if (it == m_dialogueLocks.end() || heldByOther)
            return;

        m_dialogueLocks.erase(it);
        notify.PlayerId = playerId;
        notify.Locked = false;
    }

    GameServer::Get()->SendToPlayers(notify);
    spdlog::info("Dialogue with npc {:X} {} by player {}", message.NpcServerId, notify.Locked ? "locked" : "released", playerId);
}

void ObjectService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    const uint32_t playerId = acEvent.pPlayer->GetId();

    for (auto it = m_dialogueLocks.begin(); it != m_dialogueLocks.end();)
    {
        if (it->second != playerId)
        {
            ++it;
            continue;
        }

        NotifyDialogueLock notify;
        notify.NpcServerId = it->first;
        notify.PlayerId = playerId;
        notify.Locked = false;
        GameServer::Get()->SendToPlayers(notify, acEvent.pPlayer);

        it = m_dialogueLocks.erase(it);
    }

    // A leaving player's frames become free again (PlayerId 0)
    for (auto it = m_powerArmorClaims.begin(); it != m_powerArmorClaims.end();)
    {
        if (it->second != playerId)
        {
            ++it;
            continue;
        }

        NotifyPowerArmor notify;
        notify.Type = NotifyPowerArmor::kClaim;
        notify.PlayerId = 0;
        notify.FrameId = FromKey(it->first);
        GameServer::Get()->SendToPlayers(notify, acEvent.pPlayer);

        it = m_powerArmorClaims.erase(it);
    }
}

void ObjectService::OnQuestItemTaken(const PacketEvent<RequestQuestItemTaken>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;

    // Only the players doing the item's quest together share it: everyone else's quest is their own and needs their copy
    const auto* pTeam = m_world.GetQuestService().GetTeam(message.QuestId, acMessage.pPlayer->GetId());
    if (!pTeam)
        return;

    NotifyQuestItemTaken notify;
    notify.SourceId = message.SourceId;
    notify.ItemId = message.ItemId;
    notify.Count = message.Count;
    notify.QuestId = message.QuestId;

    // Wherever they are, not just nearby: their copy of the container/body/world must lose it
    for (const uint32_t playerId : pTeam->Participants)
    {
        Player* pPlayer = m_world.GetPlayerManager().GetById(playerId);
        if (pPlayer && pPlayer != acMessage.pPlayer)
            pPlayer->Send(notify);
    }

    spdlog::info("Quest item {:X}:{:X} x{} for quest {:X}:{:X} taken from {:X}:{:X}, removed for the {} other participant(s)", message.ItemId.ModId, message.ItemId.BaseId, message.Count,
                 message.QuestId.ModId, message.QuestId.BaseId, message.SourceId.ModId, message.SourceId.BaseId, pTeam->Participants.size() - 1);
}

// TODO(cosideci): the cell handling of objects need to be revamped.
// We already store the location and worldspace of the mod through CellIdComponent.
// Clients need a message saying the entity was destroyed.
void ObjectService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept
{
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetCellComponent().Cell == acEvent.OldCell)
            return;
    }

    auto objectView = m_world.view<ObjectComponent, CellIdComponent>();
    Vector<entt::entity> toDestroy;

    for (auto entity : objectView)
    {
        const auto& cellIdComponent = objectView.get<CellIdComponent>(entity);

        if (cellIdComponent.Cell != acEvent.OldCell)
            continue;

        toDestroy.push_back(entity);
    }

    for (auto& entity : toDestroy)
    {
        m_world.destroy(entity);
    }
}

// NOTE: this whole system kinda relies on all objects in a cell being static.
// This is fine for containers and doors, but if this system is expanded, think of temporaries.
void ObjectService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();

    AssignObjectsResponse response;

    for (const ObjectData& object : acMessage.Packet.Objects)
    {
        const auto iter = std::find_if(
            std::begin(view), std::end(view),
            [view, id = object.Id](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                return formIdComponent.Id == id;
            });

        if (iter != std::end(view))
        {
            ObjectData objectData;
            objectData.ServerId = World::ToInteger(*iter);

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory = inventoryComponent.Content;

            objectData.IsSenderFirst = false;

            response.Objects.push_back(objectData);
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity, acMessage.pPlayer);
            objectComponent.CurrentLockData = object.CurrentLockData;

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, object.WorldSpaceId, object.CurrentCoords);
            auto& inventoryComp = m_world.emplace<InventoryComponent>(cEntity);
            inventoryComp.Content = object.CurrentInventory;

            ObjectData objectData;
            objectData.Id = object.Id;
            objectData.ServerId = World::ToInteger(cEntity);
            objectData.IsSenderFirst = true;

            response.Objects.push_back(objectData);
        }
    }

    if (!response.Objects.empty())
        acMessage.pPlayer->Send(response);
}

void ObjectService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;
    notifyActivate.PreActivationOpenState = acMessage.Packet.PreActivationOpenState;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
        {
            pPlayer->Send(notifyActivate);
        }
    }
}

void ObjectService::OnTerminalItemRun(const PacketEvent<RequestTerminalItemRun>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;

    NotifyTerminalItemRun notify;
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.TerminalRefId = message.TerminalRefId;
    notify.TerminalFormId = message.TerminalFormId;
    notify.MenuItemId = message.MenuItemId;

    // The players whose game has the terminal loaded: the ones near the player at it
    uint32_t sentTo = 0;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer || !pPlayer->GetCellComponent().IsInRange(acMessage.pPlayer->GetCellComponent(), false))
            continue;

        pPlayer->Send(notify);
        ++sentTo;
    }

    spdlog::info("{}: player {:X} ran item {} of terminal {:X}:{:X} on {:X}:{:X}, sent to {} player(s)", __FUNCTION__, notify.PlayerId, message.MenuItemId, message.TerminalFormId.ModId,
                 message.TerminalFormId.BaseId, message.TerminalRefId.ModId, message.TerminalRefId.BaseId, sentTo);
}

void ObjectService::OnDestructionStage(const PacketEvent<RequestDestructionStage>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;

    NotifyDestructionStage notify;
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.ObjectId = message.ObjectId;
    notify.Stage = message.Stage;
    notify.Health = message.Health;

    // The players whose game has the object loaded: the ones near the player whose game it happened in
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().IsInRange(acMessage.pPlayer->GetCellComponent(), false))
            pPlayer->Send(notify);
    }
}

void ObjectService::OnPipboyLight(const PacketEvent<RequestPipboyLight>& acMessage) const noexcept
{
    NotifyPipboyLight notify;
    notify.ServerId = acMessage.Packet.ServerId;
    notify.On = acMessage.Packet.On;

    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
}

void ObjectService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) const noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();

    const auto iter = std::find_if(
        std::begin(objectView), std::end(objectView),
        [objectView, id = acMessage.Packet.Id](auto entity)
        {
            const auto& formIdComponent = objectView.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentLockData.IsLocked = acMessage.Packet.IsLocked;
        objectComponent.CurrentLockData.LockLevel = acMessage.Packet.LockLevel;
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        if (pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
            pPlayer->Send(notifyLockChange);
    }
}

void ObjectService::OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>& acMessage) noexcept
{
    auto& packet = acMessage.Packet;

    NotifyScriptAnimation message{};
    message.FormID = packet.FormID;
    message.Animation = packet.Animation;
    message.EventName = packet.EventName;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        pPlayer->Send(message);
    }
}

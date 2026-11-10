#include <Services/CommandService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/SetTimeCommandRequest.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/RequestDifficulty.h>
#include <Messages/RequestMovementRules.h>
#include <Messages/TeleportCommandRequest.h>
#include <Messages/TeleportCommandResponse.h>

CommandService::CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_setTimeConnection = aDispatcher.sink<PacketEvent<SetTimeCommandRequest>>().connect<&CommandService::OnSetTimeCommand>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportCommandRequest>>().connect<&CommandService::OnTeleportCommandRequest>(this);
    m_difficultyConnection = aDispatcher.sink<PacketEvent<RequestDifficulty>>().connect<&CommandService::OnRequestDifficulty>(this);
    m_movementRulesConnection = aDispatcher.sink<PacketEvent<RequestMovementRules>>().connect<&CommandService::OnRequestMovementRules>(this);
}

void CommandService::OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept
{
    NotifySetTimeResult response{};

    // Only set time if player is an admin (or the host, see GameServer::HandleAuthenticationRequest). Checked by the
    // connection it came from: the packet's PlayerId is whatever the client put there.
    if (GameServer::Get()->GetAdminSessions().contains(acMessage.pPlayer->GetConnectionId()))
    {
        const auto cHours = static_cast<int>(acMessage.Packet.Hours);
        const auto cMinutes = static_cast<int>(acMessage.Packet.Minutes);

        m_world.GetCalendarService().SetTime(cHours, cMinutes, m_world.GetCalendarService().GetTimeScale());
        spdlog::info("Time set to {:02}:{:02} by {}", cHours, cMinutes, acMessage.pPlayer->GetUsername().c_str());

        response.Result = NotifySetTimeResult::SetTimeResult::kSuccess;
        acMessage.pPlayer->Send(response);

        return;
    }

    response.Result = NotifySetTimeResult::SetTimeResult::kNoPermission;
    acMessage.pPlayer->Send(response);
}

void CommandService::OnRequestDifficulty(const PacketEvent<RequestDifficulty>& acMessage) const noexcept
{
    // the host's only (an admin session: the hosted server's 127.0.0.1 connection, see HandleAuthenticationRequest)
    if (!GameServer::Get()->GetAdminSessions().contains(acMessage.pPlayer->GetConnectionId()))
        return;

    GameServer::Get()->SetDifficulty(acMessage.Packet.Difficulty);
}

void CommandService::OnRequestMovementRules(const PacketEvent<RequestMovementRules>& acMessage) const noexcept
{
    // the host's only, as the difficulty
    if (!GameServer::Get()->GetAdminSessions().contains(acMessage.pPlayer->GetConnectionId()))
        return;

    GameServer::Get()->SetMovementRules(acMessage.Packet.Rules);
}

void CommandService::OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept
{
    Player* pTargetPlayer = nullptr;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetUsername() == acMessage.Packet.TargetPlayer)
            pTargetPlayer = pPlayer;
    }

    TeleportCommandResponse response{};
    if (pTargetPlayer)
    {
        auto character = pTargetPlayer->GetCharacter();
        if (character)
        {
            const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
            if (pMovementComponent)
            {
                const auto& cellComponent = pTargetPlayer->GetCellComponent();
                response.CellId = cellComponent.Cell;
                response.Position = pMovementComponent->Position;
                response.WorldSpaceId = cellComponent.WorldSpaceId;
            }
        }
    }

    acMessage.pPlayer->Send(response);
}

#include <Services/WorkshopService.h>

#include <GameServer.h>
#include <World.h>

#include <Messages/NotifySettlementProduction.h>
#include <Messages/NotifySettlementRatings.h>
#include <Messages/RequestSettlementProduction.h>
#include <Messages/RequestSettlementRatings.h>

// The host-run settlement day (see WorkshopService.h): who runs it, the host's ratings for everyone, the day's
// production for a settlement's leader.

namespace
{
uint64_t DayKey(const GameId& acId) noexcept
{
    return (static_cast<uint64_t>(acId.ModId) << 32) | acId.BaseId;
}

bool IsHostSession(const Player& acPlayer) noexcept
{
    return GameServer::Get()->GetAdminSessions().count(acPlayer.GetConnectionId()) != 0;
}
} // namespace

void WorkshopService::OnSettlementRatings(const PacketEvent<RequestSettlementRatings>& acMessage) noexcept
{
    Player& player = *acMessage.pPlayer;
    if (!IsHostSession(player))
    {
        spdlog::warn("Player {} '{}' sent settlement ratings without being the host, ignored", player.GetId(), player.GetUsername().c_str());
        return;
    }

    const auto& message = acMessage.Packet;
    const bool newHost = m_dayHostId != player.GetId();
    if (newHost)
    {
        m_dayHostId = player.GetId();
        spdlog::info("Settlements: player {} '{}' runs the settlements' day", player.GetId(), player.GetUsername().c_str());
    }

    NotifySettlementRatings notify;
    notify.HostPlayerId = m_dayHostId;
    for (const SettlementRatings& settlement : message.Settlements)
    {
        if (settlement.WorkshopId == GameId{})
            continue;
        m_ratings[DayKey(settlement.WorkshopId)] = settlement;
        notify.Settlements.push_back(settlement);
    }

    // Everyone hears who runs the day when that changes; the ratings themselves only matter to the others
    if (notify.Settlements.empty() && !newHost)
        return;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != &player)
            pPlayer->Send(notify);
    }
}

void WorkshopService::OnSettlementProduction(const PacketEvent<RequestSettlementProduction>& acMessage) noexcept
{
    Player& player = *acMessage.pPlayer;
    const auto& message = acMessage.Packet;
    if (!IsHostSession(player) || player.GetId() != m_dayHostId)
    {
        spdlog::warn("Player {} '{}' sent settlement production without running the day, ignored", player.GetId(), player.GetUsername().c_str());
        return;
    }

    NotifySettlementProduction notify;
    notify.WorkshopId = message.WorkshopId;
    notify.Items = message.Items;

    // The leader, if they're still here; else back to the host's workbench, so nothing is lost
    Player* pLeader = nullptr;
    if (const auto it = m_claims.find(DayKey(message.WorkshopId)); it != m_claims.end())
    {
        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer->GetUsername() == it->second.LeaderName)
            {
                pLeader = pPlayer;
                break;
            }
        }
    }

    Player* pReceiver = pLeader && pLeader != &player ? pLeader : &player;
    pReceiver->Send(notify);
    spdlog::info("Settlements: the day's production at {:X}:{:X} ({} kinds of item) to player {} '{}'{}", message.WorkshopId.ModId, message.WorkshopId.BaseId,
                 message.Items.Entries.size(), pReceiver->GetId(), pReceiver->GetUsername().c_str(), pReceiver == &player ? " (back to the host, the leader left)" : "");
}

void WorkshopService::SendRatings(Player* apPlayer) const noexcept
{
    NotifySettlementRatings notify;
    notify.HostPlayerId = m_dayHostId;
    notify.Settlements.reserve(m_ratings.size());
    for (const auto& [key, settlement] : m_ratings)
        notify.Settlements.push_back(settlement);

    if (apPlayer)
    {
        apPlayer->Send(notify);
        return;
    }
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetId() != m_dayHostId)
            pPlayer->Send(notify);
    }
}

void WorkshopService::OnDayPlayerLeave(const Player& acPlayer) noexcept
{
    if (acPlayer.GetId() != m_dayHostId)
        return;

    // The host left: every game runs its own day again. Their ratings stay theirs (and go with their game).
    m_dayHostId = 0;
    m_ratings.clear();
    spdlog::info("Settlements: the host left, every game runs its own settlements' day");

    NotifySettlementRatings notify;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != &acPlayer)
            pPlayer->Send(notify);
    }
}

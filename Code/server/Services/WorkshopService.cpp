#include <Services/WorkshopService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/CharacterRemoveEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyCamps.h>
#include <Messages/NotifySettlementClaims.h>
#include <Messages/RequestCamp.h>
#include <Messages/RequestSettlementClaim.h>
#include <Messages/RequestSettlementProduction.h>
#include <Messages/RequestSettlementRatings.h>
#include <Messages/RequestWorkshopChange.h>

#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
// settlements.txt: a header line, "next <item id>", then one line per object (see ToLine)
constexpr char kDataFileName[] = "settlements.txt";
constexpr char kDataHeader[] = "# Reclamation Day settlements v1";
// Scrapped objects remembered for players who weren't there, the newest
constexpr size_t kMaxRemoved = 4000;
// settlement-claims.txt: tab-separated (names have spaces), "claim <workbench> <leader> <settlement>" then its
// "right <workbench> <player> <build 0/1> <scrap 0/1>" lines
constexpr char kClaimsFileName[] = "settlement-claims.txt";
constexpr char kClaimsHeader[] = "# Reclamation Day settlement claims v1";
constexpr size_t kMaxNameLength = 64;
// camps.txt: tab-separated, "next <camp id>", then "camp <id> <owner> <module item> <worldspace> <x> <y> <z> <heading>
// <stored 0/1>" and, for a stored one, "layout <camp id> <its object as a settlements.txt item line>" (its module item is
// then the layout's)
constexpr char kCampsFileName[] = "camps.txt";
constexpr char kCampsHeader[] = "# Reclamation Day C.A.M.P.s v1";

uint64_t ToKey(const GameId& acId) noexcept
{
    return (static_cast<uint64_t>(acId.ModId) << 32) | acId.BaseId;
}

std::string ToText(const GameId& acId) noexcept
{
    return fmt::format("{:X}:{:X}", acId.ModId, acId.BaseId);
}

bool FromText(const std::string& acText, GameId& aId) noexcept
{
    const auto colon = acText.find(':');
    if (colon == std::string::npos)
        return false;
    aId.ModId = std::strtoul(acText.substr(0, colon).c_str(), nullptr, 16);
    aId.BaseId = std::strtoul(acText.substr(colon + 1).c_str(), nullptr, 16);
    return true;
}

// <kind> type player workshop item ref base cell worldspace x y z rx ry rz scale actor end0 end1 endref0 endref1 thickness
std::string ToLine(const char* acpKind, const NotifyWorkshopChange& acChange) noexcept
{
    return fmt::format("{} {} {} {} {} {} {} {} {} {:.9g} {:.9g} {:.9g} {:.9g} {:.9g} {:.9g} {:.9g} {} {} {} {} {} {:.9g}\n", acpKind, acChange.ChangeType, acChange.PlayerId,
                       ToText(acChange.WorkshopId), acChange.ItemId, ToText(acChange.RefId), ToText(acChange.BaseId), ToText(acChange.CellId), ToText(acChange.WorldSpaceId),
                       acChange.Position[0], acChange.Position[1], acChange.Position[2], acChange.Rotation[0], acChange.Rotation[1], acChange.Rotation[2], acChange.Scale,
                       acChange.ActorServerId, acChange.EndItemIds[0], acChange.EndItemIds[1], ToText(acChange.EndRefIds[0]), ToText(acChange.EndRefIds[1]), acChange.WireThickness);
}

bool FromLine(std::istringstream& aLine, NotifyWorkshopChange& aChange) noexcept
{
    uint32_t type = 0;
    std::string workshop, ref, base, cell, worldSpace, endRef0, endRef1;
    aLine >> type >> aChange.PlayerId >> workshop >> aChange.ItemId >> ref >> base >> cell >> worldSpace >> aChange.Position[0] >> aChange.Position[1] >> aChange.Position[2] >>
        aChange.Rotation[0] >> aChange.Rotation[1] >> aChange.Rotation[2] >> aChange.Scale >> aChange.ActorServerId >> aChange.EndItemIds[0] >> aChange.EndItemIds[1] >> endRef0 >>
        endRef1 >> aChange.WireThickness;
    if (aLine.fail())
        return false;

    aChange.ChangeType = static_cast<uint8_t>(type);
    return FromText(workshop, aChange.WorkshopId) && FromText(ref, aChange.RefId) && FromText(base, aChange.BaseId) && FromText(cell, aChange.CellId) &&
           FromText(worldSpace, aChange.WorldSpaceId) && FromText(endRef0, aChange.EndRefIds[0]) && FromText(endRef1, aChange.EndRefIds[1]);
}

// Where this world's settlements are kept: the hosted world's folder, or data\ next to the server
std::filesystem::path GetDataFolder() noexcept
{
    if (const wchar_t* pFolder = _wgetenv(L"RD_WORLD_DATA"); pFolder && *pFolder)
        return std::filesystem::path(pFolder);
    return std::filesystem::current_path() / "data";
}

// A name as kept and shown: one line, no tabs (the claims file's separator), trimmed, not too long
String CleanName(const String& acName) noexcept
{
    String name = acName;
    for (char& c : name)
    {
        if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
    }
    const auto first = name.find_first_not_of(' ');
    if (first == String::npos)
        return {};
    name = name.substr(first, name.find_last_not_of(' ') - first + 1);
    if (name.size() > kMaxNameLength)
        name.resize(kMaxNameLength);
    return name;
}

Vector<std::string> SplitTabs(const std::string& acLine) noexcept
{
    Vector<std::string> fields;
    size_t start = 0;
    while (true)
    {
        const size_t tab = acLine.find('\t', start);
        fields.push_back(acLine.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            return fields;
        start = tab + 1;
    }
}

SettlementClaim::Right* FindRight(SettlementClaim& aClaim, const String& acName) noexcept
{
    const auto it = std::find_if(aClaim.Rights.begin(), aClaim.Rights.end(), [&acName](const SettlementClaim::Right& acRight) { return acRight.Name == acName; });
    return it == aClaim.Rights.end() ? nullptr : &*it;
}

bool IsAdmin(const Player& acPlayer) noexcept
{
    // The host is one (GameServer: it connects through 127.0.0.1 to the server its game started)
    return GameServer::Get()->GetAdminSessions().count(acPlayer.GetConnectionId()) != 0;
}

bool IsWire(const NotifyWorkshopChange& acItem) noexcept
{
    return acItem.EndItemIds[0] || acItem.EndItemIds[1] || acItem.EndRefIds[0] != GameId{} || acItem.EndRefIds[1] != GameId{};
}
} // namespace

WorkshopService::WorkshopService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_workshopChangeConnection = aDispatcher.sink<PacketEvent<RequestWorkshopChange>>().connect<&WorkshopService::OnWorkshopChange>(this);
    m_settlementClaimConnection = aDispatcher.sink<PacketEvent<RequestSettlementClaim>>().connect<&WorkshopService::OnSettlementClaim>(this);
    m_campConnection = aDispatcher.sink<PacketEvent<RequestCamp>>().connect<&WorkshopService::OnCamp>(this);
    m_settlementRatingsConnection = aDispatcher.sink<PacketEvent<RequestSettlementRatings>>().connect<&WorkshopService::OnSettlementRatings>(this);
    m_settlementProductionConnection = aDispatcher.sink<PacketEvent<RequestSettlementProduction>>().connect<&WorkshopService::OnSettlementProduction>(this);
    m_playerJoinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&WorkshopService::OnPlayerJoin>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&WorkshopService::OnPlayerLeave>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WorkshopService::OnUpdate>(this);

    Load();
    LoadClaims();
    LoadCamps();
}

void WorkshopService::OnWorkshopChange(const PacketEvent<RequestWorkshopChange>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;

    // The players' games refuse what they may not do; this is in case one didn't (it keeps the change to itself)
    if (const char* pRefusal = WhyNotAllowed(*acMessage.pPlayer, message.WorkshopId, message.ChangeType == RequestWorkshopChange::kRemoved))
    {
        spdlog::warn("Player {} '{}' changed workshop {:X}:{:X} (change {}), refused: {}", acMessage.pPlayer->GetId(), acMessage.pPlayer->GetUsername().c_str(), message.WorkshopId.ModId,
                     message.WorkshopId.BaseId, message.ChangeType, pRefusal);
        SendClaims(acMessage.pPlayer);
        return;
    }

    NotifyWorkshopChange notify;
    notify.ChangeType = message.ChangeType;
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.WorkshopId = message.WorkshopId;
    notify.ItemId = message.ItemId;
    notify.RefId = message.RefId;
    notify.BaseId = message.BaseId;
    notify.CellId = message.CellId;
    notify.WorldSpaceId = message.WorldSpaceId;
    notify.Position = message.Position;
    notify.Rotation = message.Rotation;
    notify.Scale = message.Scale;
    notify.LocalRefId = message.LocalRefId;
    notify.ActorServerId = message.ActorServerId;
    notify.EndItemIds = message.EndItemIds;
    notify.EndRefIds = message.EndRefIds;
    notify.WireThickness = message.WireThickness;

    m_dirty = true;

    switch (message.ChangeType)
    {
    case RequestWorkshopChange::kPlaced:
        notify.ItemId = m_nextItemId++;
        m_items[notify.ItemId] = notify;

        // Everyone, the builder included: it learns the item id of its own object
        GameServer::Get()->SendToPlayers(notify);

        spdlog::info("Player {} built {:X}:{:X} at workshop {:X}:{:X}, item {}", notify.PlayerId, notify.BaseId.ModId, notify.BaseId.BaseId, notify.WorkshopId.ModId, notify.WorkshopId.BaseId,
                     notify.ItemId);
        return;

    case RequestWorkshopChange::kMoved:
        if (notify.ItemId)
        {
            auto it = m_items.find(notify.ItemId);
            if (it != m_items.end())
            {
                NotifyWorkshopChange& item = it.value();
                item.Position = notify.Position;
                item.Rotation = notify.Rotation;
                item.Scale = notify.Scale;
                item.CellId = notify.CellId;
                item.WorldSpaceId = notify.WorldSpaceId;
            }
        }
        else
            m_refChanges[ToKey(notify.RefId)] = notify;
        break;

    case RequestWorkshopChange::kRemoved:
    {
        // What was built goes, remembered as scrapped for who still has it (in a game saved before)
        const auto forget = [this, &notify](uint32_t aItemId)
        {
            const auto it = m_items.find(aItemId);
            if (it == m_items.end())
                return;

            NotifyWorkshopChange removed = it->second;
            removed.ChangeType = RequestWorkshopChange::kRemoved;
            removed.PlayerId = notify.PlayerId;
            removed.LocalRefId = 0;
            m_items.erase(it);

            // Turrets are characters, theirs go with the character
            if (!removed.ActorServerId)
                m_removed[aItemId] = removed;
        };

        if (notify.ItemId)
            forget(notify.ItemId);
        else if (notify.RefId != GameId{})
            m_refChanges[ToKey(notify.RefId)] = notify;

        // Its wires go with it (the players apply that themselves): who joins later doesn't get wires to nothing
        Vector<uint32_t> wires;
        for (const auto& [itemId, item] : m_items)
        {
            for (size_t end = 0; end < item.EndItemIds.size(); ++end)
            {
                if ((notify.ItemId && item.EndItemIds[end] == notify.ItemId) || (!notify.ItemId && notify.RefId != GameId{} && item.EndRefIds[end] == notify.RefId))
                    wires.push_back(itemId);
            }
        }
        for (const uint32_t wire : wires)
            forget(wire);

        TrimRemoved();

        // A dead body or a turret: its character goes away for everyone, the players who didn't scrap it drop their copy
        if (notify.ActorServerId)
        {
            const auto entity = static_cast<entt::entity>(notify.ActorServerId);
            if (m_world.valid(entity) && m_world.all_of<CharacterComponent, OwnerComponent>(entity))
            {
                m_world.GetDispatcher().trigger(CharacterRemoveEvent(notify.ActorServerId));
                spdlog::info("Player {} scrapped character {:X} (a body or a turret)", notify.PlayerId, notify.ActorServerId);
            }
        }
        break;
    }

    case RequestWorkshopChange::kRestored:
        // The world's own object, its scrap undone: kept as its latest change, so whoever has it scrapped in their
        // game (saved after the scrap, or joining later) brings it back too
        if (notify.RefId == GameId{})
        {
            spdlog::warn("Player {} sent a restore without the object", notify.PlayerId);
            return;
        }
        m_refChanges[ToKey(notify.RefId)] = notify;
        break;

    default:
        spdlog::warn("Player {} sent an unknown workshop change {}", notify.PlayerId, message.ChangeType);
        return;
    }

    notify.LocalRefId = 0;
    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);

    const char* pChange = notify.ChangeType == RequestWorkshopChange::kMoved ? "moved" : notify.ChangeType == RequestWorkshopChange::kRestored ? "restored" : "removed";
    spdlog::info("Player {} {} item {} / {:X}:{:X} at workshop {:X}:{:X}", notify.PlayerId, pChange, notify.ItemId, notify.RefId.ModId, notify.RefId.BaseId, notify.WorkshopId.ModId,
                 notify.WorkshopId.BaseId);
}

void WorkshopService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    // The C.A.M.P.s first: their objects are built at their modules
    if (!m_camps.empty())
        SendCamps(acEvent.pPlayer);

    // The settlements as they are now: what was built, what was moved or scrapped of what was already there, and what
    // was scrapped of what was built (their game may have it, saved before)
    for (const auto& [itemId, item] : m_items)
    {
        NotifyWorkshopChange notify = item;
        notify.ChangeType = RequestWorkshopChange::kPlaced;
        notify.LocalRefId = 0;
        acEvent.pPlayer->Send(notify);
    }

    for (const auto& [key, change] : m_refChanges)
        acEvent.pPlayer->Send(change);

    for (const auto& [itemId, removed] : m_removed)
        acEvent.pPlayer->Send(removed);

    if (!m_items.empty() || !m_refChanges.empty() || !m_removed.empty())
        spdlog::info("Sent player {} {} settlement objects, {} changes to existing ones and {} scrapped since", acEvent.pPlayer->GetId(), m_items.size(), m_refChanges.size(),
                     m_removed.size());

    // The claims to them, and to everyone if they lead one (now connected)
    const String& name = acEvent.pPlayer->GetUsername();
    const bool isLeader = std::any_of(m_claims.begin(), m_claims.end(), [&name](const auto& acEntry) { return acEntry.second.LeaderName == name; });
    SendClaims(isLeader ? nullptr : acEvent.pPlayer);

    // Whether a host runs the settlements' day, and their ratings
    if (m_dayHostId)
        SendRatings(acEvent.pPlayer);
}

void WorkshopService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    const String& name = acEvent.pPlayer->GetUsername();
    if (std::any_of(m_claims.begin(), m_claims.end(), [&name](const auto& acEntry) { return acEntry.second.LeaderName == name; }))
        SendClaims(nullptr, acEvent.pPlayer);

    OnDayPlayerLeave(*acEvent.pPlayer);
}

void WorkshopService::OnSettlementClaim(const PacketEvent<RequestSettlementClaim>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    Player& player = *acMessage.pPlayer;
    const String& name = player.GetUsername();

    if (message.WorkshopId == GameId{})
    {
        spdlog::warn("Player {} '{}' sent a settlement claim without a workbench", player.GetId(), name.c_str());
        return;
    }

    const uint64_t key = ToKey(message.WorkshopId);
    auto it = m_claims.find(key);

    switch (message.Action)
    {
    case RequestSettlementClaim::kClaim:
    {
        // Two at once: the first one's, the other learns it from the claims sent back
        if (it != m_claims.end())
        {
            spdlog::info("Player {} '{}' tried to claim {} ({:X}:{:X}), {}'s already", player.GetId(), name.c_str(), it->second.Name.c_str(), message.WorkshopId.ModId,
                         message.WorkshopId.BaseId, it->second.LeaderName.c_str());
            SendClaims(acMessage.pPlayer);
            return;
        }

        SettlementClaim claim;
        claim.WorkshopId = message.WorkshopId;
        claim.Name = CleanName(message.SettlementName);
        if (claim.Name.empty())
            claim.Name = "Settlement";
        claim.LeaderName = name;
        m_claims[key] = std::move(claim);
        spdlog::info("Player {} '{}' claimed {} ({:X}:{:X})", player.GetId(), name.c_str(), m_claims[key].Name.c_str(), message.WorkshopId.ModId, message.WorkshopId.BaseId);
        break;
    }

    case RequestSettlementClaim::kAbandon:
    {
        if (it == m_claims.end())
        {
            SendClaims(acMessage.pPlayer);
            return;
        }

        // A C.A.M.P. isn't abandoned (its owner moves it); the host releasing one removes it with all built there
        if (Camp::IsCamp(message.WorkshopId))
        {
            if (!IsAdmin(player))
            {
                spdlog::warn("Player {} '{}' tried to abandon C.A.M.P. {}", player.GetId(), name.c_str(), message.WorkshopId.BaseId);
                SendClaims(acMessage.pPlayer);
                return;
            }
            RemoveCamp(message.WorkshopId.BaseId, player.GetId());
            return;
        }

        const bool isLeader = it->second.LeaderName == name;
        if (!isLeader && !IsAdmin(player))
        {
            spdlog::warn("Player {} '{}' tried to abandon {}, {}'s", player.GetId(), name.c_str(), it->second.Name.c_str(), it->second.LeaderName.c_str());
            SendClaims(acMessage.pPlayer);
            return;
        }

        spdlog::info("Player {} '{}' {} {} ({:X}:{:X}){}", player.GetId(), name.c_str(), isLeader ? "abandoned" : "released", it->second.Name.c_str(), message.WorkshopId.ModId,
                     message.WorkshopId.BaseId, isLeader ? "" : fmt::format(", {}'s", it->second.LeaderName.c_str()));
        m_claims.erase(it);
        break;
    }

    case RequestSettlementClaim::kSetRights:
    {
        if (it == m_claims.end() || it->second.LeaderName != name)
        {
            spdlog::warn("Player {} '{}' tried to set rights at {:X}:{:X} without leading it", player.GetId(), name.c_str(), message.WorkshopId.ModId, message.WorkshopId.BaseId);
            SendClaims(acMessage.pPlayer);
            return;
        }

        SettlementClaim& claim = it.value();
        const String target = CleanName(message.PlayerName);
        if (target.empty() || target == claim.LeaderName)
            return;

        // Given to the players of the leader's party; changed or taken back from whoever has some, wherever they are
        SettlementClaim::Right* pRight = FindRight(claim, target);
        if (!pRight)
        {
            const auto& partyId = player.GetParty().JoinedPartyId;
            bool inParty = false;
            for (Player* pOther : m_world.GetPlayerManager())
            {
                if (partyId && pOther->GetUsername() == target && pOther->GetParty().JoinedPartyId == partyId)
                    inParty = true;
            }
            if (!inParty)
            {
                spdlog::warn("Player {} '{}' tried to give {} rights at {}, not in their party", player.GetId(), name.c_str(), target.c_str(), claim.Name.c_str());
                SendClaims(acMessage.pPlayer);
                return;
            }
        }

        if (!message.Build && !message.Scrap)
        {
            if (pRight)
                claim.Rights.erase(claim.Rights.begin() + (pRight - claim.Rights.data()));
        }
        else if (pRight)
        {
            pRight->Build = message.Build;
            pRight->Scrap = message.Scrap;
        }
        else
            claim.Rights.push_back({target, message.Build, message.Scrap});

        spdlog::info("Player {} '{}' set {}'s rights at {}: build {}, scrap {}", player.GetId(), name.c_str(), target.c_str(), claim.Name.c_str(), message.Build, message.Scrap);
        break;
    }

    default:
        spdlog::warn("Player {} '{}' sent an unknown settlement claim action {}", player.GetId(), name.c_str(), message.Action);
        return;
    }

    SaveClaims();
    SendClaims();
}

const char* WorkshopService::WhyNotAllowed(const Player& acPlayer, const GameId& acWorkshopId, bool aScrap) const noexcept
{
    // Workbenches without an id the others know (placed while playing) can't be claimed: as before claims
    if (acWorkshopId == GameId{})
        return nullptr;

    const auto it = m_claims.find(ToKey(acWorkshopId));
    if (it == m_claims.end())
        return "nobody claimed it";

    const SettlementClaim& claim = it->second;
    const String& name = acPlayer.GetUsername();
    if (claim.LeaderName == name)
        return nullptr;

    const auto right = std::find_if(claim.Rights.begin(), claim.Rights.end(), [&name](const SettlementClaim::Right& acRight) { return acRight.Name == name; });
    if (right == claim.Rights.end())
        return "no rights there";
    if (aScrap ? !right->Scrap : !right->Build)
        return aScrap ? "no scrap right" : "no build right";
    return nullptr;
}

void WorkshopService::SendClaims(Player* apPlayer, const Player* apLeaving) const noexcept
{
    NotifySettlementClaims notify;
    notify.Claims.reserve(m_claims.size());
    for (const auto& [key, claim] : m_claims)
    {
        SettlementClaim sent = claim;
        sent.LeaderPlayerId = 0;
        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer != apLeaving && pPlayer->GetUsername() == claim.LeaderName)
            {
                sent.LeaderPlayerId = pPlayer->GetId();
                break;
            }
        }
        notify.Claims.push_back(std::move(sent));
    }

    // Each with their own name and whether they may release anyone's
    const auto send = [&notify](Player* apReceiver)
    {
        notify.LocalName = apReceiver->GetUsername();
        notify.IsAdmin = IsAdmin(*apReceiver);
        apReceiver->Send(notify);
    };

    if (apPlayer)
        send(apPlayer);
    else
    {
        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer != apLeaving)
                send(pPlayer);
        }
    }
}

void WorkshopService::LoadClaims() noexcept
{
    m_claimsFile = GetDataFolder() / kClaimsFileName;

    std::ifstream file(m_claimsFile);
    if (!file)
        return;

    std::string text;
    size_t lineNumber = 0;
    size_t bad = 0;
    size_t rights = 0;
    while (std::getline(file, text))
    {
        ++lineNumber;
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        if (text.empty() || text[0] == '#')
            continue;

        const auto fields = SplitTabs(text);
        GameId workshopId{};
        if (fields.size() < 4 || !FromText(fields[1], workshopId) || workshopId == GameId{})
        {
            ++bad;
            spdlog::warn("{} line {} isn't readable, skipped: {}", kClaimsFileName, lineNumber, text);
            continue;
        }

        const uint64_t key = ToKey(workshopId);
        if (fields[0] == "claim" && !fields[2].empty())
        {
            SettlementClaim& claim = m_claims[key];
            claim.WorkshopId = workshopId;
            claim.LeaderName = CleanName(String(fields[2].c_str()));
            claim.Name = CleanName(String(fields[3].c_str()));
        }
        else if (fields[0] == "right" && fields.size() >= 5 && m_claims.count(key))
        {
            m_claims[key].Rights.push_back({CleanName(String(fields[2].c_str())), fields[3] == "1", fields[4] == "1"});
            ++rights;
        }
        else
        {
            ++bad;
            spdlog::warn("{} line {} isn't readable, skipped: {}", kClaimsFileName, lineNumber, text);
        }
    }

    spdlog::info("Loaded {} claimed settlements ({} rights given) from {}{}", m_claims.size(), rights, m_claimsFile.string(), bad ? fmt::format(" ({} lines skipped)", bad) : "");
}

void WorkshopService::SaveClaims() const noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(m_claimsFile.parent_path(), ec);

    // Written aside, then put in place, as settlements.txt
    auto temporary = m_claimsFile;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file)
        {
            spdlog::error("Couldn't write {}, settlement claims aren't kept", temporary.string());
            return;
        }

        file << kClaimsHeader << '\n';
        file << "# claim<TAB>workbench<TAB>leader<TAB>settlement, then right<TAB>workbench<TAB>player<TAB>build<TAB>scrap\n";
        for (const auto& [key, claim] : m_claims)
        {
            const std::string workshop = ToText(claim.WorkshopId);
            file << "claim\t" << workshop << '\t' << claim.LeaderName.c_str() << '\t' << claim.Name.c_str() << '\n';
            for (const SettlementClaim::Right& right : claim.Rights)
                file << "right\t" << workshop << '\t' << right.Name.c_str() << '\t' << (right.Build ? 1 : 0) << '\t' << (right.Scrap ? 1 : 0) << '\n';
        }

        if (!file.flush())
        {
            spdlog::error("Couldn't write {}, settlement claims aren't kept", temporary.string());
            return;
        }
    }

    std::filesystem::rename(temporary, m_claimsFile, ec);
    if (ec)
        spdlog::error("Couldn't replace {}: {}", m_claimsFile.string(), ec.message());
}

void WorkshopService::OnUpdate(const UpdateEvent&) noexcept
{
    // A moment after a change, at most every 2 s: changes come in bursts (moving with the build tools)
    if (!m_dirty)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - m_savedAt < std::chrono::seconds(2))
        return;

    m_savedAt = now;
    m_dirty = false;
    Save();
}

void WorkshopService::Load() noexcept
{
    const auto folder = GetDataFolder();
    m_dataFile = folder / kDataFileName;

    std::ifstream file(m_dataFile);
    if (!file)
    {
        spdlog::info("No settlements saved for this world yet ({})", m_dataFile.string());
        return;
    }

    std::string text;
    size_t lineNumber = 0;
    size_t bad = 0;
    while (std::getline(file, text))
    {
        ++lineNumber;
        if (text.empty() || text[0] == '#')
            continue;

        std::istringstream line(text);
        std::string kind;
        line >> kind;

        if (kind == "next")
        {
            uint32_t next = 0;
            line >> next;
            m_nextItemId = std::max(m_nextItemId, next);
            continue;
        }

        NotifyWorkshopChange change{};
        if (!FromLine(line, change))
        {
            ++bad;
            spdlog::warn("settlements.txt line {} isn't readable, skipped: {}", lineNumber, text);
            continue;
        }

        if (kind == "item" && change.ItemId)
        {
            m_items[change.ItemId] = change;
            m_nextItemId = std::max(m_nextItemId, change.ItemId + 1);
        }
        else if (kind == "ref" && change.RefId != GameId{})
            m_refChanges[ToKey(change.RefId)] = change;
        else if (kind == "removed" && change.ItemId)
        {
            m_removed[change.ItemId] = change;
            m_nextItemId = std::max(m_nextItemId, change.ItemId + 1);
        }
        else
            ++bad;
    }

    spdlog::info("Loaded the settlements of this world from {}: {} objects built, {} changes to existing ones, {} scrapped{}", m_dataFile.string(), m_items.size(), m_refChanges.size(),
                 m_removed.size(), bad ? fmt::format(" ({} lines skipped)", bad) : "");
}

void WorkshopService::Save() noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(m_dataFile.parent_path(), ec);

    // Written aside, then put in place: the server can be ended at any moment
    auto temporary = m_dataFile;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file)
        {
            spdlog::error("Couldn't write {}, settlement changes aren't kept", temporary.string());
            return;
        }

        file << kDataHeader << '\n';
        file << "# type player workshop item ref base cell worldspace x y z rx ry rz scale actor wire_end0 wire_end1 wire_endref0 wire_endref1 wire_thickness\n";
        file << "next " << m_nextItemId << '\n';

        // Turrets aren't kept: they're characters, saved in their owner's game and synced again from there
        for (const auto& [itemId, item] : m_items)
        {
            if (!item.ActorServerId)
                file << ToLine("item", item);
        }
        for (const auto& [key, change] : m_refChanges)
            file << ToLine("ref", change);
        for (const auto& [itemId, removed] : m_removed)
            file << ToLine("removed", removed);

        if (!file.flush())
        {
            spdlog::error("Couldn't write {}, settlement changes aren't kept", temporary.string());
            return;
        }
    }

    std::filesystem::rename(temporary, m_dataFile, ec);
    if (ec)
        spdlog::error("Couldn't replace {}: {}", m_dataFile.string(), ec.message());
}

void WorkshopService::OnCamp(const PacketEvent<RequestCamp>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    Player& player = *acMessage.pPlayer;
    const String& name = player.GetUsername();

    auto mine = std::find_if(m_camps.begin(), m_camps.end(), [&name](const auto& acEntry) { return acEntry.second.OwnerName == name; });

    if (message.Action == RequestCamp::kRemove)
    {
        if (!IsAdmin(player))
        {
            spdlog::warn("Player {} '{}' tried to remove C.A.M.P. {} without being the host", player.GetId(), name.c_str(), message.CampId);
            return;
        }
        RemoveCamp(message.CampId, player.GetId());
        return;
    }

    if (message.Action == RequestCamp::kPickUp)
    {
        if (mine == m_camps.end() || mine->second.Stored)
        {
            spdlog::warn("Player {} '{}' tried to pick up a C.A.M.P.: {}", player.GetId(), name.c_str(), mine == m_camps.end() ? "they have none" : "it's stored already");
            SendCamps(acMessage.pPlayer);
            return;
        }
        PickUpCamp(mine.value(), player.GetId());
        return;
    }

    if (message.Action != RequestCamp::kPlace && message.Action != RequestCamp::kMove)
    {
        spdlog::warn("Player {} '{}' sent an unknown C.A.M.P. action {}", player.GetId(), name.c_str(), message.Action);
        return;
    }

    // Placing with one already is moving it (its game may not have heard of it yet)
    const bool move = mine != m_camps.end();
    if (message.WorldSpaceId == GameId{} || (message.Action == RequestCamp::kMove && !move))
    {
        spdlog::warn("Player {} '{}' sent a C.A.M.P. {} that can't be: {}", player.GetId(), name.c_str(), move ? "move" : "placement",
                     message.WorldSpaceId == GameId{} ? "inside" : "they have none");
        SendCamps(acMessage.pPlayer);
        return;
    }

    if (const char* pRefusal = WhyNoCampHere(message.WorldSpaceId, message.Position, move ? mine->first : 0))
    {
        spdlog::info("Player {} '{}' can't set a C.A.M.P. down at ({:.0f}, {:.0f}): {}", player.GetId(), name.c_str(), message.Position[0], message.Position[1], pRefusal);
        SendCamps(acMessage.pPlayer);
        return;
    }

    if (!move)
    {
        if (message.ModuleBaseId == GameId{})
        {
            spdlog::warn("Player {} '{}' sent a C.A.M.P. without its module (is RD_Camps.esp loaded?)", player.GetId(), name.c_str());
            return;
        }

        Camp camp;
        camp.Id = m_nextCampId++;
        camp.OwnerName = name;
        camp.WorldSpaceId = message.WorldSpaceId;
        camp.Position = message.Position;
        camp.Heading = message.Heading;

        // Its module: a settlement object of the C.A.M.P. itself
        NotifyWorkshopChange module{};
        module.ChangeType = RequestWorkshopChange::kPlaced;
        module.PlayerId = player.GetId();
        module.WorkshopId = camp.GetWorkshopId();
        module.BaseId = message.ModuleBaseId;
        module.WorldSpaceId = message.WorldSpaceId;
        module.Position = message.Position;
        module.Rotation = {0.f, 0.f, message.Heading};
        camp.ModuleItemId = AddItem(module);
        m_camps[camp.Id] = camp;

        // Its owner leads it
        SettlementClaim claim;
        claim.WorkshopId = camp.GetWorkshopId();
        claim.Name = CleanName(name + "'s C.A.M.P.");
        claim.LeaderName = name;
        m_claims[ToKey(claim.WorkshopId)] = claim;

        SaveCamps();
        SaveClaims();
        SendCamps();
        GameServer::Get()->SendToPlayers(m_items[camp.ModuleItemId]);
        SendClaims();

        spdlog::info("Player {} '{}' set their C.A.M.P. ({}) down at ({:.0f}, {:.0f}, {:.0f}) in {:X}:{:X}, module item {}", player.GetId(), name.c_str(), camp.Id,
                     camp.Position[0], camp.Position[1], camp.Position[2], camp.WorldSpaceId.ModId, camp.WorldSpaceId.BaseId, camp.ModuleItemId);
        return;
    }

    Camp& camp = mine.value();
    const float turn = message.Heading - camp.Heading;
    const char* pDone = "moved";
    Vector<NotifyWorkshopChange> objects;
    uint32_t moduleItemId = camp.ModuleItemId;
    if (camp.Stored)
    {
        // Set down again: its layout is built at the new spot, turned and shifted as the module is (free)
        const auto layout = m_layouts.find(camp.Id);
        if (layout != m_layouts.end())
        {
            objects = layout->second.Objects;
            moduleItemId = layout->second.ModuleItemId;
        }
        if (std::none_of(objects.begin(), objects.end(), [moduleItemId](const NotifyWorkshopChange& acItem) { return acItem.ItemId == moduleItemId; }))
        {
            // Nothing kept of its module (it can't be, but a lost layout shouldn't lose the C.A.M.P.): a new one
            if (message.ModuleBaseId == GameId{})
            {
                spdlog::warn("Player {} '{}' sent a C.A.M.P. without its module (is RD_Camps.esp loaded?)", player.GetId(), name.c_str());
                return;
            }
            NotifyWorkshopChange module{};
            module.ChangeType = RequestWorkshopChange::kPlaced;
            module.WorkshopId = camp.GetWorkshopId();
            module.BaseId = message.ModuleBaseId;
            module.WorldSpaceId = camp.WorldSpaceId;
            module.Position = camp.Position;
            module.Rotation = {0.f, 0.f, camp.Heading};
            module.ItemId = moduleItemId = 0xFFFFFFFF;
            objects.insert(objects.begin(), module);
        }
        m_layouts.erase(camp.Id);
        camp.Stored = false;
        pDone = "set their stored";
    }
    else
    {
        // Moving: everything built at it goes (wires before what they connect, the module last) and is built again.
        // Turrets (characters, run by the game of whoever built them) aren't removed: their item is let go here and a
        // new one at the new spot moves them there (RebuildCamp)
        objects = GetCampObjects(camp);
        for (auto it = objects.rbegin(); it != objects.rend(); ++it)
        {
            if (it->ActorServerId)
            {
                m_items.erase(it->ItemId);
                m_dirty = true;
            }
            else
                RemoveItem(it->ItemId, player.GetId());
        }
        TrimRemoved();
    }

    // The module built again with its base as the owner's game numbers it now: plugins are numbered anew each time the
    // server starts, so the base a C.A.M.P. was saved with may be another plugin's by now (2026-10-27: a C.A.M.P. from
    // 10-22, moved after plugins were added to the load order, came back as nothing)
    if (message.ModuleBaseId != GameId{})
    {
        for (NotifyWorkshopChange& object : objects)
        {
            if (object.ItemId == moduleItemId)
                object.BaseId = message.ModuleBaseId;
        }
    }

    const auto [placed, turrets] = RebuildCamp(camp, objects, moduleItemId, message.WorldSpaceId, message.Position, message.Heading, player.GetId());

    SaveCamps();
    SendCamps();
    for (const uint32_t itemId : placed)
        GameServer::Get()->SendToPlayers(m_items[itemId]);

    const auto moved = std::count_if(placed.begin(), placed.end(), [this](uint32_t aItemId) { return m_items[aItemId].ActorServerId != 0; });
    spdlog::info("Player {} '{}' {} C.A.M.P. ({}) to ({:.0f}, {:.0f}, {:.0f}), turned {:.1f} degrees: {} objects built again, {} turrets moved, {} left out", player.GetId(),
                 name.c_str(), pDone, camp.Id, camp.Position[0], camp.Position[1], camp.Position[2], turn * 57.29578f, placed.size() - moved, moved, turrets);
}

Vector<NotifyWorkshopChange> WorkshopService::GetCampObjects(const Camp& acCamp) const noexcept
{
    const GameId workshopId = acCamp.GetWorkshopId();
    Vector<NotifyWorkshopChange> objects;
    for (const auto& [itemId, item] : m_items)
    {
        if (item.WorkshopId == workshopId)
            objects.push_back(item);
    }
    const uint32_t moduleItemId = acCamp.ModuleItemId;
    std::stable_sort(objects.begin(), objects.end(),
                     [moduleItemId](const NotifyWorkshopChange& acLeft, const NotifyWorkshopChange& acRight)
                     {
                         const auto rank = [moduleItemId](const NotifyWorkshopChange& acItem) { return acItem.ItemId == moduleItemId ? 0 : IsWire(acItem) ? 2 : 1; };
                         return rank(acLeft) < rank(acRight);
                     });
    return objects;
}

std::pair<Vector<uint32_t>, size_t> WorkshopService::RebuildCamp(Camp& aCamp, const Vector<NotifyWorkshopChange>& acObjects, uint32_t aModuleItemId,
                                                                 const GameId& acWorldSpaceId, const std::array<float, 3>& acPosition, float aHeading,
                                                                 uint32_t aPlayerId) noexcept
{
    const std::array<float, 3> from = aCamp.Position;
    const float turn = aHeading - aCamp.Heading;
    const float cosTurn = std::cos(turn);
    const float sinTurn = std::sin(turn);

    aCamp.WorldSpaceId = acWorldSpaceId;
    aCamp.Position = acPosition;
    aCamp.Heading = aHeading;

    TiltedPhoques::Map<uint32_t, uint32_t> newIds;
    Vector<uint32_t> placed;
    size_t turrets = 0;
    for (const NotifyWorkshopChange& object : acObjects)
    {
        // A turret (a character) is moved by its game, the one running it, when its item comes at the new spot; one
        // whose character is gone (its game left) is left out
        if (object.ActorServerId)
        {
            const auto entity = static_cast<entt::entity>(object.ActorServerId);
            if (!m_world.valid(entity) || !m_world.all_of<CharacterComponent, OwnerComponent>(entity))
            {
                ++turrets;
                continue;
            }
        }

        NotifyWorkshopChange item = object;
        item.ChangeType = RequestWorkshopChange::kPlaced;
        item.PlayerId = aPlayerId;
        item.LocalRefId = 0;
        item.WorldSpaceId = acWorldSpaceId;
        item.CellId = {}; // outside, the spot decides the cell

        // Heading is clockwise from +Y (forward is (sin, cos)): turning by t takes (x, y) to (x cos t + y sin t, y cos t - x sin t)
        const float x = object.Position[0] - from[0];
        const float y = object.Position[1] - from[1];
        item.Position = {acPosition[0] + x * cosTurn + y * sinTurn, acPosition[1] - x * sinTurn + y * cosTurn, acPosition[2] + object.Position[2] - from[2]};
        item.Rotation[2] = std::remainder(object.Rotation[2] + turn, 6.2831853f);

        if (IsWire(object))
        {
            // Between objects of the C.A.M.P.: their new ids; a wire to anything else stays behind
            bool ends = true;
            for (size_t end = 0; end < 2; ++end)
            {
                const auto newEnd = newIds.find(object.EndItemIds[end]);
                if (!object.EndItemIds[end] || newEnd == newIds.end())
                    ends = false;
                else
                    item.EndItemIds[end] = newEnd->second;
            }
            if (!ends)
                continue;
        }

        const uint32_t newId = AddItem(item);
        newIds[object.ItemId] = newId;
        placed.push_back(newId);
        if (object.ItemId == aModuleItemId)
            aCamp.ModuleItemId = newId;
    }
    return {std::move(placed), turrets};
}

void WorkshopService::PickUpCamp(Camp& aCamp, uint32_t aPlayerId) noexcept
{
    // Everything built at it goes, as for a move: wires before what they connect, the module last
    Vector<NotifyWorkshopChange> objects = GetCampObjects(aCamp);
    for (auto it = objects.rbegin(); it != objects.rend(); ++it)
        RemoveItem(it->ItemId, aPlayerId);
    TrimRemoved();

    // Kept as it stood; turrets are characters (their game runs them), they can't be kept
    const auto turrets = std::count_if(objects.begin(), objects.end(), [](const NotifyWorkshopChange& acItem) { return acItem.ActorServerId != 0; });
    objects.erase(std::remove_if(objects.begin(), objects.end(), [](const NotifyWorkshopChange& acItem) { return acItem.ActorServerId != 0; }), objects.end());
    CampLayout& layout = m_layouts[aCamp.Id];
    layout.ModuleItemId = aCamp.ModuleItemId;
    layout.Objects = std::move(objects);
    aCamp.ModuleItemId = 0;
    aCamp.Stored = true;

    SaveCamps();
    SendCamps();

    spdlog::info("Player {} picked up {}'s C.A.M.P. ({}): {} objects kept as its layout, {} turrets left out", aPlayerId, aCamp.OwnerName.c_str(), aCamp.Id,
                 layout.Objects.size(), turrets);
}

const char* WorkshopService::WhyNoCampHere(const GameId& acWorldSpaceId, const std::array<float, 3>& acPosition, uint32_t aIgnoredCampId) const noexcept
{
    for (const auto& [campId, camp] : m_camps)
    {
        // A stored one takes no room (Fallout 76: only a C.A.M.P. that stands keeps others away)
        if (campId == aIgnoredCampId || camp.Stored || camp.WorldSpaceId != acWorldSpaceId)
            continue;

        const float dx = camp.Position[0] - acPosition[0];
        const float dy = camp.Position[1] - acPosition[1];
        if (dx * dx + dy * dy < 4.f * Camp::kRadius * Camp::kRadius)
            return "too close to another C.A.M.P.";
    }
    return nullptr;
}

uint32_t WorkshopService::AddItem(NotifyWorkshopChange aItem) noexcept
{
    aItem.ItemId = m_nextItemId++;
    m_items[aItem.ItemId] = aItem;
    m_dirty = true;
    return aItem.ItemId;
}

void WorkshopService::RemoveItem(uint32_t aItemId, uint32_t aPlayerId) noexcept
{
    const auto it = m_items.find(aItemId);
    if (it == m_items.end())
        return;

    NotifyWorkshopChange removed = it->second;
    removed.ChangeType = RequestWorkshopChange::kRemoved;
    removed.PlayerId = aPlayerId;
    removed.LocalRefId = 0;
    m_items.erase(it);
    m_dirty = true;

    // Everyone, the owner too: nobody scrapped it in their game
    GameServer::Get()->SendToPlayers(removed);

    // Turrets are characters, theirs go with the character
    if (removed.ActorServerId)
    {
        const auto entity = static_cast<entt::entity>(removed.ActorServerId);
        if (m_world.valid(entity) && m_world.all_of<CharacterComponent, OwnerComponent>(entity))
            m_world.GetDispatcher().trigger(CharacterRemoveEvent(removed.ActorServerId));
        return;
    }
    m_removed[aItemId] = removed;
}

void WorkshopService::TrimRemoved() noexcept
{
    // The oldest scrapped objects are forgotten
    while (m_removed.size() > kMaxRemoved)
    {
        auto oldest = m_removed.begin();
        for (auto it = m_removed.begin(); it != m_removed.end(); ++it)
        {
            if (it->first < oldest->first)
                oldest = it;
        }
        m_removed.erase(oldest);
    }
}

void WorkshopService::RemoveCamp(uint32_t aCampId, uint32_t aPlayerId) noexcept
{
    const auto camp = m_camps.find(aCampId);
    if (camp == m_camps.end())
        return;

    const GameId workshopId = camp->second.GetWorkshopId();
    const String owner = camp->second.OwnerName;

    // Its objects, its module last
    Vector<uint32_t> items;
    for (const auto& [itemId, item] : m_items)
    {
        if (item.WorkshopId == workshopId && itemId != camp->second.ModuleItemId)
            items.push_back(itemId);
    }
    items.push_back(camp->second.ModuleItemId);
    for (const uint32_t itemId : items)
        RemoveItem(itemId, aPlayerId);
    TrimRemoved();

    m_claims.erase(ToKey(workshopId));
    m_layouts.erase(aCampId);
    m_camps.erase(camp);

    SaveCamps();
    SaveClaims();
    SendCamps();
    SendClaims();

    spdlog::info("Player {} removed {}'s C.A.M.P. ({}) and the {} objects built at it", aPlayerId, owner.c_str(), aCampId, items.size() - 1);
}

void WorkshopService::SendCamps(Player* apPlayer) const noexcept
{
    NotifyCamps notify;
    notify.Camps.reserve(m_camps.size());
    for (const auto& [campId, camp] : m_camps)
        notify.Camps.push_back(camp);

    if (apPlayer)
        apPlayer->Send(notify);
    else
        GameServer::Get()->SendToPlayers(notify);
}

void WorkshopService::LoadCamps() noexcept
{
    m_campsFile = GetDataFolder() / kCampsFileName;

    std::ifstream file(m_campsFile);
    if (!file)
        return;

    std::string text;
    size_t lineNumber = 0;
    size_t bad = 0;
    while (std::getline(file, text))
    {
        ++lineNumber;
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        if (text.empty() || text[0] == '#')
            continue;

        const auto fields = SplitTabs(text);
        if (fields.size() == 2 && fields[0] == "next")
        {
            m_nextCampId = std::max(m_nextCampId, static_cast<uint32_t>(std::strtoul(fields[1].c_str(), nullptr, 10)));
            continue;
        }

        if (fields.size() >= 3 && fields[0] == "layout")
        {
            std::istringstream line(fields[2]);
            std::string kind;
            line >> kind;
            NotifyWorkshopChange object{};
            if (kind != "item" || !FromLine(line, object))
            {
                ++bad;
                spdlog::warn("{} line {} isn't readable, skipped: {}", kCampsFileName, lineNumber, text);
                continue;
            }
            m_layouts[std::strtoul(fields[1].c_str(), nullptr, 10)].Objects.push_back(object);
            continue;
        }

        Camp camp;
        if (fields.size() < 9 || fields[0] != "camp" || !FromText(fields[4], camp.WorldSpaceId))
        {
            ++bad;
            spdlog::warn("{} line {} isn't readable, skipped: {}", kCampsFileName, lineNumber, text);
            continue;
        }

        camp.Id = std::strtoul(fields[1].c_str(), nullptr, 10);
        camp.OwnerName = CleanName(String(fields[2].c_str()));
        camp.ModuleItemId = std::strtoul(fields[3].c_str(), nullptr, 10);
        for (size_t i = 0; i < 3; ++i)
            camp.Position[i] = std::strtof(fields[5 + i].c_str(), nullptr);
        camp.Heading = std::strtof(fields[8].c_str(), nullptr);
        camp.Stored = fields.size() >= 10 && fields[9] == "1";
        if (!camp.Id || camp.OwnerName.empty())
        {
            ++bad;
            spdlog::warn("{} line {} isn't readable, skipped: {}", kCampsFileName, lineNumber, text);
            continue;
        }

        // A stored one's module item is its layout's (nothing of it stands)
        if (camp.Stored)
        {
            m_layouts[camp.Id].ModuleItemId = camp.ModuleItemId;
            camp.ModuleItemId = 0;
        }
        m_camps[camp.Id] = camp;
        m_nextCampId = std::max(m_nextCampId, camp.Id + 1);
        if (!camp.Stored && !m_items.count(camp.ModuleItemId))
            spdlog::warn("C.A.M.P. {} ({}'s): its module (item {}) isn't in {}", camp.Id, camp.OwnerName.c_str(), camp.ModuleItemId, kDataFileName);
    }

    // Layouts of C.A.M.P.s that aren't stored (or aren't there) go
    for (auto it = m_layouts.begin(); it != m_layouts.end();)
    {
        const auto camp = m_camps.find(it->first);
        if (camp == m_camps.end() || !camp->second.Stored)
            it = m_layouts.erase(it);
        else
            ++it;
    }

    spdlog::info("Loaded {} C.A.M.P.s ({} stored) from {}{}", m_camps.size(), m_layouts.size(), m_campsFile.string(), bad ? fmt::format(" ({} lines skipped)", bad) : "");
}

void WorkshopService::SaveCamps() const noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(m_campsFile.parent_path(), ec);

    // Written aside, then put in place, as settlements.txt
    auto temporary = m_campsFile;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file)
        {
            spdlog::error("Couldn't write {}, C.A.M.P.s aren't kept", temporary.string());
            return;
        }

        file << kCampsHeader << '\n';
        file << "# camp<TAB>id<TAB>owner<TAB>module item<TAB>worldspace<TAB>x<TAB>y<TAB>z<TAB>heading<TAB>stored, then for a stored one layout<TAB>id<TAB>"
                "<a settlements.txt item line> for each object it had\n";
        file << "next\t" << m_nextCampId << '\n';
        for (const auto& [campId, camp] : m_camps)
        {
            const auto layout = camp.Stored ? m_layouts.find(campId) : m_layouts.end();
            const uint32_t moduleItemId = layout != m_layouts.end() ? layout->second.ModuleItemId : camp.ModuleItemId;
            file << fmt::format("camp\t{}\t{}\t{}\t{}\t{:.9g}\t{:.9g}\t{:.9g}\t{:.9g}\t{}\n", camp.Id, camp.OwnerName.c_str(), moduleItemId, ToText(camp.WorldSpaceId),
                                camp.Position[0], camp.Position[1], camp.Position[2], camp.Heading, camp.Stored ? 1 : 0);
            if (layout != m_layouts.end())
            {
                for (const NotifyWorkshopChange& object : layout->second.Objects)
                    file << "layout\t" << campId << '\t' << ToLine("item", object);
            }
        }

        if (!file.flush())
        {
            spdlog::error("Couldn't write {}, C.A.M.P.s aren't kept", temporary.string());
            return;
        }
    }

    std::filesystem::rename(temporary, m_campsFile, ec);
    if (ec)
        spdlog::error("Couldn't replace {}: {}", m_campsFile.string(), ec.message());
}

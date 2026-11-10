#include <Services/NemesisService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/CharacterRemoveEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyNemesis.h>
#include <Messages/RequestNemesis.h>

#include <Nemesis/NemesisWire.h>

#include <fstream>
#include <random>
#include <sstream>

namespace
{
constexpr char kDataFileName[] = "nemeses.txt";
// A player's spot is used for rolls while it's this fresh (their game sends one every few seconds)
constexpr auto kSpotFreshness = std::chrono::seconds(30);

// The engine's numbers are the wire's
static_assert(Nemesis::Wire::kMet == RequestNemesis::kMet);
static_assert(Nemesis::Wire::kHead == RequestNemesis::kHead);
static_assert(Nemesis::Wire::kCancelled == RequestNemesis::kCancelled);
static_assert(Nemesis::Wire::kOpen == RequestNemesis::kOpen);
static_assert(Nemesis::Wire::kPatrol == NemesisSpawn::kPatrol);
static_assert(Nemesis::Wire::kAttack == NotifyNemesis::kAttack);
static_assert(Nemesis::Wire::kLetGoRuns == NotifyNemesis::kLetGo && Nemesis::Wire::kLetGoFights == NotifyNemesis::kLetGoFight);
static_assert(Nemesis::Wire::kWorld == NotifyNemesis::kWorld);
static_assert(Nemesis::Wire::kRinging == NemesisMan::kRinging);
static_assert(static_cast<uint8_t>(Nemesis::Kind::SuperMutant) == NemesisMan::kSuperMutant);
static_assert(static_cast<uint8_t>(Nemesis::Part::Unknown) == RequestNemesis::kPartUnknown);

std::filesystem::path GetDataFolder() noexcept
{
    if (const wchar_t* pFolder = _wgetenv(L"RD_WORLD_DATA"); pFolder && *pFolder)
        return std::filesystem::path(pFolder);
    return std::filesystem::current_path() / "data";
}

std::string ToStd(const String& acText)
{
    return std::string(acText.c_str(), acText.size());
}

// What a game wrote, as one line of text (names and places are shown; faces aren't text and don't go through here)
std::string Clean(const String& acText, size_t aMax = 96)
{
    std::string text = ToStd(acText);
    for (char& c : text)
    {
        if (c == '\t' || c == '\r' || c == '\n' || static_cast<unsigned char>(c) < 0x20)
            c = ' ';
    }
    if (text.size() > aMax)
        text.resize(aMax);
    return text;
}
} // namespace

NemesisService::NemesisService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_engine(std::random_device{}())
{
    m_requestConnection = aDispatcher.sink<PacketEvent<RequestNemesis>>().connect<&NemesisService::OnRequest>(this);
    m_playerJoinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&NemesisService::OnPlayerJoin>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&NemesisService::OnPlayerLeave>(this);
    m_characterRemoveConnection = aDispatcher.sink<CharacterRemoveEvent>().connect<&NemesisService::OnCharacterRemove>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&NemesisService::OnUpdate>(this);

    Load();
}

NemesisService::~NemesisService() noexcept
{
    if (m_engine.Dirty)
        Save();
}

// --- players

std::string NemesisService::NameOf(const Player& acPlayer) noexcept
{
    return ToStd(acPlayer.GetUsername());
}

bool NemesisService::IsHost(const Player& acPlayer) noexcept
{
    // The host connects through 127.0.0.1 to the server its game started (as WorkshopService)
    return GameServer::Get()->GetAdminSessions().count(acPlayer.GetConnectionId()) != 0;
}

std::vector<std::string> NemesisService::PartyOf(Player& aPlayer) const noexcept
{
    std::vector<std::string> names{NameOf(aPlayer)};
    if (const PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(&aPlayer))
    {
        for (const Player* pMember : pParty->Members)
        {
            if (pMember && pMember != &aPlayer)
                names.push_back(NameOf(*pMember));
        }
    }
    return names;
}

void NemesisService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    // Their game says hello (campaign, karma) once it's in; until then they're only known
    if (acEvent.pPlayer && m_engine.IsStarted())
        m_engine.PlayerJoined(NameOf(*acEvent.pPlayer), GameMinutes());
}

void NemesisService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (!acEvent.pPlayer)
        return;
    m_spots.erase(acEvent.pPlayer->GetId());
    m_engine.PlayerLeft(NameOf(*acEvent.pPlayer));
}

void NemesisService::OnCharacterRemove(const CharacterRemoveEvent& acEvent) noexcept
{
    m_engine.ActorGone(acEvent.ServerId);
}

// --- the clock

int64_t NemesisService::GameMinutes() const noexcept
{
    const auto [day, month, year] = m_world.GetCalendarService().GetDate();
    // The calendar starts at year 1 until the first player's game sets it (to Fallout 4's GameYear: 287 for 2287)
    if (year <= 1)
        return 0;
    const auto [hour, minute] = m_world.GetCalendarService().GetTime();
    return Nemesis::Text::ToMinutes(year, month, day, hour, std::min(59, std::max(0, minute)));
}

void NemesisService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_tickAt < std::chrono::milliseconds(250))
        return;
    m_tickAt = now;

    const int64_t minutes = GameMinutes();
    if (!minutes)
        return;

    if (!m_engine.IsStarted() && !m_campaign.empty())
    {
        m_engine.Start(m_campaign, m_plugins, minutes);
        spdlog::info("Nemeses: a new nemesis world for the {} campaign: {} men in {} gangs", m_campaign, m_engine.State().Records.size(), m_engine.State().Gangs.size());
        PlayerManager::Get()->ForEach([this, minutes](Player* apPlayer) { m_engine.PlayerJoined(NameOf(*apPlayer), minutes); });
    }
    if (!m_engine.IsStarted())
        return;

    m_engine.Advance(minutes);

    // An encounter for whoever can be met (the engine keeps each player's next roll)
    for (auto& [playerId, spot] : m_spots)
    {
        if (now - spot.At > kSpotFreshness)
            continue;
        if (auto plan = m_engine.RollEncounter(spot.Where, minutes))
            SendSpawn(*plan);
    }

    Flush();

    if (m_engine.Dirty && now - m_savedAt >= std::chrono::seconds(10))
    {
        m_savedAt = now;
        Save();
    }
}

// --- messages

void NemesisService::OnRequest(const PacketEvent<RequestNemesis>& acMessage) noexcept
{
    const RequestNemesis& message = acMessage.Packet;
    Player& player = *acMessage.pPlayer;
    const std::string name = NameOf(player);
    const int64_t minutes = GameMinutes();

    if (message.Action == RequestNemesis::kHello)
    {
        OnHello(player, message);
        return;
    }
    if (!m_engine.IsStarted() || !minutes)
        return;

    switch (message.Action)
    {
    case RequestNemesis::kStatus:
    {
        Spot& spot = m_spots[player.GetId()];
        spot.Where.Player = name;
        spot.Where.CanMeet = message.Kind != 0;
        spot.Where.World = Clean(message.World, 64);
        spot.Where.X = message.Position[0];
        spot.Where.Y = message.Position[1];
        spot.At = std::chrono::steady_clock::now();
        break;
    }
    case RequestNemesis::kDeed:
    {
        Nemesis::DeedInput deed;
        deed.Deed = message.Kind;
        deed.Amount = message.Value;
        deed.RecordId = message.RecordId;
        deed.ActorServerId = message.ActorServerId;
        deed.Body = static_cast<Nemesis::Kind>(std::min<uint8_t>(message.Body, static_cast<uint8_t>(Nemesis::Kind::SuperMutant)));
        deed.Appearance = ToStd(message.Appearance);
        deed.AppearanceFlags = message.AppearanceFlags;
        deed.Gang = Clean(message.Text, 32);
        deed.World = Clean(message.World, 64);
        deed.X = message.Position[0];
        deed.Y = message.Position[1];
        deed.Place = Clean(message.Text2);
        m_engine.OnDeed(name, deed, minutes);
        break;
    }
    case RequestNemesis::kDeath:
    {
        Nemesis::DeathInput death;
        death.RecordId = message.RecordId;
        death.ActorServerId = message.ActorServerId;
        death.Cause = message.Kind;
        death.Flags = message.Value;
        death.Gang = Clean(message.Text, 32);
        death.World = Clean(message.World, 64);
        death.X = message.Position[0];
        death.Y = message.Position[1];
        death.Place = Clean(message.Text2);
        m_engine.OnDeath(name, death, minutes);
        break;
    }
    case RequestNemesis::kWound:
    {
        Nemesis::WoundInput wound;
        wound.RecordId = message.RecordId;
        wound.ActorServerId = message.ActorServerId;
        wound.Where = static_cast<Nemesis::Part>(std::min<uint8_t>(message.Kind & ~RequestNemesis::kPartCrippled, RequestNemesis::kPartUnknown));
        wound.Crippled = (message.Kind & RequestNemesis::kPartCrippled) != 0;
        wound.Cause = message.Body;
        wound.Damage = std::clamp(message.Value, 0, 100000);
        wound.Hits = std::clamp<int>(message.Count, 1, 1000);
        // Game units to centimetres (a unit is about 1.43 cm)
        wound.Range = static_cast<int>(std::clamp(message.Range, 0.f, 1.0e6f) * 1.428f);
        wound.Weapon = Clean(message.Text, 64);
        wound.Ammo = Clean(message.Text2, 64);
        wound.World = Clean(message.World, 64);
        wound.X = message.Position[0];
        wound.Y = message.Position[1];
        m_engine.OnWound(name, wound, minutes);
        break;
    }
    case RequestNemesis::kOutcome:
        m_engine.OnOutcome(name, message.RecordId, message.Kind, minutes);
        break;
    case RequestNemesis::kSpawned:
        m_engine.OnSpawned(message.RecordId, std::vector<uint32_t>(message.ServerIds.begin(), message.ServerIds.end()));
        break;
    case RequestNemesis::kLook:
        // A face is a few kilobytes at most; anything bigger isn't one
        if (message.Appearance.size() <= (1u << 15))
            m_engine.OnLook(message.RecordId, message.ActorServerId, ToStd(message.Appearance), message.AppearanceFlags);
        break;
    case RequestNemesis::kKarma:
        m_engine.SetKarma(name, message.Value, message.Kind);
        break;
    case RequestNemesis::kPlace:
        if (auto plan = m_engine.OnPlace(name, Clean(message.Text, 64), minutes))
            SendSpawn(*plan);
        break;
    case RequestNemesis::kQuestion:
        SendUi(player, "nemesisCard", m_engine.OnQuestion(name, message.RecordId, message.ActorServerId, message.Kind, std::max(0, message.Value), minutes));
        break;
    case RequestNemesis::kUi:
        OnUi(player, Clean(message.Text, 32), Clean(message.Text2, 512));
        break;
    default:
        break;
    }

    Flush();
}

void NemesisService::OnHello(Player& aPlayer, const RequestNemesis& acMessage) noexcept
{
    const std::string name = NameOf(aPlayer);
    const std::string campaign = Clean(acMessage.Text, 32) == "winter-of-atom" ? "winter-of-atom" : "2287";

    // The host's game picks the campaign (the first game to say hello if the host hasn't): a world is one campaign
    if (m_campaign.empty() || (IsHost(aPlayer) && !m_engine.IsStarted()))
    {
        m_campaign = campaign;
        m_plugins.clear();
        std::stringstream plugins(Clean(acMessage.Text2, 512));
        std::string plugin;
        while (std::getline(plugins, plugin, ','))
        {
            if (!plugin.empty())
                m_plugins.push_back(plugin);
        }
        m_plugins.push_back("Fallout4.esm");
    }
    else if (m_engine.IsStarted() && m_engine.State().Campaign != campaign)
        spdlog::warn("Nemeses: {} plays the {} campaign, this world's nemeses are the {} campaign's", name, campaign, m_engine.State().Campaign);

    const int64_t minutes = GameMinutes();
    if (!m_engine.IsStarted() && minutes)
    {
        m_engine.Start(m_campaign, m_plugins, minutes);
        spdlog::info("Nemeses: a new nemesis world for the {} campaign: {} men in {} gangs", m_campaign, m_engine.State().Records.size(), m_engine.State().Gangs.size());
    }
    else if (m_engine.IsStarted() && IsHost(aPlayer))
    {
        // DLC gangs installed since: they're seeded now
        m_engine.Start(m_engine.State().Campaign, m_plugins, minutes);
    }
    if (!m_engine.IsStarted())
        return;

    m_engine.PlayerJoined(name, minutes);
    m_engine.SetKarma(name, acMessage.Value, acMessage.Kind);

    // The names of the nemeses already out there
    for (const auto& [id, record] : m_engine.State().Records)
    {
        if (record.LiveActor)
            SendIdentity(record.LiveActor, record, &aPlayer);
    }
    spdlog::info("Nemeses: {} is in ({} campaign, karma {}, alignment {})", name, campaign, acMessage.Value, acMessage.Kind);
    Flush();
}

void NemesisService::OnUi(Player& aPlayer, const std::string& acRequest, const std::string& acPayload) noexcept
{
    const std::string name = NameOf(aPlayer);
    // The Pip-Boy's NEMESIS tab
    if (acRequest == "pipboy")
        SendUi(aPlayer, "nemesesPipboy", m_engine.PipboyPages(name, PartyOf(aPlayer)));
    // The overlay's nemesis screen, a page at a time ("record", "gang:<id>", "man:<id>", "world", "wounds")
    else if (acRequest == "screen")
        SendUi(aPlayer, "nemesisScreen", m_engine.ScreenJson(name, PartyOf(aPlayer), acPayload));
    // The bar over a surrendered man held at gunpoint: "<record id>:<actor server id>"
    else if (acRequest == "gunpoint")
    {
        const auto colon = acPayload.find(':');
        const uint32_t recordId = static_cast<uint32_t>(std::strtoul(acPayload.c_str(), nullptr, 10));
        const uint32_t actorId = colon == std::string::npos ? 0u : static_cast<uint32_t>(std::strtoul(acPayload.c_str() + colon + 1, nullptr, 10));
        SendUi(aPlayer, "nemesisGunpoint", m_engine.GunpointJson(name, recordId, actorId));
    }
    else if (acRequest == "settings" && IsHost(aPlayer))
        ApplySettings(acPayload);
    else if (acRequest == "reset" && IsHost(aPlayer))
    {
        m_engine.Reset(GameMinutes());
        spdlog::info("Nemeses: {} started the nemesis world over", name);
        SendUi(aPlayer, "nemesesPipboy", m_engine.PipboyPages(name, PartyOf(aPlayer)));
    }
}

// The host's MCM page: "enabled=1;hours=5;chance=50;cheat=25;news=1;hate=1;fear=1"
void NemesisService::ApplySettings(const std::string& acSettings) noexcept
{
    Nemesis::Settings& settings = m_engine.Config();
    std::stringstream stream(acSettings);
    std::string pair;
    while (std::getline(stream, pair, ';'))
    {
        const auto equals = pair.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = pair.substr(0, equals);
        const float value = std::strtof(pair.c_str() + equals + 1, nullptr);
        if (key == "enabled")
            settings.Enabled = value != 0.f;
        else if (key == "hours")
            settings.EncounterHours = std::clamp(value, 1.f, 48.f);
        else if (key == "chance")
            settings.EncounterChance = std::clamp(static_cast<int>(value), 0, 100);
        else if (key == "cheat")
            settings.CheatDeathBase = std::clamp(static_cast<int>(value), 0, 90);
        else if (key == "news")
            settings.WorldNews = value != 0.f;
        else if (key == "hate")
            settings.HateScale = std::clamp(value, 0.25f, 3.f);
        else if (key == "fear")
            settings.FearScale = std::clamp(value, 0.25f, 3.f);
    }
    spdlog::info("Nemeses: settings {} (on {}, every {:.1f} h, {}%, cheat death {}%)", acSettings, settings.Enabled, settings.EncounterHours, settings.EncounterChance, settings.CheatDeathBase);
}

// --- sending

void NemesisService::SendToName(const std::string& acPlayer, const NotifyNemesis& acMessage) const noexcept
{
    if (acPlayer.empty())
    {
        GameServer::Get()->SendToPlayers(acMessage);
        return;
    }
    if (const Player* pPlayer = PlayerManager::Get()->GetByUsername(String(acPlayer.c_str())))
        pPlayer->Send(acMessage);
}

void NemesisService::SendUi(Player& aPlayer, const char* acpEvent, const std::string& acJson) const noexcept
{
    NotifyNemesis message;
    message.Action = NotifyNemesis::kUi;
    message.Text = acJson.c_str();
    message.Text2 = acpEvent;
    aPlayer.Send(message);
}

void NemesisService::SendIdentity(uint32_t aActorServerId, const Nemesis::Record& acRecord, Player* apTo) const noexcept
{
    NotifyNemesis message;
    message.Action = NotifyNemesis::kIdentity;
    message.ActorServerId = aActorServerId;
    message.RecordId = acRecord.Id;
    message.Text = acRecord.FullName().c_str();
    message.Kind = static_cast<uint8_t>(acRecord.Rank);
    NotifyNemesis::Identity identity;
    identity.Alignment = acRecord.Alignment;
    identity.Injuries = acRecord.Injuries;
    identity.Legendary = acRecord.Rank >= 4;
    identity.Temper = static_cast<int>(acRecord.Temper);
    identity.Wits = acRecord.Wits;
    identity.Aggression = acRecord.Aggression;
    identity.Nerve = acRecord.Nerve;
    identity.Loyalty = acRecord.Loyalty;
    message.Value = NotifyNemesis::PackIdentity(identity);
    if (apTo)
        apTo->Send(message);
    else
        GameServer::Get()->SendToPlayers(message);
}

void NemesisService::SendSpawn(const Nemesis::SpawnPlan& acPlan) noexcept
{
    NotifyNemesis message;
    message.Action = NotifyNemesis::kSpawn;
    NemesisSpawn& spawn = message.Spawn;
    spawn.EncounterId = acPlan.EncounterId;
    spawn.Kind = acPlan.Kind;
    spawn.Gang = acPlan.Gang.c_str();
    spawn.Taunt = acPlan.Taunt.c_str();
    spawn.Distance = acPlan.Distance;
    if (const auto colon = acPlan.Location.find(':'); colon != std::string::npos)
    {
        spawn.LocationPlugin = acPlan.Location.substr(0, colon).c_str();
        spawn.LocationLocalId = static_cast<uint32_t>(std::strtoul(acPlan.Location.c_str() + colon + 1, nullptr, 16));
    }
    for (const Nemesis::SpawnPlan::Man& planned : acPlan.Men)
    {
        NemesisMan man;
        man.RecordId = planned.RecordId;
        man.Body = static_cast<uint8_t>(planned.Body);
        man.Appearance = String(planned.Appearance.data(), planned.Appearance.size());
        man.AppearanceFlags = planned.AppearanceFlags;
        man.Name = planned.Name.c_str();
        man.Rank = static_cast<uint8_t>(std::clamp(planned.Rank, 1, 5));
        man.Injuries = planned.Injuries;
        man.Alignment = static_cast<int8_t>(std::clamp(planned.Alignment, -2, 2));
        man.Legendary = planned.Legendary;
        spawn.Men.push_back(man);
    }

    if (const Player* pPlayer = PlayerManager::Get()->GetByUsername(String(acPlan.Player.c_str())))
    {
        pPlayer->Send(message);
        spdlog::info("Nemeses: encounter {} for {}: kind {}, {} ({} men)", acPlan.EncounterId, acPlan.Player, acPlan.Kind, acPlan.Men.empty() ? std::string() : acPlan.Men[0].Name,
                     acPlan.Men.size());
    }
    else
        m_engine.OnOutcome(acPlan.Player, acPlan.EncounterId, Nemesis::Wire::kCancelled, GameMinutes());
}

void NemesisService::Flush() noexcept
{
    for (const Nemesis::Notice& notice : m_engine.Notices)
    {
        if (notice.Style == Nemesis::Wire::kWorld && !m_engine.Config().WorldNews)
            continue;
        NotifyNemesis message;
        message.Action = NotifyNemesis::kNotice;
        message.Kind = notice.Style;
        message.Text = notice.Text.c_str();
        SendToName(notice.Player, message);
    }
    m_engine.Notices.clear();

    for (const Nemesis::LiveChange& change : m_engine.Live)
    {
        if (!change.ActorServerId)
            continue;
        if (change.What == Nemesis::LiveChange::kIdentity)
        {
            if (const Nemesis::Record* pRecord = m_engine.Find(change.RecordId))
                SendIdentity(change.ActorServerId, *pRecord);
            continue;
        }
        NotifyNemesis message;
        message.Action = NotifyNemesis::kOrder;
        message.ActorServerId = change.ActorServerId;
        message.RecordId = change.RecordId;
        message.Kind = change.Order;
        if (const Player* pPlayer = change.ForPlayer.empty() ? nullptr : PlayerManager::Get()->GetByUsername(String(change.ForPlayer.c_str())))
            message.Value = static_cast<int32_t>(pPlayer->GetId());
        GameServer::Get()->SendToPlayers(message);
    }
    m_engine.Live.clear();

    for (const Nemesis::KarmaChange& change : m_engine.Karma)
    {
        NotifyNemesis message;
        message.Action = NotifyNemesis::kKarma;
        message.Value = change.Amount;
        message.Kind = change.Silent ? 1 : 0;
        SendToName(change.Player, message);
    }
    m_engine.Karma.clear();

    for (const auto& [player, encounterId] : m_engine.EndedEncounters)
    {
        NotifyNemesis message;
        message.Action = NotifyNemesis::kEndEncounter;
        message.RecordId = encounterId;
        SendToName(player, message);
    }
    m_engine.EndedEncounters.clear();
}

// --- nemeses.txt

void NemesisService::Load() noexcept
{
    m_file = GetDataFolder() / kDataFileName;

    std::ifstream file(m_file, std::ios::binary);
    if (!file)
    {
        spdlog::info("Nemeses: none kept for this world yet ({})", m_file.string());
        return;
    }
    std::stringstream text;
    text << file.rdbuf();

    int bad = 0;
    if (!m_engine.Load(text.str(), bad))
    {
        spdlog::error("Nemeses: {} isn't a nemeses file, a new world starts (the old one is kept as {}.bad)", m_file.string(), kDataFileName);
        std::error_code ec;
        std::filesystem::copy_file(m_file, m_file.string() + ".bad", std::filesystem::copy_options::overwrite_existing, ec);
        return;
    }
    m_campaign = m_engine.State().Campaign;
    spdlog::info("Nemeses: loaded the {} campaign's nemeses from {}: {} men, {} gangs, {} events{}", m_campaign, m_file.string(), m_engine.State().Records.size(),
                 m_engine.State().Gangs.size(), m_engine.State().Chronicle.size(), bad ? fmt::format(" ({} lines skipped)", bad) : "");
}

void NemesisService::Save() noexcept
{
    if (!m_engine.IsStarted())
        return;

    std::error_code ec;
    std::filesystem::create_directories(m_file.parent_path(), ec);

    // Written aside, then put in place: the server can be ended at any moment
    auto temporary = m_file;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string text = m_engine.Save();
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size())) || !file.flush())
        {
            spdlog::error("Nemeses: couldn't write {}, the nemeses aren't kept", temporary.string());
            return;
        }
    }

    std::filesystem::rename(temporary, m_file, ec);
    if (ec)
    {
        spdlog::error("Nemeses: couldn't replace {}: {}", m_file.string(), ec.message());
        return;
    }
    m_engine.Dirty = false;
}

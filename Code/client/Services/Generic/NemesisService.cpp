#include <Services/NemesisService.h>

#if TP_FALLOUT4

#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <SaveFolder.h>
#include <Games/ActorExtension.h>
#include <Games/TES.h>
#include <Forms/TESNPC.h>
#include <Events/ActorDamageEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/OverlayRestartedEvent.h>
#include <Events/UpdateEvent.h>
#include <Games/Fallout4/NemesisGame.h>
#include <Games/Fallout4/NemesisPipboy.h>
#include <Interface/UI.h>
#include <Messages/RequestNemesis.h>
#include <Services/OverlayService.h>
#include <Services/TransportService.h>

#include <random>
#include <PerfProbe.h>

namespace
{
constexpr double kTickInterval = 0.25;
constexpr double kStatusInterval = 5.0;
constexpr double kSettingsInterval = 2.0;
// The Pip-Boy pages: asked for this often, and this soon after news
constexpr double kPagesInterval = 30.0;
constexpr double kPagesAfterNews = 1.0;
// A fight is over when he's been out of combat this long, or this far away
constexpr double kFightTimeout = 60.0;
constexpr float kFightRange = 8000.f;
constexpr size_t kMaxFights = 32;
// A death this soon after the local player's hit is theirs; going down this soon after a man's hit is his doing
constexpr double kKillWindow = 6.0;
constexpr double kDownWindow = 15.0;
// His gang mates this close saw him die
constexpr float kWitnessRange = 3000.f;
// An encounter: its men's server ids are waited for this long; it's over when they're this far, or after this long
constexpr double kServerIdWait = 12.0;
constexpr float kEncounterRange = 12000.f;
constexpr double kEncounterLength = 600.0;
// The player meets a nemesis when this close
constexpr float kMeetRange = 2500.f;
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr wchar_t kSettingsSection[] = L"Nemeses";
// The plugins whose gangs the server seeds when they're loaded
constexpr const char* kGangPlugins[] = {"DLCRobot.esm", "DLCCoast.esm", "DLCNukaWorld.esm", "KARMA.esm", "RD_WinterOfAtom.esp"};

double Now() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float DistanceBetween(const NiPoint3& acA, const NiPoint3& acB) noexcept
{
    const float dx = acA.x - acB.x;
    const float dy = acA.y - acB.y;
    const float dz = acA.z - acB.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Actor* ActorOf(uint32_t aFormId) noexcept
{
    return aFormId ? Cast<Actor>(TESForm::GetById(aFormId)) : nullptr;
}

// The server's id of a character (local or another player's), 0 until the character sync has it
uint32_t ServerIdOf(uint32_t aFormId) noexcept
{
    auto& world = World::Get();
    for (const auto entity : world.view<FormIdComponent, LocalComponent>())
    {
        if (world.get<FormIdComponent>(entity).Id == aFormId)
            return world.get<LocalComponent>(entity).Id;
    }
    for (const auto entity : world.view<FormIdComponent, RemoteComponent>())
    {
        if (world.get<FormIdComponent>(entity).Id == aFormId)
            return world.get<RemoteComponent>(entity).Id;
    }
    return 0;
}

std::string ReadCampaign() noexcept
{
    std::string campaign;
    if (const auto folder = SaveFolder::GetGameUserFolder(); !folder.empty())
    {
        std::ifstream file(folder / L"ReclamationDay" / L"campaign.txt");
        std::getline(file, campaign);
    }
    campaign.erase(std::remove_if(campaign.begin(), campaign.end(), [](unsigned char c) { return std::isspace(c); }), campaign.end());
    std::transform(campaign.begin(), campaign.end(), campaign.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return campaign == "winter-of-atom" ? campaign : std::string("2287");
}

void ShowOverlayCursor(bool aShow) noexcept
{
    auto& overlay = World::Get().ctx().at<OverlayService>();
    if (overlay.GetActive() == aShow)
        return;
    overlay.SetActive(aShow);
    if (aShow)
        while (ShowCursor(TRUE) <= 0)
            ;
    else
        while (ShowCursor(FALSE) >= 0)
            ;
}

// "him" or "her" by a fight's body (NemesisGame::BodyOf: 1 and 3 are women)
const char* HimOf(uint8_t aBody) noexcept
{
    return aBody == 1 || aBody == 3 ? "her" : "him";
}

// The game's window is in front (the keys aren't going to another window)
bool GameHasKeyboard() noexcept
{
    HWND window = GetForegroundWindow();
    DWORD processId = 0;
    if (window)
        GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

int RandomPercent() noexcept
{
    static std::mt19937 s_random{std::random_device{}()};
    return std::uniform_int_distribution<int>(0, 99)(s_random);
}

float RandomOffset(float aRange) noexcept
{
    static std::mt19937 s_random{std::random_device{}()};
    return std::uniform_real_distribution<float>(-aRange, aRange)(s_random);
}

const char* NameOf(const Actor* apActor) noexcept
{
    const auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    const char* pName = pNpc ? pNpc->fullName.value.AsAscii() : nullptr;
    if (pName && *pName)
        return pName;
    const uint8_t body = NemesisGame::BodyOf(apActor);
    return body == 1 || body == 3 ? "She" : "He";
}
} // namespace

// The form ids of surrendered men, read by the activation hook
static std::mutex s_surrenderedLock;
static std::unordered_set<uint32_t> s_surrendered;

std::string NemesisService::Settings::ForServer() const
{
    return fmt::format("enabled={};hours={:.1f};chance={};cheat={};news={}", Enabled ? 1 : 0, EncounterHours, EncounterChance, CheatDeath, WorldNews ? 1 : 0);
}

NemesisService::NemesisService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&NemesisService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&NemesisService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&NemesisService::OnDisconnected>(this);
    m_overlayRestartedConnection = aDispatcher.sink<OverlayRestartedEvent>().connect<&NemesisService::OnOverlayRestarted>(this);
    m_damageConnection = aDispatcher.sink<ActorDamageEvent>().connect<&NemesisService::OnDamage>(this);
    m_notifyConnection = aDispatcher.sink<NotifyNemesis>().connect<&NemesisService::OnNotify>(this);
    LoadKeys();
}

// config/Nemeses.ini: [Keys] Screen = a letter, a digit, F1-F12 or a virtual key code ("0x4E"); N by default
void NemesisService::LoadKeys() noexcept
{
    const auto path = TiltedPhoques::GetPath() / "config" / "Nemeses.ini";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path);
        file << "; Reclamation Day's nemeses: the key that opens the full nemesis screen (also from the Pip-Boy).\n"
                "; A letter, a digit, F1-F12 or a virtual key code such as 0x4E. N is free in Fallout 4's own controls.\n"
                "[Keys]\nScreen=N\n";
    }

    wchar_t text[32]{};
    GetPrivateProfileStringW(L"Keys", L"Screen", L"N", text, static_cast<DWORD>(std::size(text)), path.c_str());
    std::wstring value = text;
    value.erase(std::remove_if(value.begin(), value.end(), iswspace), value.end());
    std::transform(value.begin(), value.end(), value.begin(), towupper);

    int key = 0;
    std::string name;
    if (value.size() == 1 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'0' && value[0] <= L'9')))
        key = value[0], name = std::string(1, static_cast<char>(value[0]));
    else if (value.size() >= 2 && value[0] == L'F' && iswdigit(value[1]))
    {
        const int number = std::stoi(value.substr(1));
        if (number >= 1 && number <= 12)
            key = VK_F1 + number - 1, name = "F" + std::to_string(number);
    }
    else if (value.rfind(L"0X", 0) == 0)
    {
        key = static_cast<int>(std::wcstoul(value.c_str() + 2, nullptr, 16));
        name = fmt::format("key 0x{:X}", key);
    }
    if (key <= 0 || key > 0xFE)
        key = 'N', name = "N";
    m_screenKey = key;
    NemesisPipboy::SetKeyName(name);
    spdlog::info("Nemeses: {} opens the nemesis screen (config/Nemeses.ini)", name);
}

// --- connection

void NemesisService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    m_playerId = acEvent.PlayerId;
    m_helloSent = false;
    m_sentSettings.clear();
}

void NemesisService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // The men already out that are far go; the rest stay as they are (CharacterService deletes temporary characters
    // when the game connects again)
    if (m_encounter)
        for (const EncounterMan& man : m_encounter->Men)
            m_leftovers.push_back(man.FormId);
    ClearLeftovers(false);
    m_leftovers.clear();
    m_encounter.reset();
    m_fights.clear();
    m_identities.clear();
    m_helloSent = false;
    m_karmaKnown = false;
    m_location = nullptr;
    m_pagesAt = 0.0;
    m_pagesWantedAt = 0.0;
    NemesisPipboy::Clear();
    m_gunpoint = 0;
    m_screenWanted = false;
    if (m_screenOpen)
        CloseScreen();
    {
        std::scoped_lock lock(s_surrenderedLock);
        s_surrendered.clear();
    }
    if (m_questioning)
        CloseCard();
}

void NemesisService::OnOverlayRestarted(const OverlayRestartedEvent&) noexcept
{
    // The screen and the questioning card went with the old page
    if (m_screenOpen)
        CloseScreen();
    if (m_questioning)
        CloseCard();
}

void NemesisService::SendRequest(RequestNemesis& aRequest) noexcept
{
    if (m_transport.IsConnected())
        m_transport.Send(aRequest);
}

void NemesisService::SendHello() noexcept
{
    m_winterOfAtom = ReadCampaign() == "winter-of-atom";
    m_karmaKnown = NemesisGame::ReadKarma(m_karma, m_alignment);
    if (!m_karmaKnown)
    {
        m_karma = 0;
        m_alignment = 2;
    }

    std::string plugins;
    for (const char* pPlugin : kGangPlugins)
    {
        const Mod* pMod = ModManager::Get()->GetByName(pPlugin);
        if (pMod && pMod->IsLoaded())
            plugins += std::string(plugins.empty() ? "" : ",") + pPlugin;
    }

    RequestNemesis request;
    request.Action = RequestNemesis::kHello;
    request.Text = m_winterOfAtom ? "winter-of-atom" : "2287";
    request.Text2 = plugins.c_str();
    request.Value = m_karma;
    request.Kind = static_cast<uint8_t>(m_alignment);
    SendRequest(request);
    m_helloSent = true;
    spdlog::info("Nemeses: hello sent ({} campaign, karma {} alignment {}, plugins {})", m_winterOfAtom ? "winter-of-atom" : "2287", m_karma, m_alignment, plugins);
}

// --- settings (the MCM page; the host's go to the server)

void NemesisService::LoadSettings() noexcept
{
    const auto path = std::filesystem::current_path() / L"Data" / kSettingsFile;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    const auto stamp = ec ? std::filesystem::file_time_type{} : time;
    if (m_settingsLoaded && stamp == m_settingsTime)
        return;
    m_settingsLoaded = true;
    m_settingsTime = stamp;

    const std::wstring file = path.wstring();
    const auto number = [&file](const wchar_t* acpKey, float aDefault)
    {
        wchar_t text[32]{};
        GetPrivateProfileStringW(kSettingsSection, acpKey, L"", text, static_cast<DWORD>(std::size(text)), file.c_str());
        wchar_t* pEnd = nullptr;
        const float value = std::wcstof(text, &pEnd);
        return pEnd != text && std::isfinite(value) ? value : aDefault;
    };
    Settings settings;
    settings.Enabled = number(L"bEnabled", 1.f) != 0.f;
    settings.EncounterHours = std::clamp(number(L"fEncounterHours", 5.f), 1.f, 48.f);
    settings.EncounterChance = std::clamp(static_cast<int>(number(L"iEncounterChance", 50.f)), 0, 100);
    settings.CheatDeath = std::clamp(static_cast<int>(number(L"iCheatDeath", 25.f)), 0, 90);
    settings.WorldNews = number(L"bWorldNews", 1.f) != 0.f;
    settings.Surrender = number(L"bSurrender", 1.f) != 0.f;
    settings.KeepFaces = number(L"bKeepFaces", 0.f) != 0.f;
    settings.ShowTaunts = number(L"bShowTaunts", 1.f) != 0.f;
    m_settings = settings;
}

// --- the game's update

void NemesisService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfProbe::Section _perf("Nemesis");
    // Every frame: a short press of the screen's key (or of Activate at gunpoint) isn't missed between ticks
    CheckScreenKey();
    CheckQuestionKey();
    CheckInterrogateRequest();

    const double now = Now();
    if (now - m_tickAt < kTickInterval)
        return;
    m_tickAt = now;

    if (!m_transport.IsConnected())
        return;
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->baseForm || !pPlayer->GetParentCellEx())
        return;

    if (now - m_settingsAt >= kSettingsInterval)
    {
        m_settingsAt = now;
        LoadSettings();
    }

    if (!m_helloSent)
    {
        // Once the game is in (a load or the main menu don't count)
        if (NemesisGame::IsBusy())
            return;
        SendHello();
    }

    // The host's settings for the server (it ignores everyone else's)
    if (const std::string settings = m_settings.ForServer(); settings != m_sentSettings)
    {
        m_sentSettings = settings;
        RequestNemesis request;
        request.Action = RequestNemesis::kUi;
        request.Text = "settings";
        request.Text2 = settings.c_str();
        SendRequest(request);
    }

    if (now - m_statusAt >= kStatusInterval)
    {
        m_statusAt = now;
        SendStatus();
    }

    CheckPlace();
    CheckDowned();
    CheckFights();
    CheckHeldUp();
    CheckGunpoint();
    CheckIdentities();
    CheckPipboy();
    // Asked for from the Pip-Boy: open once it's down
    if (m_screenWanted && !NemesisGame::IsPipboyOpen() && !NemesisGame::IsBusy())
    {
        m_screenWanted = false;
        OpenScreen();
    }
    RunEncounter();
    if (now - m_leftoversAt >= 1.0)
    {
        m_leftoversAt = now;
        ClearLeftovers(false);
    }
}

void NemesisService::SendStatus() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    const bool canMeet = m_settings.Enabled && pPlayer->GetWorldSpace() && !NemesisGame::IsBusy() && !NemesisGame::InSettlement() &&
                         !pPlayer->actorState.IsBleedingOut() && !pPlayer->IsInCombat() && !m_encounter;

    RequestNemesis request;
    request.Action = RequestNemesis::kStatus;
    request.Kind = canMeet ? 1 : 0;
    request.World = NemesisGame::WorldKeyOf(pPlayer).c_str();
    request.Position = {pPlayer->position.x, pPlayer->position.y, pPlayer->position.z};
    SendRequest(request);

    // KARMA moved: the server's rules follow it
    int karma = 0;
    int alignment = 2;
    if (NemesisGame::ReadKarma(karma, alignment) && (!m_karmaKnown || karma != m_karma || alignment != m_alignment))
    {
        m_karmaKnown = true;
        m_karma = karma;
        m_alignment = alignment;
        RequestNemesis change;
        change.Action = RequestNemesis::kKarma;
        change.Value = karma;
        change.Kind = static_cast<uint8_t>(alignment);
        SendRequest(change);
    }
}

void NemesisService::CheckPlace() noexcept
{
    TESForm* pLocation = NemesisGame::PlayerLocation();
    if (pLocation == m_location)
        return;
    m_location = pLocation;
    if (!pLocation || !m_settings.Enabled || m_encounter)
        return;

    // A camp, a funeral, a man's habit there: the server decides
    RequestNemesis request;
    request.Action = RequestNemesis::kPlace;
    request.Text = NemesisGame::KeyOf(pLocation->formID).c_str();
    SendRequest(request);
}

void NemesisService::CheckDowned() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    const bool down = pPlayer->actorState.IsBleedingOut();
    if (down && !m_wasDown)
    {
        const double now = Now();
        if (m_lastAttacker && now - m_lastAttackerAt <= kDownWindow)
        {
            Actor* pAttacker = ActorOf(m_lastAttacker);
            if (Fight* pFight = FightOf(m_lastAttacker, false); pFight && pAttacker && !pAttacker->IsDead())
                SendDeed(RequestNemesis::kDownedYou, *pFight);
        }
        if (m_encounter && m_encounter->SpawnedAt)
            EndEncounter(RequestNemesis::kHeDownedYou);
    }
    m_wasDown = down;
}

// --- fights

uint32_t NemesisService::RecordOf(uint32_t aFormId) const noexcept
{
    if (m_encounter)
    {
        for (const EncounterMan& man : m_encounter->Men)
        {
            if (man.FormId == aFormId)
                return man.Plan.RecordId;
        }
    }
    const auto fight = m_fights.find(aFormId);
    if (fight != m_fights.end() && fight->second.RecordId)
        return fight->second.RecordId;
    if (const uint32_t serverId = ServerIdOf(aFormId))
    {
        const auto identity = m_identities.find(serverId);
        if (identity != m_identities.end())
            return identity->second.RecordId;
    }
    return 0;
}

const NotifyNemesis::Identity* NemesisService::TemperamentOf(uint32_t aFormId) const noexcept
{
    const uint32_t serverId = m_identities.empty() ? 0 : ServerIdOf(aFormId);
    const auto identity = serverId ? m_identities.find(serverId) : m_identities.end();
    return identity != m_identities.end() && identity->second.Traits.Temper >= 0 ? &identity->second.Traits : nullptr;
}

NemesisService::Fight* NemesisService::FightOf(uint32_t aFormId, bool aCreate) noexcept
{
    auto it = m_fights.find(aFormId);
    if (it != m_fights.end())
        return &it->second;
    if (!aCreate || m_fights.size() >= kMaxFights)
        return nullptr;

    Actor* pActor = ActorOf(aFormId);
    if (!pActor || pActor->IsDead() || !NemesisGame::IsPerson(pActor))
        return nullptr;

    Fight fight;
    fight.FormId = aFormId;
    fight.RecordId = RecordOf(aFormId);
    fight.Gang = NemesisGame::GangOf(pActor, m_winterOfAtom);
    // KARMA's hunters: the lawful come for the wicked, the killers for hire for the good
    const bool hunter = NemesisGame::IsKarmaHunter(pActor);
    if (hunter)
        fight.Gang = m_alignment <= 1 ? "marshals" : "blackledger";
    if (fight.Gang.empty() && !fight.RecordId)
        return nullptr;
    fight.Body = NemesisGame::BodyOf(pActor);
    fight.FirstAt = Now();
    fight.CombatAt = fight.FirstAt;
    fight.Position = pActor->position;
    fight.World = NemesisGame::WorldKeyOf(pActor);
    fight.Limbs = NemesisGame::LimbConditions(pActor);
    fight.LimbsKnown = true;

    Fight& created = m_fights[aFormId] = fight;
    if (hunter && !created.RecordId)
        SendDeed(RequestNemesis::kMet, created);
    return &created;
}

void NemesisService::OnDamage(const ActorDamageEvent& acEvent) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !m_transport.IsConnected() || !m_settings.Enabled || !m_helloSent)
        return;
    const double now = Now();

    if (acEvent.HitterId == pPlayer->formID)
    {
        if (Fight* pFight = FightOf(acEvent.HitteeId, true))
        {
            pFight->PlayerHitAt = now;
            pFight->CombatAt = now;
            pFight->Damage += acEvent.Damage;
            // The wound log: what hit him and from how far, sent at the next look with the limb it took
            pFight->PendingDamage += acEvent.Damage;
            if (pFight->PendingHits < 1000)
                ++pFight->PendingHits;
            if (pFight->PendingWeapon.empty())
            {
                pFight->PendingWeapon = NemesisGame::WeaponName(pPlayer);
                pFight->PendingAmmo = NemesisGame::AmmoName(pPlayer);
                pFight->PendingCause = NemesisGame::CauseOf(pPlayer);
            }
            if (Actor* pHittee = ActorOf(acEvent.HitteeId))
                pFight->PendingRange = DistanceBetween(pHittee->position, pPlayer->position);
            if (m_encounter)
                m_encounter->Engaged = true;
        }
    }
    else if (acEvent.HitteeId == pPlayer->formID)
    {
        if (Fight* pFight = FightOf(acEvent.HitterId, true))
        {
            pFight->HitPlayerAt = now;
            pFight->CombatAt = now;
            m_lastAttacker = acEvent.HitterId;
            m_lastAttackerAt = now;
            if (m_encounter)
                m_encounter->Engaged = true;
        }
    }
}

void NemesisService::CheckFights() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    const double now = Now();

    for (auto it = m_fights.begin(); it != m_fights.end();)
    {
        Fight& fight = it->second;
        Actor* pActor = ActorOf(fight.FormId);
        bool over = false;

        // The hits since the last look, before a death is reported (the server marks the one that killed him)
        if (pActor && fight.PendingHits)
            SendWound(fight, pActor);

        if (!pActor)
        {
            // Gone (unloaded and deleted): he lived through it
            if (!fight.Questioned)
                ResolveLived(fight);
            over = true;
        }
        else if (pActor->IsDead())
        {
            ResolveDeath(fight, pActor);
            over = true;
        }
        else
        {
            fight.Position = pActor->position;
            fight.World = NemesisGame::WorldKeyOf(pActor);
            const float health = NemesisGame::HealthPercent(pActor);
            fight.LowestHealth = std::min(fight.LowestHealth, health);
            fight.Crippled |= NemesisGame::CrippledLimbs(pActor);
            if (pActor->IsInCombat())
                fight.CombatAt = now;

            // Near death, a man who isn't brave gives up to the player who did it (the game running him decides)
            if (!fight.Surrendered && !fight.SurrenderRolled && m_settings.Surrender && health < 0.2f && fight.PlayerHitAt >= 0.0 &&
                pActor->GetExtension()->IsLocal())
            {
                fight.SurrenderRolled = true;
                int chance = fight.Body == 4 ? 4 : (fight.Gang == "atom" || fight.Gang == "atomites") ? 10 : 35;
                if (fight.RecordId)
                    chance = std::min(chance, 25);
                if (RandomPercent() < chance)
                    Surrender(fight, pActor);
            }

            const float distance = DistanceBetween(pActor->position, pPlayer->position);
            if (fight.Surrendered)
                over = distance > kFightRange;
            else if (now - fight.CombatAt > kFightTimeout || distance > kFightRange)
            {
                ResolveLived(fight);
                over = true;
            }
        }

        if (over)
        {
            // Nobody questioned him, or he ran: his hands down and his temper back (a man of the world's, kept in saves,
            // isn't left so)
            if ((fight.Surrendered && !fight.Questioned) || fight.Fled)
                if (pActor && !pActor->IsDead() && pActor->GetExtension()->IsLocal())
                    NemesisGame::Release(pActor);
            {
                std::scoped_lock lock(s_surrenderedLock);
                s_surrendered.erase(fight.FormId);
            }
            if (m_questioning == fight.FormId)
                CloseCard();
            it = m_fights.erase(it);
        }
        else
            ++it;
    }
}

void NemesisService::ResolveLived(Fight& aFight) noexcept
{
    // Only fights the local player was in
    if (aFight.PlayerHitAt < 0.0 && aFight.HitPlayerAt < 0.0)
        return;
    if (aFight.Damage > 0.f)
        SendDeed(RequestNemesis::kHit, aFight, static_cast<int>(aFight.Damage));

    if ((aFight.Crippled & 0xF) && aFight.PlayerHitAt >= 0.0)
        SendDeed(RequestNemesis::kCrippled, aFight, (aFight.Crippled & 3) ? 1 : 2);
    else if (aFight.LowestHealth <= 0.5f && aFight.PlayerHitAt >= 0.0)
        SendDeed(RequestNemesis::kWounded, aFight);
    else
        SendDeed(RequestNemesis::kGunfight, aFight);
}

void NemesisService::ResolveDeath(Fight& aFight, Actor* apActor) noexcept
{
    const double now = Now();
    const bool mine = aFight.PlayerHitAt >= 0.0 && now - aFight.PlayerHitAt <= kKillWindow;
    // Another player's kill is their game's to report; a nemesis nobody killed, the game running him reports
    if (!mine && !(aFight.RecordId && apActor->GetExtension()->IsLocal()))
        return;

    int32_t flags = mine ? RequestNemesis::kByLocalPlayer : 0;
    const uint8_t limbs = aFight.Crippled | NemesisGame::CrippledLimbs(apActor);
    if (limbs & 1)
        flags |= RequestNemesis::kLeftLeg;
    if (limbs & 2)
        flags |= RequestNemesis::kRightLeg;
    if (limbs & 4)
        flags |= RequestNemesis::kLeftArm;
    if (limbs & 8)
        flags |= RequestNemesis::kRightArm;
    if (limbs & 16)
        flags |= RequestNemesis::kHead;
    if (aFight.Surrendered)
        flags |= RequestNemesis::kWhileSurrendered | RequestNemesis::kFinal;
    SendDeath(aFight, flags, mine ? NemesisGame::CauseOf(PlayerCharacter::Get()) : static_cast<uint8_t>(RequestNemesis::kOther));

    // His gang mates who were in it saw it
    if (!mine || aFight.Gang.empty())
        return;
    int witnesses = 0;
    for (auto& [formId, other] : m_fights)
    {
        if (formId == aFight.FormId || other.Gang != aFight.Gang || other.Surrendered || DistanceBetween(other.Position, aFight.Position) > kWitnessRange)
            continue;
        Actor* pOther = ActorOf(formId);
        if (!pOther || pOther->IsDead())
            continue;
        SendDeed(RequestNemesis::kComradeKilled, other);
        if (++witnesses >= 2)
            break;
    }
}

void NemesisService::Surrender(Fight& aFight, Actor* apActor, bool aHeldUp) noexcept
{
    // Held up with the game's Intimidation perk, his hands are already up: only Interrogate is added
    if (aHeldUp)
        NemesisGame::MarkCaptive(apActor);
    else
        NemesisGame::Surrender(apActor);
    aFight.Surrendered = true;
    aFight.SurrenderRolled = true;
    {
        std::scoped_lock lock(s_surrenderedLock);
        s_surrendered.insert(aFight.FormId);
    }
    SendDeed(RequestNemesis::kSurrendered, aFight);
    NemesisGame::ShowHud(fmt::format("{} gives up. Keep your weapon on {} to interrogate {}.", NameOf(apActor), HimOf(aFight.Body), HimOf(aFight.Body)));
}

void NemesisService::CheckHeldUp() noexcept
{
    // A gang man held up with the game's Intimidation perk can be questioned too, without a fight first
    ProcessLists* pLists = ProcessLists::Get();
    if (!pLists || !m_settings.Surrender)
        return;
    for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
    {
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(pLists->highActorHandleArray[i]));
        if (!pActor || pActor->IsDead() || !pActor->GetExtension()->IsLocal() || !NemesisGame::IsHeldUp(pActor))
            continue;
        if (auto it = m_fights.find(pActor->formID); it != m_fights.end() && (it->second.Surrendered || it->second.Questioned))
            continue;
        if (Fight* pFight = FightOf(pActor->formID, true))
            Surrender(*pFight, pActor, true);
    }
}

void NemesisService::SendWound(Fight& aFight, Actor* apActor) noexcept
{
    // The limb whose condition dropped most since the last look took the hits; none: the body (or it was already gone)
    const std::array<float, 6> limbs = NemesisGame::LimbConditions(apActor);
    uint8_t part = RequestNemesis::kPartUnknown;
    float biggest = 0.f;
    for (size_t i = 0; aFight.LimbsKnown && i < limbs.size(); ++i)
    {
        const float drop = aFight.Limbs[i] - limbs[i];
        if (drop > biggest)
        {
            biggest = drop;
            part = static_cast<uint8_t>(i);
            if (limbs[i] <= 0.f && aFight.Limbs[i] > 0.f)
                part |= RequestNemesis::kPartCrippled;
            else
                part &= ~RequestNemesis::kPartCrippled;
        }
    }
    aFight.Limbs = limbs;
    aFight.LimbsKnown = true;

    RequestNemesis request;
    request.Action = RequestNemesis::kWound;
    request.Kind = part;
    request.Body = aFight.PendingCause;
    request.Value = static_cast<int32_t>(aFight.PendingDamage + 0.5f);
    request.Count = aFight.PendingHits;
    request.Range = aFight.PendingRange;
    request.Text = aFight.PendingWeapon.c_str();
    request.Text2 = aFight.PendingAmmo.c_str();
    request.RecordId = aFight.RecordId ? aFight.RecordId : RecordOf(aFight.FormId);
    request.ActorServerId = ServerIdOf(aFight.FormId);
    request.World = aFight.World.c_str();
    request.Position = {apActor->position.x, apActor->position.y, apActor->position.z};
    if (request.RecordId || request.ActorServerId)
        SendRequest(request);

    aFight.PendingDamage = 0.f;
    aFight.PendingHits = 0;
    aFight.PendingWeapon.clear();
    aFight.PendingAmmo.clear();
}

void NemesisService::CheckGunpoint() noexcept
{
    // The nearest surrendered man the player's weapon is on, within talking distance
    constexpr float kGunpointRange = 1500.f;
    auto* pPlayer = PlayerCharacter::Get();
    uint32_t aimed = 0;
    float nearest = kGunpointRange;
    for (auto& [formId, fight] : m_fights)
    {
        if (!fight.Surrendered || fight.Questioned)
            continue;
        Actor* pActor = ActorOf(formId);
        if (!pActor || pActor->IsDead() || !NemesisGame::AimedAt(pActor, kGunpointRange))
            continue;
        const float distance = DistanceBetween(pActor->position, pPlayer->position);
        if (distance <= nearest)
            nearest = distance, aimed = formId;
    }
    if (aimed == m_gunpoint)
        return;
    m_gunpoint = aimed;
    // The key that questions him down the sights, read each time (the player may have rebound it)
    if (aimed)
    {
        const int key = NemesisGame::ActivateKey();
        m_activateKey = key ? key : 'E';
    }
    // His card has the bar while it's open
    if (m_questioning)
        return;
    if (Fight* pFight = FightOf(aimed, false))
    {
        RequestNemesis request;
        request.Action = RequestNemesis::kUi;
        request.Text = "gunpoint";
        request.Text2 = fmt::format("{}:{}", pFight->RecordId ? pFight->RecordId : RecordOf(aimed), ServerIdOf(aimed)).c_str();
        SendRequest(request);
    }
    else
        World::Get().ctx().at<OverlayService>().SendEvent("nemesisGunpoint", "null");
}

void NemesisService::CheckScreenKey() noexcept
{
    // The Pip-Boy's OPEN FULL SCREEN entry, pressed (RD_Nemeses.esp's script)
    bool wanted = NemesisPipboy::TakeOpenRequest();

    // The key, while the game has the keyboard (not over another window, not typing in the overlay's chat)
    const bool down = GameHasKeyboard() && (GetAsyncKeyState(m_screenKey) & 0x8000) != 0;
    const bool pressed = down && !m_screenKeyDown;
    m_screenKeyDown = down;
    if (pressed && !m_screenOpen && !World::Get().ctx().at<OverlayService>().GetActive())
        wanted = true;

    if (!wanted || m_screenOpen || !m_settings.Enabled || !m_transport.IsConnected() || !m_helloSent)
        return;
    if (NemesisGame::IsPipboyOpen())
    {
        NemesisGame::ClosePipboy();
        m_screenWanted = true;
        return;
    }
    if (NemesisGame::IsBusy())
        return;
    OpenScreen();
}

void NemesisService::CheckInterrogateRequest() noexcept
{
    // Interrogate chosen on the crosshair (RD_Nemeses.esp's perk fragment): the captive the weapon is on, or the nearest
    // one in front of it when the last look hasn't caught up yet
    if (!NemesisGame::TakeInterrogateRequest() || m_questioning || m_screenOpen)
        return;
    uint32_t target = m_gunpoint;
    if (!target)
    {
        auto* pPlayer = PlayerCharacter::Get();
        float nearest = 1500.f;
        for (auto& [formId, fight] : m_fights)
        {
            Actor* pActor = fight.Surrendered && !fight.Questioned ? ActorOf(formId) : nullptr;
            if (!pActor || pActor->IsDead() || !NemesisGame::AimedAt(pActor, nearest))
                continue;
            nearest = DistanceBetween(pActor->position, pPlayer->position);
            target = formId;
        }
    }
    if (target)
        OpenCard(target);
}

void NemesisService::CheckQuestionKey() noexcept
{
    // Down the sights the game activates nothing (its activate handler drops the press), so at gunpoint Activate is
    // read here; from the hip the game's activation (OnActivate) opens the card
    const bool down = m_gunpoint && GameHasKeyboard() && (GetAsyncKeyState(m_activateKey) & 0x8000) != 0;
    const bool pressed = down && !m_questionKeyDown;
    m_questionKeyDown = down;
    if (pressed && !m_questioning && !m_screenOpen && NemesisGame::AimingDownSights() && !World::Get().ctx().at<OverlayService>().GetActive())
        OpenCard(m_gunpoint);
}

void NemesisService::OpenScreen() noexcept
{
    if (m_questioning)
        CloseCard();
    m_screenOpen = true;
    ShowOverlayCursor(true);
    RequestNemesis request;
    request.Action = RequestNemesis::kUi;
    request.Text = "screen";
    request.Text2 = "record";
    SendRequest(request);
}

void NemesisService::CloseScreen() noexcept
{
    m_screenOpen = false;
    // Closed with its key, which the overlay saw first: it counts again once it's let go, or it would reopen the screen
    m_screenKeyDown = true;
    World::Get().ctx().at<OverlayService>().SendEvent("nemesisScreen", "null");
    ShowOverlayCursor(false);
}

// --- what goes to the server

void NemesisService::SendDeed(uint8_t aDeed, const Fight& acFight, int aAmount) noexcept
{
    RequestNemesis request;
    request.Action = RequestNemesis::kDeed;
    request.Kind = aDeed;
    request.Value = aAmount;
    request.RecordId = acFight.RecordId ? acFight.RecordId : RecordOf(acFight.FormId);
    request.ActorServerId = ServerIdOf(acFight.FormId);
    request.Body = acFight.Body;
    request.Text = acFight.Gang.c_str();
    request.World = acFight.World.c_str();
    request.Position = {acFight.Position.x, acFight.Position.y, acFight.Position.z};
    request.Text2 = NemesisGame::PlaceName().c_str();

    // A stranger who may become a nemesis brings his face along
    if (m_settings.KeepFaces && !request.RecordId && aDeed != RequestNemesis::kHit && aDeed != RequestNemesis::kMet)
    {
        std::string face;
        uint32_t flags = 0;
        if (NemesisGame::CaptureFace(ActorOf(acFight.FormId), face, flags))
        {
            request.Appearance = String(face.data(), face.size());
            request.AppearanceFlags = flags;
        }
    }
    SendRequest(request);
}

void NemesisService::SendDeath(const Fight& acFight, int32_t aFlags, uint8_t aCause) noexcept
{
    RequestNemesis request;
    request.Action = RequestNemesis::kDeath;
    request.Kind = aCause;
    request.Value = aFlags;
    request.RecordId = acFight.RecordId ? acFight.RecordId : RecordOf(acFight.FormId);
    request.ActorServerId = ServerIdOf(acFight.FormId);
    request.Body = acFight.Body;
    request.Text = acFight.Gang.c_str();
    request.World = acFight.World.c_str();
    request.Position = {acFight.Position.x, acFight.Position.y, acFight.Position.z};
    request.Text2 = NemesisGame::PlaceName().c_str();
    SendRequest(request);
}

// --- names

void NemesisService::CheckIdentities() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    const double now = Now();
    for (auto it = m_identities.begin(); it != m_identities.end();)
    {
        Identity& identity = it->second;
        Actor* pActor = Utils::GetByServerId<Actor>(it->first);
        if (!pActor)
        {
            // Not here yet (or anymore): kept a while for when he comes into this game
            it = now - identity.Since > 1800.0 ? m_identities.erase(it) : std::next(it);
            continue;
        }
        identity.Since = now;
        NemesisGame::SetName(pActor, identity.Name);

        // The player sees his face: he's known to them now
        if (!identity.Met && !pActor->IsDead() && DistanceBetween(pActor->position, pPlayer->position) < kMeetRange)
        {
            identity.Met = true;
            RequestNemesis request;
            request.Action = RequestNemesis::kDeed;
            request.Kind = RequestNemesis::kMet;
            request.RecordId = identity.RecordId;
            request.ActorServerId = it->first;
            SendRequest(request);
        }
        ++it;
    }
}

void NemesisService::CheckPipboy() noexcept
{
    static const BSFixedString s_pipboyMenu("PipboyMenu");
    UI* pUi = UI::Get();
    const bool open = pUi && pUi->GetMenuOpen(s_pipboyMenu);
    const double now = Now();
    const bool opened = open && !m_pipboyOpen;
    m_pipboyOpen = open;

    if (!opened && now - m_pagesAt < kPagesInterval && (!m_pagesWantedAt || now < m_pagesWantedAt))
        return;
    m_pagesAt = now;
    m_pagesWantedAt = 0.0;
    RequestNemesis request;
    request.Action = RequestNemesis::kUi;
    request.Text = "pipboy";
    SendRequest(request);
}

// --- encounters

void NemesisService::PlaceEncounter() noexcept
{
    Encounter& encounter = *m_encounter;
    auto* pPlayer = PlayerCharacter::Get();
    NiPoint3 spot{};
    if (NemesisGame::IsBusy() || !pPlayer->GetWorldSpace() || !NemesisGame::FindSpot(encounter.Plan.Distance, spot))
    {
        EndEncounter(RequestNemesis::kCancelled);
        return;
    }

    for (size_t i = 0; i < encounter.Plan.Men.size(); ++i)
    {
        const NemesisMan& man = encounter.Plan.Men[i];
        NiPoint3 at = spot;
        if (i)
        {
            at.x += RandomOffset(400.f);
            at.y += RandomOffset(400.f);
            if (!NemesisGame::GroundAt(at))
                at = spot;
        }
        Actor* pActor = NemesisGame::Spawn(NemesisGame::SpawnBase(encounter.Plan.Gang.c_str(), man.Rank, man.Legendary), at);
        if (!pActor)
            continue;
        if (!man.Name.empty())
            NemesisGame::SetName(pActor, man.Name.c_str());
        NemesisGame::ApplyRank(pActor, man.Rank, man.Injuries);
        NemesisGame::SetAlignment(pActor, man.Alignment);
        if (m_settings.KeepFaces && !man.Appearance.empty())
            NemesisGame::ApplyFace(pActor, std::string(man.Appearance.c_str(), man.Appearance.size()), man.AppearanceFlags);
        encounter.Men.push_back({man, pActor->formID, 0});
    }

    if (encounter.Men.empty())
    {
        EndEncounter(RequestNemesis::kCancelled);
        return;
    }

    const double now = Now();
    encounter.SpawnedAt = now;
    const std::string leader = encounter.Plan.Men.empty() ? std::string() : std::string(encounter.Plan.Men[0].Name.c_str());
    switch (encounter.Plan.Kind)
    {
    case NemesisSpawn::kSighting:
        NemesisGame::ShowHud(leader.empty() ? "Someone is watching you." : fmt::format("You catch sight of {} in the distance.", leader));
        break;
    case NemesisSpawn::kCallOut:
        // He says his piece first
        encounter.AttackAt = now + 6.0;
        break;
    default:
        encounter.AttackAt = now + 1.5;
        break;
    }
    if (m_settings.ShowTaunts && !encounter.Plan.Taunt.empty() && encounter.Plan.Kind != NemesisSpawn::kSighting)
        NemesisGame::ShowHud(leader.empty() ? std::string(encounter.Plan.Taunt.c_str()) : fmt::format("{}: \"{}\"", leader, encounter.Plan.Taunt.c_str()));

    spdlog::info("Nemeses: encounter {} placed, kind {}, {} men of {} ({})", encounter.Id, encounter.Plan.Kind, encounter.Men.size(), encounter.Plan.Gang.c_str(), leader);
}

void NemesisService::RunEncounter() noexcept
{
    if (!m_encounter)
        return;
    Encounter& encounter = *m_encounter;
    const double now = Now();

    if (!encounter.SpawnedAt)
    {
        if (now >= encounter.SpawnAt)
            PlaceEncounter();
        return;
    }

    // Their server ids once the character sync has them; their faces kept the first time
    if (!encounter.Reported)
    {
        bool all = true;
        for (EncounterMan& man : encounter.Men)
        {
            if (!man.ServerId)
                man.ServerId = ServerIdOf(man.FormId);
            all &= man.ServerId != 0;
        }
        if (all || now - encounter.SpawnedAt > kServerIdWait)
        {
            encounter.Reported = true;
            RequestNemesis spawned;
            spawned.Action = RequestNemesis::kSpawned;
            spawned.RecordId = encounter.Id;
            for (const EncounterMan& man : encounter.Men)
                spawned.ServerIds.push_back(man.ServerId);
            SendRequest(spawned);

            for (const EncounterMan& man : encounter.Men)
            {
                std::string face;
                uint32_t flags = 0;
                if (m_settings.KeepFaces && man.Plan.RecordId && man.Plan.Appearance.empty() && NemesisGame::CaptureFace(ActorOf(man.FormId), face, flags))
                {
                    RequestNemesis look;
                    look.Action = RequestNemesis::kLook;
                    look.RecordId = man.Plan.RecordId;
                    look.Appearance = String(face.data(), face.size());
                    look.AppearanceFlags = flags;
                    SendRequest(look);
                }
            }
        }
    }

    auto* pPlayer = PlayerCharacter::Get();
    if (encounter.AttackAt && now >= encounter.AttackAt)
    {
        encounter.AttackAt = 0.0;
        for (const EncounterMan& man : encounter.Men)
            NemesisGame::Attack(ActorOf(man.FormId), pPlayer);
    }

    // Over: all of them dead or gone, the leader gave up, they're far, or it's been long
    bool anyAlive = false;
    float nearest = std::numeric_limits<float>::max();
    for (const EncounterMan& man : encounter.Men)
    {
        Actor* pActor = ActorOf(man.FormId);
        if (!pActor || pActor->IsDead())
            continue;
        anyAlive = true;
        nearest = std::min(nearest, DistanceBetween(pActor->position, pPlayer->position));
    }
    const Fight* pLeader = encounter.Men.empty() ? nullptr : FightOf(encounter.Men[0].FormId, false);

    if (!anyAlive)
        EndEncounter(RequestNemesis::kHeKilled);
    else if (pLeader && pLeader->Surrendered)
        EndEncounter(RequestNemesis::kHeSurrendered);
    else if (nearest > kEncounterRange)
        EndEncounter(encounter.Engaged ? RequestNemesis::kHeFled : RequestNemesis::kYouLeft);
    else if (now - encounter.SpawnedAt > kEncounterLength)
        EndEncounter(RequestNemesis::kHeFled);
}

void NemesisService::EndEncounter(uint8_t aOutcome) noexcept
{
    if (!m_encounter)
        return;

    RequestNemesis request;
    request.Action = RequestNemesis::kOutcome;
    request.RecordId = m_encounter->Id;
    request.Kind = aOutcome;
    SendRequest(request);

    // They stay while the player is near (their bodies to search, the living to finish), and go once far
    for (const EncounterMan& man : m_encounter->Men)
        m_leftovers.push_back(man.FormId);
    ClearLeftovers(false);
    spdlog::info("Nemeses: encounter {} over (outcome {})", m_encounter->Id, aOutcome);
    m_encounter.reset();
}

void NemesisService::ClearLeftovers(bool aAll) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    m_leftovers.erase(std::remove_if(m_leftovers.begin(), m_leftovers.end(),
                                     [pPlayer, aAll](uint32_t aFormId)
                                     {
                                         Actor* pActor = ActorOf(aFormId);
                                         // Only what was spawned for an encounter: temporary characters
                                         if (!pActor || !pActor->IsTemporary())
                                             return true;
                                         if (!aAll && pPlayer && DistanceBetween(pActor->position, pPlayer->position) < kFightRange)
                                             return false;
                                         pActor->Delete();
                                         return true;
                                     }),
                      m_leftovers.end());
}

// --- the server's messages

void NemesisService::OnNotify(const NotifyNemesis& acMessage) noexcept
{
    switch (acMessage.Action)
    {
    case NotifyNemesis::kSpawn:
    {
        if (m_encounter || !m_settings.Enabled)
        {
            RequestNemesis request;
            request.Action = RequestNemesis::kOutcome;
            request.RecordId = acMessage.Spawn.EncounterId;
            request.Kind = RequestNemesis::kCancelled;
            SendRequest(request);
            break;
        }
        Encounter encounter;
        encounter.Id = acMessage.Spawn.EncounterId;
        encounter.Plan = acMessage.Spawn;
        encounter.SpawnAt = Now() + 0.5;
        m_encounter = std::move(encounter);
        break;
    }
    case NotifyNemesis::kIdentity:
    {
        Identity& identity = m_identities[acMessage.ActorServerId];
        const bool met = identity.Met && identity.RecordId == acMessage.RecordId;
        identity.RecordId = acMessage.RecordId;
        identity.Name = acMessage.Text.c_str();
        identity.Rank = acMessage.Kind;
        identity.Traits = NotifyNemesis::UnpackIdentity(acMessage.Value);
        identity.Met = met;
        identity.Since = Now();
        if (Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActorServerId))
        {
            NemesisGame::SetName(pActor, identity.Name);
            if (Fight* pFight = FightOf(pActor->formID, false))
                pFight->RecordId = acMessage.RecordId;
        }
        break;
    }
    case NotifyNemesis::kNotice:
        NemesisGame::ShowHud(acMessage.Text.c_str());
        // The Pip-Boy's pages follow
        if (!m_pagesWantedAt)
            m_pagesWantedAt = Now() + kPagesAfterNews;
        break;
    case NotifyNemesis::kUi:
        if (acMessage.Text2 == "nemesesPipboy")
            NemesisPipboy::SetPages(std::string(acMessage.Text.c_str(), acMessage.Text.size()));
        // An answer that comes after the player looked away, or closed the screen, stays unshown
        else if ((acMessage.Text2 == "nemesisGunpoint" && (!m_gunpoint || m_questioning)) || (acMessage.Text2 == "nemesisScreen" && !m_screenOpen))
            break;
        else
            World::Get().ctx().at<OverlayService>().SendEvent(acMessage.Text2.c_str(), acMessage.Text.c_str());
        break;
    case NotifyNemesis::kOrder:
    {
        // The game running him acts
        Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActorServerId);
        if (!pActor || pActor->IsDead() || !pActor->GetExtension()->IsLocal())
            break;
        switch (acMessage.Kind)
        {
        case NotifyNemesis::kSurrender:
            if (Fight* pFight = FightOf(pActor->formID, true); pFight && !pFight->Surrendered)
                Surrender(*pFight, pActor);
            break;
        case NotifyNemesis::kLetGo:
        case NotifyNemesis::kLetGoFight:
        {
            // Spared: hands down; he runs, or (the server's roll, a third of them) turns on the player again
            const bool fights = acMessage.Kind == NotifyNemesis::kLetGoFight;
            NemesisGame::LetGo(pActor, fights);
            {
                std::scoped_lock lock(s_surrenderedLock);
                s_surrendered.erase(pActor->formID);
            }
            if (Fight* pFight = FightOf(pActor->formID, false))
            {
                pFight->Surrendered = false;
                pFight->Questioned = true;
                pFight->Fled = !fights;
            }
            break;
        }
        case NotifyNemesis::kFlee:
        case NotifyNemesis::kCalm:
            NemesisGame::Calm(pActor);
            {
                std::scoped_lock lock(s_surrenderedLock);
                s_surrendered.erase(pActor->formID);
            }
            if (Fight* pFight = FightOf(pActor->formID, false))
            {
                pFight->Surrendered = false;
                pFight->Questioned = true;
            }
            break;
        case NotifyNemesis::kAttack:
            if (static_cast<uint32_t>(acMessage.Value) == m_playerId)
                NemesisGame::Attack(pActor, PlayerCharacter::Get());
            break;
        default:
            break;
        }
        break;
    }
    case NotifyNemesis::kKarma:
        NemesisGame::ModKarma(acMessage.Value, acMessage.Kind == 1);
        break;
    case NotifyNemesis::kEndEncounter:
        if (m_encounter && m_encounter->Id == acMessage.RecordId)
        {
            spdlog::info("Nemeses: the server ended encounter {}", m_encounter->Id);
            for (const EncounterMan& man : m_encounter->Men)
                m_leftovers.push_back(man.FormId);
            m_encounter.reset();
        }
        break;
    default:
        break;
    }
}

// --- questioning and the overlay

bool NemesisService::OnActivate(TESObjectREFR* apObject, Actor* apActivator) noexcept
{
    if (!apObject || !apActivator || apActivator != PlayerCharacter::Get())
        return false;
    if (auto* pActor = Cast<Actor>(apObject); !pActor || pActor->IsDead())
        return false;
    const uint32_t formId = apObject->formID;
    {
        std::scoped_lock lock(s_surrenderedLock);
        if (!s_surrendered.count(formId))
            return false;
    }
    // Questioning is at gunpoint: the weapon out and on him
    if (!NemesisGame::AimedAt(Cast<Actor>(apObject), 1500.f))
    {
        const Fight* pFight = FightOf(formId, false);
        const char* pHim = HimOf(pFight ? pFight->Body : NemesisGame::BodyOf(Cast<Actor>(apObject)));
        NemesisGame::ShowHud(fmt::format("Hold your weapon on {} to make {} talk.", pHim, pHim));
        return true;
    }

    // His card opens instead of the game's talk
    m_world.GetRunner().Queue([this, formId]() { OpenCard(formId); });
    return true;
}

void NemesisService::OpenCard(uint32_t aFormId) noexcept
{
    Fight* pFight = FightOf(aFormId, false);
    if (!pFight || !pFight->Surrendered || m_questioning)
        return;
    m_questioning = aFormId;
    RequestNemesis request;
    request.Action = RequestNemesis::kQuestion;
    request.Kind = RequestNemesis::kOpen;
    request.RecordId = pFight->RecordId ? pFight->RecordId : RecordOf(aFormId);
    request.ActorServerId = ServerIdOf(aFormId);
    SendRequest(request);
    ShowOverlayCursor(true);
}

void NemesisService::Question(uint8_t aQuestion, int aCaps) noexcept
{
    Fight* pFight = FightOf(m_questioning, false);
    Actor* pActor = ActorOf(m_questioning);
    if (!pFight || !pActor || pActor->IsDead())
    {
        CloseCard();
        return;
    }

    RequestNemesis request;
    request.Action = RequestNemesis::kQuestion;
    request.Kind = aQuestion;
    request.RecordId = pFight->RecordId ? pFight->RecordId : RecordOf(m_questioning);
    request.ActorServerId = ServerIdOf(m_questioning);

    switch (aQuestion)
    {
    case RequestNemesis::kFinishHim:
        // Executed: the death is reported as such when it's seen (a surrendered man, the player's kill)
        pFight->Questioned = true;
        pFight->PlayerHitAt = Now();
        SendDeed(RequestNemesis::kExecuted, *pFight);
        NemesisGame::Kill(pActor, PlayerCharacter::Get());
        CloseCard();
        return;
    case RequestNemesis::kPay:
        if (!NemesisGame::PayCaps(aCaps))
        {
            NemesisGame::ShowHud("You don't have the caps.");
            return;
        }
        request.Value = aCaps;
        break;
    case RequestNemesis::kLetHimGo:
        // The server rolls whether he runs or turns on the player, and orders the game running him (kLetGo, kLetGoFight)
        pFight->Questioned = true;
        {
            std::scoped_lock lock(s_surrenderedLock);
            s_surrendered.erase(m_questioning);
        }
        break;
    case RequestNemesis::kThreaten:
        // A threat lands more often with more Charisma
        request.Value = NemesisGame::PlayerCharisma();
        break;
    case RequestNemesis::kClose:
        CloseCard();
        return;
    default:
        break;
    }
    SendRequest(request);
}

void NemesisService::CloseCard() noexcept
{
    m_questioning = 0;
    // The bar comes back at the next look if the gun is still on him
    m_gunpoint = 0;
    ShowOverlayCursor(false);
}

void NemesisService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    if (acRequest == "question")
    {
        const auto colon = acPayload.find(':');
        const int question = std::atoi(acPayload.substr(0, colon).c_str());
        const int caps = colon == std::string::npos ? 0 : std::atoi(acPayload.c_str() + colon + 1);
        if (question >= 0 && question <= RequestNemesis::kOpen)
            Question(static_cast<uint8_t>(question), std::max(0, caps));
        return;
    }
    if (acRequest == "close")
    {
        CloseCard();
        return;
    }
    if (acRequest == "closeScreen")
    {
        if (m_screenOpen)
            CloseScreen();
        return;
    }

    // The nemesis screen's pages ("record", "gang:<id>", "man:<id>", "world", "wounds"), the server's JSON back as the
    // nemesisScreen event; and the host's "reset"
    if ((acRequest == "screen" && m_screenOpen) || acRequest == "reset")
    {
        RequestNemesis request;
        request.Action = RequestNemesis::kUi;
        request.Text = acRequest.c_str();
        request.Text2 = acPayload.c_str();
        SendRequest(request);
    }
}

#endif

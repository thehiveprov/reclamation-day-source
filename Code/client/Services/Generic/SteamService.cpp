#include <Services/SteamService.h>

#if TP_FALLOUT4

#include <World.h>
#include <BuildInfo.h>
#include <PlayerCharacter.h>
#include <Events/OverlayRestartedEvent.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <Games/Fallout4/SteamApi.h>
#include <Games/Fallout4/SocialPipboy.h>
#include <Games/Fallout4/NemesisGame.h>
#include <Games/Fallout4/MiniJson.h>
#include <Games/Fallout4/Interface/MainMenuIntegration.h>

#include <Client.hpp>

namespace
{
using namespace std::chrono_literals;

// The friends looked at again this often (Steam's callbacks say sooner when one changes)
constexpr auto kRefreshInterval = 3s;
// An invite holds for this long
constexpr auto kInviteLifetime = 30min;
// The rich presence keys friends' games read (docs/STEAM_SOCIAL.md)
constexpr char kStatusKey[] = "rd_status";
constexpr char kWorldKey[] = "rd_world";
constexpr char kPrivacyKey[] = "rd_privacy";
constexpr char kInvitesKey[] = "rd_invites";
constexpr char kVersionKey[] = "rd_version";
constexpr char kHostKey[] = "rd_host";
// What Steam's own Join Game and invites pass on to the friend's game ("+rd_join <SteamID>")
constexpr char kJoinCommand[] = "+rd_join";

// PersonaStateChange_t, AvatarImageLoaded_t, FriendRichPresenceUpdate_t, GameRichPresenceJoinRequested_t (pack 8)
struct PersonaStateChange
{
    uint64_t SteamId;
    int ChangeFlags;
};
struct AvatarImageLoaded
{
    uint64_t SteamId;
    int Image;
    int Wide;
    int Tall;
};
struct FriendRichPresenceUpdate
{
    uint64_t SteamId;
    uint32_t AppId;
};
struct GameRichPresenceJoinRequested
{
    uint64_t SteamId;
    char Connect[256];
};
static_assert(sizeof(GameRichPresenceJoinRequested) == 264);

// The game's window is in front (the keys aren't going to another window)
bool GameHasKeyboard() noexcept
{
    HWND window = GetForegroundWindow();
    DWORD processId = 0;
    if (window)
        GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

bool IsInWorld() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && pPlayer->parentCell;
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

const char* StateName(int aState) noexcept
{
    switch (static_cast<SteamApi::Persona>(aState))
    {
    case SteamApi::Persona::kOffline:
    case SteamApi::Persona::kInvisible: return "offline";
    case SteamApi::Persona::kBusy: return "busy";
    case SteamApi::Persona::kAway: return "away";
    case SteamApi::Persona::kSnooze: return "snooze";
    default: return "online";
    }
}

std::string Base64(const std::vector<uint8_t>& acData) noexcept
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((acData.size() + 2) / 3 * 4);
    for (size_t i = 0; i < acData.size(); i += 3)
    {
        const uint32_t chunk = acData[i] << 16 | (i + 1 < acData.size() ? acData[i + 1] << 8 : 0) | (i + 2 < acData.size() ? acData[i + 2] : 0);
        out += kAlphabet[chunk >> 18 & 63];
        out += kAlphabet[chunk >> 12 & 63];
        out += i + 1 < acData.size() ? kAlphabet[chunk >> 6 & 63] : '=';
        out += i + 2 < acData.size() ? kAlphabet[chunk & 63] : '=';
    }
    return out;
}

// An avatar's RGBA pixels as a "data:image/bmp;base64," URL (24-bit, top-down: avatars have no transparency)
std::string AvatarUrl(uint32_t aWidth, uint32_t aHeight, const std::vector<uint8_t>& acPixels) noexcept
{
    const uint32_t rowSize = (aWidth * 3 + 3) & ~3u;
    std::vector<uint8_t> bmp(54 + static_cast<size_t>(rowSize) * aHeight);
    const auto put32 = [&bmp](size_t aOffset, uint32_t aValue) { std::memcpy(&bmp[aOffset], &aValue, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<uint32_t>(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, aWidth);
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(aHeight)));
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, static_cast<uint32_t>(bmp.size() - 54));
    for (uint32_t y = 0; y < aHeight; ++y)
    {
        for (uint32_t x = 0; x < aWidth; ++x)
        {
            const uint8_t* pIn = &acPixels[(static_cast<size_t>(y) * aWidth + x) * 4];
            uint8_t* pOut = &bmp[54 + static_cast<size_t>(y) * rowSize + x * 3];
            pOut[0] = pIn[2];
            pOut[1] = pIn[1];
            pOut[2] = pIn[0];
        }
    }
    return "data:image/bmp;base64," + Base64(bmp);
}

// A hosted world's privacy as the Pip-Boy says it, and what it means (the sidebar's own words)
const char* PrivacyLabel(SteamService::Privacy aPrivacy) noexcept
{
    switch (aPrivacy)
    {
    case SteamService::Privacy::kInviteOnly: return "Invite only";
    case SteamService::Privacy::kPrivate: return "Private";
    default: return "Friends";
    }
}

const char* PrivacyHint(SteamService::Privacy aPrivacy) noexcept
{
    switch (aPrivacy)
    {
    case SteamService::Privacy::kInviteOnly: return "Only friends you invite can join.";
    case SteamService::Privacy::kPrivate: return "No one joins through Steam. Joining by address still works.";
    default: return "Steam friends can ask to join. You let each one in.";
    }
}

bool IsOffline(int aState) noexcept
{
    const auto state = static_cast<SteamApi::Persona>(aState);
    return state == SteamApi::Persona::kOffline || state == SteamApi::Persona::kInvisible;
}

// A SteamID from the overlay (sent as text: 64 bits don't fit a JavaScript number)
uint64_t ParseId(const std::string& acText) noexcept
{
    return std::strtoull(acText.c_str(), nullptr, 10);
}

// The host's SteamID in "+rd_join <SteamID>"
uint64_t ParseJoin(const std::string& acConnect) noexcept
{
    const size_t at = acConnect.find(kJoinCommand);
    if (at == std::string::npos)
        return 0;
    return std::strtoull(acConnect.c_str() + at + std::size(kJoinCommand) - 1, nullptr, 10);
}
} // namespace

SteamService::SteamService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_overlayRestartedConnection = aDispatcher.sink<OverlayRestartedEvent>().connect<&SteamService::OnOverlayRestarted>(this);
    LoadKeys();

    // Steam's own callbacks (run by the game's SteamAPI_RunCallbacks, on its main thread)
    SteamApi::Listen(SteamApi::kPersonaStateChange, sizeof(PersonaStateChange), [this](const void*) { m_dirty = true; });
    SteamApi::Listen(SteamApi::kFriendRichPresenceUpdate, sizeof(FriendRichPresenceUpdate), [this](const void*) { m_dirty = true; });
    SteamApi::Listen(SteamApi::kAvatarImageLoaded, sizeof(AvatarImageLoaded),
                     [this](const void* apData)
                     {
                         const auto* pLoaded = static_cast<const AvatarImageLoaded*>(apData);
                         if (m_avatarsWanted.contains(pLoaded->SteamId))
                             SendAvatar(pLoaded->SteamId);
                     });
    SteamApi::Listen(SteamApi::kGameRichPresenceJoinRequested, sizeof(GameRichPresenceJoinRequested),
                     [this](const void* apData)
                     {
                         const auto* pRequest = static_cast<const GameRichPresenceJoinRequested*>(apData);
                         OnJoinRequested(std::string(pRequest->Connect, strnlen(pRequest->Connect, sizeof(pRequest->Connect))));
                     });
}

SteamService::~SteamService() noexcept
{
    m_gateway.Stop();
}

// config/Social.ini: [Keys] Sidebar = a letter, a digit, F1-F12 or a virtual key code ("0x50"); P by default
void SteamService::LoadKeys() noexcept
{
    const auto path = TiltedPhoques::GetPath() / "config" / "Social.ini";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path);
        file << "; Reclamation Day's social sidebar (Steam friends: join, invite): the key that opens it in the world.\n"
                "; A letter, a digit, F1-F12 or a virtual key code such as 0x50. P is free in Fallout 4's own controls.\n"
                "[Keys]\nSidebar=P\n";
    }

    wchar_t text[32]{};
    GetPrivateProfileStringW(L"Keys", L"Sidebar", L"P", text, static_cast<DWORD>(std::size(text)), path.c_str());
    std::wstring value = text;
    value.erase(std::remove_if(value.begin(), value.end(), iswspace), value.end());
    std::transform(value.begin(), value.end(), value.begin(), towupper);

    int key = 0;
    if (value.size() == 1 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'0' && value[0] <= L'9')))
        key = value[0];
    else if (value.size() >= 2 && value[0] == L'F' && iswdigit(value[1]))
    {
        const int number = std::stoi(value.substr(1));
        if (number >= 1 && number <= 12)
            key = VK_F1 + number - 1;
    }
    else if (value.rfind(L"0X", 0) == 0)
        key = static_cast<int>(std::wcstoul(value.c_str() + 2, nullptr, 16));
    if (key <= 0 || key > 0xFE)
        key = 'P';
    m_sidebarKey = key;
    spdlog::info("Social: key 0x{:X} opens the social sidebar (config/Social.ini)", key);
}

void SteamService::QueueUiRequest(std::string aRequest, std::string aPayload) noexcept
{
    std::scoped_lock lock(m_uiLock);
    m_uiRequests.emplace_back(std::move(aRequest), std::move(aPayload));
}

void SteamService::OnMainLoop() noexcept
{
    std::vector<std::pair<std::string, std::string>> requests;
    {
        std::scoped_lock lock(m_uiLock);
        requests.swap(m_uiRequests);
    }

    if (!m_ready)
    {
        if (!SteamApi::Init())
        {
            // the overlay told there's no Steam (a copy of the game without it)
            if (!requests.empty())
                World::Get().ctx().at<OverlayService>().SendEvent("social", R"({"available":false,"friends":[],"requests":[],"joined":[],"players":[]})");
            // the Pip-Boy's tab says so (and lists the players here)
            if (const auto now = std::chrono::steady_clock::now(); now - m_lastRefresh > kRefreshInterval)
            {
                m_lastRefresh = now;
                UpdatePipboy();
            }
            CheckPipboy();
            return;
        }
        m_ready = true;
        m_me = SteamApi::MySteamId();
        TiltedPhoques::Client::SetPeerToPeerInterface(SteamApi::Sockets());
        m_presenceDirty = true;
    }

    for (const auto& [request, payload] : requests)
        OnUiRequest(request, payload);

    m_gateway.Update();

    // someone new asking in: the host's told where to answer
    const std::vector<uint64_t> waiting = m_gateway.Waiting();
    for (const uint64_t id : waiting)
    {
        if (std::find(m_waitingShown.begin(), m_waitingShown.end(), id) == m_waitingShown.end())
            Toast(NameOf(id) + " wants to join your world. Press " + std::string(1, static_cast<char>(m_sidebarKey)) + " to answer.");
    }
    if (waiting != m_waitingShown)
    {
        m_waitingShown = waiting;
        m_dirty = true;
    }
    // invites used or run out
    const auto now = std::chrono::steady_clock::now();
    for (const uint64_t id : m_gateway.Joined())
    {
        if (m_invited.erase(id))
            m_presenceDirty = true;
    }
    for (auto it = m_invited.begin(); it != m_invited.end();)
    {
        if (now - it->second > kInviteLifetime)
        {
            it = m_invited.erase(it);
            m_presenceDirty = true;
        }
        else
            ++it;
    }

    CheckKey();
    CheckPipboy();

    if (m_dirty || now - m_lastRefresh > kRefreshInterval)
    {
        m_dirty = false;
        m_lastRefresh = now;
        RefreshFriends();
        SendState();
        // (again each time: a loaded save puts the entries' globals back as it had them)
        UpdatePipboy();
    }
    if (m_presenceDirty)
    {
        m_presenceDirty = false;
        UpdatePresence();
    }
}

void SteamService::CheckKey() noexcept
{
    const bool down = GameHasKeyboard() && (GetAsyncKeyState(m_sidebarKey) & 0x8000) != 0;
    const bool pressed = down && !m_keyDown;
    m_keyDown = down;
    if (!pressed)
        return;
    if (m_sidebarOpen)
        CloseSidebar();
    else if (IsInWorld() && !NemesisGame::IsBusy() && !World::Get().ctx().at<OverlayService>().GetActive())
        OpenSidebar();
}

void SteamService::OpenSidebar() noexcept
{
    m_sidebarOpen = true;
    ShowOverlayCursor(true);
    m_lastState.clear();
    SendState();
    World::Get().ctx().at<OverlayService>().SendEvent("socialSidebar", "true");
}

void SteamService::CloseSidebar() noexcept
{
    if (!m_sidebarOpen)
        return;
    m_sidebarOpen = false;
    World::Get().ctx().at<OverlayService>().SendEvent("socialSidebar", "false");
    ShowOverlayCursor(false);
}

void SteamService::RefreshFriends() noexcept
{
    const std::string me = std::to_string(m_me);
    std::vector<Friend> friends;
    for (const uint64_t id : SteamApi::Friends())
    {
        Friend entry;
        entry.Id = id;
        entry.Name = SteamApi::FriendName(id);
        entry.State = static_cast<int>(SteamApi::FriendState(id));
        entry.InFallout4 = SteamApi::FriendAppId(id) == SteamApi::kFallout4AppId;
        if (entry.InFallout4)
        {
            entry.Status = SteamApi::FriendPresence(id, kStatusKey);
            entry.WorldName = SteamApi::FriendPresence(id, kWorldKey);
            entry.Privacy = SteamApi::FriendPresence(id, kPrivacyKey);
            entry.Version = SteamApi::FriendPresence(id, kVersionKey);
            const std::string invites = "," + SteamApi::FriendPresence(id, kInvitesKey) + ",";
            entry.InvitedMe = invites.find("," + me + ",") != std::string::npos;
        }
        friends.push_back(std::move(entry));
    }

    // in Reclamation Day first (hosting, then playing), then in Fallout 4, online, away, offline; by name
    const auto rank = [](const Friend& acFriend)
    {
        if (acFriend.Status == "hosting")
            return 0;
        if (!acFriend.Status.empty())
            return 1;
        if (acFriend.InFallout4)
            return 2;
        switch (static_cast<SteamApi::Persona>(acFriend.State))
        {
        case SteamApi::Persona::kOffline:
        case SteamApi::Persona::kInvisible: return 5;
        case SteamApi::Persona::kAway:
        case SteamApi::Persona::kSnooze: return 4;
        default: return 3;
        }
    };
    std::stable_sort(friends.begin(), friends.end(),
                     [&rank](const Friend& acA, const Friend& acB)
                     {
                         const int a = rank(acA), b = rank(acB);
                         if (a != b)
                             return a < b;
                         return _stricmp(acA.Name.c_str(), acB.Name.c_str()) < 0;
                     });

    // a new invite: said once
    std::set<uint64_t> inviting;
    for (const Friend& entry : friends)
    {
        if (!entry.InvitedMe || entry.Status != "hosting")
            continue;
        inviting.insert(entry.Id);
        if (!m_invitesSeen.contains(entry.Id))
            Toast(entry.Name + " invited you to " + (entry.WorldName.empty() ? "their world" : entry.WorldName) + ". Press " +
                  std::string(1, static_cast<char>(m_sidebarKey)) + " to join.");
    }
    m_invitesSeen = std::move(inviting);
    m_friends = std::move(friends);
}

void SteamService::UpdatePresence() noexcept
{
    if (!m_ready)
        return;

    SteamApi::SetPresence(kVersionKey, BUILD_COMMIT);
    if (!m_hostedWorldId.empty())
    {
        std::string invites;
        for (const auto& [id, when] : m_invited)
            invites += (invites.empty() ? "" : ",") + std::to_string(id);
        SteamApi::SetPresence(kStatusKey, "hosting");
        SteamApi::SetPresence(kWorldKey, m_hostedName.c_str());
        SteamApi::SetPresence(kPrivacyKey, PrivacyName(m_privacy));
        SteamApi::SetPresence(kInvitesKey, invites.c_str());
        SteamApi::SetPresence(kHostKey, "");
        // Steam's own Join Game while the world's open to friends
        const std::string connect = m_privacy == Privacy::kFriends ? fmt::format("{} {}", kJoinCommand, m_me) : std::string();
        SteamApi::SetPresence("connect", connect.c_str());
        SteamApi::SetPresence("status", ("Reclamation Day: hosting " + m_hostedName).c_str());
    }
    else if (!m_playingName.empty())
    {
        SteamApi::SetPresence(kStatusKey, "playing");
        SteamApi::SetPresence(kWorldKey, m_playingName.c_str());
        SteamApi::SetPresence(kPrivacyKey, "");
        SteamApi::SetPresence(kInvitesKey, "");
        SteamApi::SetPresence(kHostKey, m_playingHost ? std::to_string(m_playingHost).c_str() : "");
        SteamApi::SetPresence("connect", "");
        SteamApi::SetPresence("status", ("Reclamation Day: playing in " + m_playingName).c_str());
    }
    else
    {
        SteamApi::SetPresence(kStatusKey, "menu");
        SteamApi::SetPresence(kWorldKey, "");
        SteamApi::SetPresence(kPrivacyKey, "");
        SteamApi::SetPresence(kInvitesKey, "");
        SteamApi::SetPresence(kHostKey, "");
        SteamApi::SetPresence("connect", "");
        SteamApi::SetPresence("status", "Reclamation Day: main menu");
    }
}

void SteamService::SendState() noexcept
{
    std::string friends;
    for (const Friend& entry : m_friends)
    {
        const bool versionOk = VersionOk(entry);
        const bool canJoin = CanJoin(entry);
        const std::string game = !entry.Status.empty() ? "rd" : entry.InFallout4 ? "fallout4" : "";
        friends += fmt::format(R"({}{{"id":"{}","name":{},"state":"{}","game":"{}","status":{},"world":{},"privacy":{},"invitedMe":{},"versionOk":{},"canJoin":{},"invited":{}}})",
                               friends.empty() ? "" : ",", entry.Id, MiniJson::Quote(entry.Name), StateName(entry.State), game, MiniJson::Quote(entry.Status),
                               MiniJson::Quote(entry.WorldName), MiniJson::Quote(entry.Privacy), entry.InvitedMe ? "true" : "false", versionOk ? "true" : "false",
                               canJoin ? "true" : "false", m_invited.contains(entry.Id) ? "true" : "false");
    }
    std::string requests;
    for (const uint64_t id : m_gateway.Waiting())
        requests += fmt::format(R"({}{{"id":"{}","name":{}}})", requests.empty() ? "" : ",", id, MiniJson::Quote(NameOf(id)));
    std::string joined;
    for (const uint64_t id : m_gateway.Joined())
        joined += fmt::format(R"({}"{}")", joined.empty() ? "" : ",", id);
    std::string players;
    for (const Player& player : PlayersHere())
        players += fmt::format(R"({}{{"id":{},"name":{},"party":"{}"}})", players.empty() ? "" : ",", player.Id, MiniJson::Quote(player.Name), player.Party);

    const std::string hosting = m_hostedWorldId.empty() ? "null" : fmt::format(R"({{"world":{},"privacy":"{}"}})", MiniJson::Quote(m_hostedName), PrivacyName(m_privacy));
    const std::string playing = m_playingName.empty() ? "null" : fmt::format(R"({{"world":{},"host":"{}"}})", MiniJson::Quote(m_playingName), m_playingHost);
    const std::string state = fmt::format(
        R"({{"available":{},"peerToPeer":{},"me":{{"id":"{}","name":{}}},"hosting":{},"playing":{},"inWorld":{},"key":"{}","friends":[{}],"requests":[{}],"joined":[{}],"players":[{}]}})",
        m_ready ? "true" : "false", SteamApi::Sockets() ? "true" : "false", m_me, MiniJson::Quote(SteamApi::MyName()), hosting, playing, IsInWorld() ? "true" : "false",
        std::string(1, static_cast<char>(m_sidebarKey)), friends, requests, joined, players);
    if (state == m_lastState)
        return;
    m_lastState = state;
    World::Get().ctx().at<OverlayService>().SendEvent("social", state);
}

void SteamService::SendAvatar(uint64_t aSteamId) noexcept
{
    const int image = SteamApi::FriendAvatar(aSteamId);
    // (-1: Steam is still loading it, AvatarImageLoaded says when it's there)
    if (image == -1)
    {
        m_avatarsWanted.insert(aSteamId);
        return;
    }
    m_avatarsWanted.erase(aSteamId);
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
    if (!SteamApi::ImageRgba(image, width, height, pixels))
        return;
    m_avatarsSent.insert(aSteamId);
    World::Get().ctx().at<OverlayService>().SendEvent("socialAvatar", fmt::format(R"({{"id":"{}","url":"{}"}})", aSteamId, AvatarUrl(width, height, pixels)));
}

void SteamService::Toast(const std::string& acText) noexcept
{
    World::Get().ctx().at<OverlayService>().SendEvent("socialToast", MiniJson::Quote(acText));
}

void SteamService::OnOverlayRestarted(const OverlayRestartedEvent&) noexcept
{
    // The friends, their pictures (it asks for those as it gets the friends), and the sidebar if it was open
    m_lastState.clear();
    m_avatarsSent.clear();
    m_dirty = true;
    if (m_sidebarOpen)
        World::Get().ctx().at<OverlayService>().SendEvent("socialSidebar", "true");
}

void SteamService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    const uint64_t id = ParseId(MiniJson::GetString(acPayload, "id"));
    if (acRequest == "refresh")
    {
        m_lastState.clear();
        m_dirty = true;
    }
    else if (acRequest == "avatars")
    {
        // "id,id,..."
        std::stringstream ids(MiniJson::GetString(acPayload, "ids"));
        for (std::string item; std::getline(ids, item, ',');)
        {
            const uint64_t avatarId = ParseId(item);
            if (avatarId && !m_avatarsSent.contains(avatarId))
                SendAvatar(avatarId);
        }
    }
    else if (acRequest == "join" && id)
    {
        const Friend* pFriend = FindFriend(id);
        Join(id, pFriend ? pFriend->WorldName : std::string());
    }
    else if (acRequest == "invite" && id)
        Invite(id);
    else if (acRequest == "answer" && id)
    {
        m_gateway.Answer(id, MiniJson::GetString(acPayload, "accept") == "1");
        m_dirty = true;
    }
    else if (acRequest == "privacy")
        SetPrivacy(ParsePrivacy(MiniJson::GetString(acPayload, "value")));
    else if (acRequest == "profile" && id)
        SteamApi::OpenOverlayTo("steamid", id);
    else if (acRequest == "close")
        CloseSidebar();
}

PeerToPeerGateway::Decision SteamService::Decide(uint64_t aSteamId) noexcept
{
    using Decision = PeerToPeerGateway::Decision;
    if (m_hostedWorldId.empty() || m_privacy == Privacy::kPrivate)
        return Decision::kRefuse;
    if (const auto it = m_invited.find(aSteamId); it != m_invited.end() && std::chrono::steady_clock::now() - it->second < kInviteLifetime)
        return Decision::kAccept;
    if (m_privacy == Privacy::kInviteOnly || !SteamApi::IsFriend(aSteamId))
        return Decision::kRefuse;
    return Decision::kAsk;
}

void SteamService::Join(uint64_t aHostSteamId, const std::string& acWorldName) noexcept
{
    if (aHostSteamId == m_me)
        return;
    spdlog::info("Social: joining {}'s world \"{}\" through Steam", aHostSteamId, acWorldName);
    CloseSidebar();
    JoinSteamWorld(aHostSteamId, acWorldName.empty() ? NameOf(aHostSteamId) + "'s world" : acWorldName);
}

void SteamService::Invite(uint64_t aSteamId) noexcept
{
    if (m_hostedWorldId.empty())
    {
        Toast("Only the host can invite friends to this world.");
        return;
    }
    m_invited[aSteamId] = std::chrono::steady_clock::now();
    m_presenceDirty = true;
    m_dirty = true;
    const bool sent = SteamApi::InviteToGame(aSteamId, fmt::format("{} {}", kJoinCommand, m_me));
    spdlog::info("Social: invited {} to \"{}\" (Steam's invite {})", aSteamId, m_hostedName, sent ? "sent" : "not sent");
    Toast("Invited " + NameOf(aSteamId) + ".");
}

void SteamService::SetPrivacy(Privacy aPrivacy) noexcept
{
    if (m_hostedWorldId.empty() || aPrivacy == m_privacy)
        return;
    m_privacy = aPrivacy;
    SetHostedWorldPrivacy(m_hostedWorldId, PrivacyName(aPrivacy));
    if (aPrivacy != Privacy::kFriends)
        m_gateway.RefuseWaiting();
    m_presenceDirty = true;
    m_dirty = true;
    spdlog::info("Social: world \"{}\" is now {}", m_hostedName, PrivacyName(aPrivacy));
}

void SteamService::OnJoinRequested(const std::string& acConnect) noexcept
{
    const uint64_t host = ParseJoin(acConnect);
    spdlog::info("Social: Steam asked to join \"{}\"", acConnect);
    if (!host)
        return;
    // in a world the sidebar asks first (leaving it saves and goes back to the main menu)
    if (IsInWorld())
    {
        // (over the Pip-Boy or another menu: once it's closed)
        if (NemesisGame::IsBusy())
        {
            m_askJoin = host;
            m_sidebarWanted = true;
            if (NemesisGame::IsPipboyOpen())
                NemesisGame::ClosePipboy();
            return;
        }
        AskToJoin(host);
        return;
    }
    const Friend* pFriend = FindFriend(host);
    Join(host, pFriend ? pFriend->WorldName : std::string());
}

void SteamService::AskToJoin(uint64_t aHostSteamId) noexcept
{
    const Friend* pFriend = FindFriend(aHostSteamId);
    if (!m_sidebarOpen)
        OpenSidebar();
    World::Get().ctx().at<OverlayService>().SendEvent("socialJoinAsk", fmt::format(R"({{"id":"{}","name":{},"world":{}}})", aHostSteamId, MiniJson::Quote(NameOf(aHostSteamId)),
                                                                                  MiniJson::Quote(pFriend ? pFriend->WorldName : std::string())));
}

void SteamService::StartHosting(const std::string& acWorldId, const std::string& acName, Privacy aPrivacy, uint16_t aPort) noexcept
{
    m_hostedWorldId = acWorldId;
    m_hostedName = acName;
    m_privacy = aPrivacy;
    m_serverPort = aPort;
    m_playingName.clear();
    m_playingHost = 0;
    m_invited.clear();
    m_presenceDirty = true;
    m_dirty = true;
    if (!m_ready)
        SteamApi::Init();
    if (!m_gateway.Start(aPort, [this](uint64_t aSteamId) { return Decide(aSteamId); }))
        spdlog::warn("Social: hosting \"{}\" without Steam: friends join by address only", acName);
}

void SteamService::StopHosting() noexcept
{
    if (m_hostedWorldId.empty())
        return;
    m_gateway.Stop();
    m_hostedWorldId.clear();
    m_hostedName.clear();
    m_invited.clear();
    m_presenceDirty = true;
    m_dirty = true;
}

void SteamService::SetPlaying(const std::string& acWorldName, uint64_t aHostSteamId) noexcept
{
    if (m_playingName == acWorldName && m_playingHost == aHostSteamId)
        return;
    m_playingName = acWorldName;
    m_playingHost = aHostSteamId;
    m_presenceDirty = true;
    m_dirty = true;
}

SteamService::Privacy SteamService::ParsePrivacy(const std::string& acName) noexcept
{
    if (acName == "invite")
        return Privacy::kInviteOnly;
    if (acName == "private")
        return Privacy::kPrivate;
    return Privacy::kFriends;
}

const char* SteamService::PrivacyName(Privacy aPrivacy) noexcept
{
    switch (aPrivacy)
    {
    case Privacy::kInviteOnly: return "invite";
    case Privacy::kPrivate: return "private";
    default: return "friends";
    }
}

std::string SteamService::StatusText(const Friend& acFriend) noexcept
{
    // (ASCII only: the Pip-Boy's font)
    const std::string version = VersionOk(acFriend) ? "" : ", a different version";
    if (!acFriend.Status.empty())
    {
        const std::string world = acFriend.WorldName.empty() ? "a world" : acFriend.WorldName;
        if (acFriend.Status == "hosting")
        {
            if (acFriend.InvitedMe)
                return "Invited you to " + world + version;
            const char* pDoor = acFriend.Privacy == "invite" ? ", invite only" : acFriend.Privacy == "private" ? ", private" : "";
            return "Hosting " + world + pDoor + version;
        }
        if (acFriend.Status == "playing")
            return "Playing in " + world + version;
        return "In the main menu" + version;
    }
    if (acFriend.InFallout4)
        return "Playing Fallout 4";
    switch (static_cast<SteamApi::Persona>(acFriend.State))
    {
    case SteamApi::Persona::kOffline:
    case SteamApi::Persona::kInvisible: return "Offline";
    case SteamApi::Persona::kBusy: return "Busy";
    case SteamApi::Persona::kAway: return "Away";
    case SteamApi::Persona::kSnooze: return "Snooze";
    default: return "Online";
    }
}

bool SteamService::VersionOk(const Friend& acFriend) noexcept
{
    return acFriend.Version.empty() || acFriend.Version == BUILD_COMMIT;
}

bool SteamService::CanJoin(const Friend& acFriend) noexcept
{
    return acFriend.InFallout4 && acFriend.Status == "hosting" && VersionOk(acFriend) && (acFriend.Privacy == "friends" || acFriend.InvitedMe);
}

std::vector<SteamService::Player> SteamService::PlayersHere() const noexcept
{
    std::vector<Player> players;
    // (the list stays as it was after leaving a server)
    if (!m_transport.IsOnline())
        return players;

    const auto& party = m_world.ctx().at<PartyService>();
    const auto& members = party.GetPartyMembers();
    for (const auto& [id, name] : party.GetPlayers())
    {
        Player player;
        player.Id = id;
        player.Name = std::string(name.c_str());
        if (party.IsInParty())
        {
            if (party.GetLeaderPlayerId() == id)
                player.Party = "leader";
            else if (std::find(members.begin(), members.end(), id) != members.end())
                player.Party = "member";
        }
        players.push_back(std::move(player));
    }

    // the party first (its leader at the top), then by name
    const auto rank = [](const Player& acPlayer) { return *acPlayer.Party == 'l' ? 0 : *acPlayer.Party == 'm' ? 1 : 2; };
    std::stable_sort(players.begin(), players.end(),
                     [&rank](const Player& acA, const Player& acB)
                     {
                         const int a = rank(acA), b = rank(acB);
                         if (a != b)
                             return a < b;
                         return _stricmp(acA.Name.c_str(), acB.Name.c_str()) < 0;
                     });
    return players;
}

void SteamService::UpdatePipboy() noexcept
{
    if (!IsInWorld())
        return;

    const std::string key(1, static_cast<char>(m_sidebarKey));
    std::vector<SocialPipboy::Page> pages;
    std::vector<PipboyEntry> entries;
    const auto add = [&pages, &entries](std::string aName, std::string aText, bool aDot, PipboyAction aAction = PipboyAction::kNone, uint64_t aId = 0)
    {
        pages.push_back({std::move(aName), std::move(aText), aDot});
        entries.push_back({aAction, aId});
    };

    if (!m_ready)
        add("STEAM", "!Steam\n~Steam isn't running for the game.\n\n Friends can't join or be invited through it. Joining a world by its address (the main "
                     "menu's MULTIPLAYER) still works.",
            false);

    // The world: hosted (its door for friends, who's asking in) or joined
    const std::vector<uint64_t> waiting = m_gateway.Waiting();
    if (!m_hostedWorldId.empty())
    {
        std::string text = "!" + m_hostedName + "\n#Your world\n^Who can join\x1D" + PrivacyLabel(m_privacy) + "\n~" + PrivacyHint(m_privacy) + "\n";
        if (!waiting.empty())
            text += "^Asking to join\x1D" + std::to_string(waiting.size()) + "\n";
        if (!SteamApi::Sockets())
            text += "~Steam's networking isn't there: friends join by address.\n";
        text += "\n Press to open the social sidebar (" + key + "): who can join, and the friends asking to join.";
        add("MY WORLD", std::move(text), !waiting.empty(), PipboyAction::kSidebar);
        for (const uint64_t id : waiting)
            add(NameOf(id) + " asks to join",
                "!" + NameOf(id) + "\n~Wants to join your world.\n\n Press to answer in the social sidebar (" + key + "): let them in or refuse. They wait up to 50 seconds.",
                true, PipboyAction::kSidebar, id);
    }
    else if (!m_playingName.empty())
    {
        std::string text = "!" + m_playingName + "\n#This world\n";
        text += m_playingHost ? "^Host\x1D" + NameOf(m_playingHost) + "\n~Joined through Steam.\n" : std::string("~Joined by address.\n");
        text += "\n~Only its host can invite friends here.";
        add("THIS WORLD", std::move(text), false);
    }

    // The players here, the party first
    const std::vector<Player> players = PlayersHere();
    if (!players.empty())
    {
        const auto inParty = std::count_if(players.begin(), players.end(), [](const Player& acPlayer) { return *acPlayer.Party != '\0'; });
        std::string text = "!In this world\n^Players\x1D" + std::to_string(players.size()) + "\n";
        if (inParty)
            text += "^In your party\x1D" + std::to_string(inParty) + "\n";
        text += "\n~Everyone playing in this world with you. The overlay's player list makes and runs parties.";
        add("IN THIS WORLD", std::move(text), false);
        for (const Player& player : players)
        {
            const bool leader = *player.Party == 'l';
            const bool member = *player.Party == 'm';
            const std::string role = leader ? "Your party's leader" : member ? "In your party" : "Not in your party";
            add(player.Name + (leader ? " (party leader)" : member ? " (party)" : ""), "!" + player.Name + "\n#In this world\n~" + role + "\n", false);
        }
    }

    // The Steam friends online (the sidebar has the offline ones too)
    if (m_ready)
    {
        size_t online = 0, inGame = 0;
        for (const Friend& entry : m_friends)
        {
            if (!entry.Status.empty() || entry.InFallout4 || !IsOffline(entry.State))
                ++online;
            if (!entry.Status.empty())
                ++inGame;
        }
        std::string text = "!Steam friends\n^Online\x1D" + std::to_string(online) + "\n^In Reclamation Day\x1D" + std::to_string(inGame) + "\n";
        if (online < m_friends.size())
            text += "~" + std::to_string(m_friends.size() - online) + " offline friends aren't listed here: the social sidebar (" + key + ") has them.\n";
        text += !m_hostedWorldId.empty() ? "\n Press a friend to invite them to your world." : "\n Press a friend hosting a world to join it.";
        add("STEAM FRIENDS", std::move(text), false);

        size_t left = online;
        for (const Friend& entry : m_friends)
        {
            if (entry.Status.empty() && !entry.InFallout4 && IsOffline(entry.State))
                continue;
            // (one entry kept for the rest)
            if (pages.size() + 1 >= SocialPipboy::kPages && left > 1)
                break;
            --left;

            std::string text = "!" + entry.Name + "\n~" + StatusText(entry) + "\n";
            if (entry.Status == "hosting" && !entry.WorldName.empty())
                text += "^World\x1D" + entry.WorldName + "\n";
            text += "\n";
            PipboyAction action = PipboyAction::kNone;
            if (CanJoin(entry))
            {
                text += " Press to join " + (entry.WorldName.empty() ? std::string("their world") : entry.WorldName) + ".";
                text += "\n~You leave this world first: your game is saved.";
                action = PipboyAction::kJoin;
            }
            else if (!m_hostedWorldId.empty())
            {
                text += m_invited.contains(entry.Id) ? "~Invited: they can join for 30 minutes. Press to invite them again." : " Press to invite them to " + m_hostedName + ".";
                action = PipboyAction::kInvite;
            }
            else if (entry.Status == "hosting")
                text += !VersionOk(entry)             ? "~Their game is a different version of Reclamation Day: the same one is needed to join."
                        : entry.Privacy == "private" ? "~Their world is private."
                                                     : "~Their world is invite only: they can invite you.";
            else if (!entry.Status.empty())
                text += "~Not hosting a world.";
            else if (entry.InFallout4)
                text += "~Playing Fallout 4 without Reclamation Day.";
            add(entry.Name, std::move(text), entry.InvitedMe && CanJoin(entry), action, entry.Id);
        }
        if (left)
            add("MORE FRIENDS", "!More friends\n~" + std::to_string(left) + " more friends online.\n\n Press to open the social sidebar (" + key + "): it lists them all.",
                false, PipboyAction::kSidebar);
    }

    if (!SocialPipboy::SetPages(pages))
        return;
    m_pipboyEntries = std::move(entries);
    if (!std::exchange(m_pipboyFilled, true))
        spdlog::info("Social: the Pip-Boy's SOCIAL tab filled ({} entries)", pages.size());
}

void SteamService::CheckPipboy() noexcept
{
    if (!IsInWorld())
    {
        m_sidebarWanted = false;
        m_askJoin = 0;
        return;
    }

    const int pressed = SocialPipboy::TakePressed();
    if (pressed >= 0 && pressed < static_cast<int>(m_pipboyEntries.size()))
    {
        const PipboyEntry entry = m_pipboyEntries[pressed];
        spdlog::info("Social: the Pip-Boy's SOCIAL entry {} pressed (action {})", pressed, static_cast<int>(entry.Action));
        switch (entry.Action)
        {
        case PipboyAction::kInvite: Invite(entry.Id); break;
        case PipboyAction::kJoin: m_askJoin = entry.Id; [[fallthrough]];
        case PipboyAction::kSidebar:
            // the sidebar over the world, the Pip-Boy put away first
            m_sidebarWanted = true;
            if (NemesisGame::IsPipboyOpen())
                NemesisGame::ClosePipboy();
            break;
        default: break;
        }
    }

    if (m_sidebarWanted && !NemesisGame::IsBusy())
    {
        m_sidebarWanted = false;
        if (const uint64_t host = std::exchange(m_askJoin, 0))
            AskToJoin(host);
        else if (!m_sidebarOpen)
            OpenSidebar();
    }
}

const SteamService::Friend* SteamService::FindFriend(uint64_t aSteamId) const noexcept
{
    const auto it = std::find_if(m_friends.begin(), m_friends.end(), [aSteamId](const Friend& acFriend) { return acFriend.Id == aSteamId; });
    return it != m_friends.end() ? &*it : nullptr;
}

std::string SteamService::NameOf(uint64_t aSteamId) const noexcept
{
    if (const Friend* pFriend = FindFriend(aSteamId))
        return pFriend->Name;
    std::string name = SteamApi::FriendName(aSteamId);
    return name.empty() ? std::to_string(aSteamId) : name;
}

#endif

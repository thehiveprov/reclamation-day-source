#pragma once

#if TP_FALLOUT4

#include <Games/Fallout4/PeerToPeerGateway.h>

struct World;
struct TransportService;
struct OverlayRestartedEvent;

/**
 * @brief Steam friends (Fallout 4, see docs/STEAM_SOCIAL.md): the social sidebar's friends, this player's rich presence,
 * invites, and joining a friend's world peer to peer through Steam, beside joining by address (an additional way in:
 * everything here adds to joining by address, nothing replaces it).
 *
 * The host's game lets friends in (PeerToPeerGateway): while it hosts a world, a friend joining through Steam is let in
 * when the host invited them, asked about in the sidebar when the world's privacy is Friends, and turned down when it's
 * Invite only or Private (the host picks per world, and approves each request to join).
 *
 * What friends see of this player (their rich presence): rd_status (menu, hosting, playing), rd_world (its name),
 * rd_privacy, rd_invites (the SteamIDs this host invited, comma separated: an invited friend's game shows the invite),
 * rd_version (the build: joining needs the same), and "connect" (+rd_join <SteamID>, Steam's own Join Game button) while
 * hosting a world open to friends.
 *
 * The sidebar opens with its key (config/Social.ini, P by default) in the world, and from the main menu's and the pause
 * menu's Social buttons (the overlay's own). The Pip-Boy's DATA > SOCIAL tab (RD_Social.esp, SocialPipboy) lists the
 * same in the world: the hosted world, friends asking to join, the players here and the party, the Steam friends;
 * pressing a friend invites them or joins them.
 */
class SteamService
{
public:
    enum class Privacy : uint8_t
    {
        kFriends,
        kInviteOnly,
        kPrivate,
    };

    SteamService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~SteamService() noexcept;

    TP_NOCOPYMOVE(SteamService);

    // Every frame from the game's main loop, the main menu included (HivemindApp::OnMainLoop)
    void OnMainLoop() noexcept;
    // The overlay: social(request, payload), see services/social.service.ts. From the overlay's thread, run on the next
    // main loop frame (the world's runner doesn't run at the main menu)
    void QueueUiRequest(std::string aRequest, std::string aPayload) noexcept;

    // The world this game hosts, its server just started on this port (MainMenuIntegration), and when it stops
    void StartHosting(const std::string& acWorldId, const std::string& acName, Privacy aPrivacy, uint16_t aPort) noexcept;
    void StopHosting() noexcept;
    // A world this game joined (its name; the host's SteamID when joined through Steam), or none (the main menu)
    void SetPlaying(const std::string& acWorldName, uint64_t aHostSteamId) noexcept;

    [[nodiscard]] static Privacy ParsePrivacy(const std::string& acName) noexcept;
    [[nodiscard]] static const char* PrivacyName(Privacy aPrivacy) noexcept;

private:
    struct Friend
    {
        uint64_t Id{0};
        std::string Name;
        int State{0};
        bool InFallout4{false};
        // their Reclamation Day presence
        std::string Status;
        std::string WorldName;
        std::string Privacy;
        std::string Version;
        bool InvitedMe{false};
        int Avatar{0};
    };

    // What pressing an entry of the Pip-Boy's SOCIAL tab does
    enum class PipboyAction : uint8_t
    {
        kNone,
        // the social sidebar (the hosted world's privacy, a friend asking to join)
        kSidebar,
        kInvite,
        kJoin,
    };
    struct PipboyEntry
    {
        PipboyAction Action{PipboyAction::kNone};
        uint64_t Id{0};
    };

    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;
    void OnOverlayRestarted(const OverlayRestartedEvent&) noexcept;
    void LoadKeys() noexcept;
    void CheckKey() noexcept;
    void OpenSidebar() noexcept;
    void CloseSidebar() noexcept;

    void RefreshFriends() noexcept;
    void UpdatePresence() noexcept;
    void SendState() noexcept;
    void SendAvatar(uint64_t aSteamId) noexcept;
    void Toast(const std::string& acText) noexcept;

    // The Pip-Boy's DATA > SOCIAL tab (SocialPipboy): its entries made again from what the sidebar shows (in a world)
    void UpdatePipboy() noexcept;
    // An entry of it pressed, and the sidebar waiting for the Pip-Boy to close
    void CheckPipboy() noexcept;

    // The players in this world (the server's list, not this one), each with their part in this player's party
    struct Player
    {
        uint32_t Id{0};
        std::string Name;
        // "leader", "member" or ""
        const char* Party{""};
    };
    [[nodiscard]] std::vector<Player> PlayersHere() const noexcept;

    // A friend as the sidebar and the Pip-Boy say it ("Hosting Sanctuary", "Away")
    [[nodiscard]] static std::string StatusText(const Friend& acFriend) noexcept;
    // Their build is this one, or they don't say (joining needs the same)
    [[nodiscard]] static bool VersionOk(const Friend& acFriend) noexcept;
    // Their world can be joined: they host it, it's open to friends or they invited this player, the same build
    [[nodiscard]] static bool CanJoin(const Friend& acFriend) noexcept;

    // Who's let in through Steam (the gateway asks)
    PeerToPeerGateway::Decision Decide(uint64_t aSteamId) noexcept;
    // Joining a friend's world: at the main menu its world page opens; in the world the player's asked to leave first
    void Join(uint64_t aHostSteamId, const std::string& acWorldName) noexcept;
    void Invite(uint64_t aSteamId) noexcept;
    void SetPrivacy(Privacy aPrivacy) noexcept;
    void OnJoinRequested(const std::string& acConnect) noexcept;
    // In a world: the sidebar asks first whether to leave it for the friend's (it's saved and left)
    void AskToJoin(uint64_t aHostSteamId) noexcept;

    [[nodiscard]] const Friend* FindFriend(uint64_t aSteamId) const noexcept;
    [[nodiscard]] std::string NameOf(uint64_t aSteamId) const noexcept;

    World& m_world;
    TransportService& m_transport;
    entt::scoped_connection m_overlayRestartedConnection;
    bool m_ready{false};
    uint64_t m_me{0};

    // hosting: the world, its privacy, who was invited (when), the gateway letting friends in
    std::string m_hostedWorldId;
    std::string m_hostedName;
    Privacy m_privacy{Privacy::kFriends};
    std::map<uint64_t, std::chrono::steady_clock::time_point> m_invited;
    PeerToPeerGateway m_gateway;
    uint16_t m_serverPort{0};
    // joined: the world's name and its host's SteamID (0: joined by address)
    std::string m_playingName;
    uint64_t m_playingHost{0};

    std::vector<Friend> m_friends;
    std::set<uint64_t> m_invitesSeen;
    std::vector<uint64_t> m_waitingShown;
    // avatars the overlay asked for, still loading in Steam
    std::set<uint64_t> m_avatarsWanted;
    std::set<uint64_t> m_avatarsSent;
    // a join waiting for the player to leave this world (at the main menu it goes on)
    uint64_t m_pendingJoin{0};
    std::string m_pendingJoinName;

    std::chrono::steady_clock::time_point m_lastRefresh{};
    bool m_dirty{true};
    bool m_presenceDirty{true};
    std::string m_lastState;

    int m_sidebarKey{'P'};
    bool m_keyDown{false};
    bool m_sidebarOpen{false};

    // the Pip-Boy tab's entries, in order
    std::vector<PipboyEntry> m_pipboyEntries;
    bool m_pipboyFilled{false};
    // the sidebar to open once the Pip-Boy has closed (an entry pressed), with this friend's join asked (0: none)
    bool m_sidebarWanted{false};
    uint64_t m_askJoin{0};

    std::mutex m_uiLock;
    std::vector<std::pair<std::string, std::string>> m_uiRequests;
};

#endif

#pragma once

#include <include/internal/cef_ptr.h>

namespace TiltedPhoques
{
struct OverlayApp;
}

struct OverlayClient;
struct RenderSystemD3D11;
struct D3D11RenderProvider;
struct FormIdComponent;
struct World;
struct Actor;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct TransportService;
struct NotifyChatMessageBroadcast;
struct NotifyPlayerList;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerDialogue;
struct ConnectionErrorEvent;
struct NotifyPlayerLevel;
struct NotifyPlayerCellChanged;
struct NotifyTeleport;
struct NotifyPlayerHealthUpdate;
enum ChatMessageTypes;
struct PartyJoinedEvent;
struct PartyLeftEvent;

using TiltedPhoques::OverlayApp;

/**
 * @brief Controls the UI from the client.
 */
struct OverlayService
{
    OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher);
    ~OverlayService() noexcept;

    TP_NOCOPYMOVE(OverlayService);

    void Create(RenderSystemD3D11* apRenderSystem) noexcept;

    void Render() noexcept;
    void Reset() const noexcept;
    void Reload() noexcept;

    void Initialize() noexcept;

    void SetActive(bool aActive) noexcept;
    [[nodiscard]] bool GetActive() const noexcept;

    void SetInGame(bool aInGame) noexcept;
    [[nodiscard]] bool GetInGame() const noexcept;

    // Fallout 4 main menu: the overlay drawn and taking input with no game loaded, for the multiplayer screens
    void SetMenuMode(bool aMenuMode) noexcept;
    [[nodiscard]] bool GetMenuMode() const noexcept { return m_menuMode; }
    // Sends an event with one string argument (e.g. JSON) to the overlay page
    void SendEvent(const char* acpEvent, const std::string& acArgument) const noexcept;

    void SetVersion(const std::string& acVersion);

    OverlayApp* GetOverlayApp() const noexcept { return m_pOverlay.get(); }

    // If the page's process ends, the old picture is hidden, the page reloaded, and the new page given the game's state.
    // The page asked for its state (uiReady), on the main loop: true if this was a page coming back
    bool OnPageReady() noexcept;
    // The process kept ending, the overlay stays off until the game restarts: the game's own HUD and menus are used
    [[nodiscard]] bool IsOff() const noexcept { return m_pageState == PageState::kOff; }
    // Shift+F2, from the window's thread: ends the page's process and brings the page back the same way. Asked for, so it
    // doesn't count towards the overlay staying off, and it also brings the overlay back from that
    void RestartPage() noexcept { m_pageRestartAsked = true; }

    void SendSystemMessage(const std::string& acMessage);
    // Shows a "respawning in" countdown on the overlay, 0 hides it
    void SetRespawnCountdown(double aSeconds) noexcept;

    void SetPlayerHealthPercentage(uint32_t aFormId) const noexcept;

protected:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnectedEvent(const ConnectedEvent&) noexcept;
    void OnDisconnectedEvent(const DisconnectedEvent&) noexcept;
    void OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) noexcept;
    void OnChatMessageReceived(const NotifyChatMessageBroadcast&) noexcept;
    void OnPlayerDialogue(const NotifyPlayerDialogue&) noexcept;
    void OnPlayerJoined(const NotifyPlayerJoined&) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft&) noexcept;
    void OnPlayerLevel(const NotifyPlayerLevel&) noexcept;
    void OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept;
    void OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept;
    void OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;

private:
    enum class PageState
    {
        kUp,
        kReloading,
        kOff,
    };

    // A player of the session, as the page was told: a new page has to be told again
    struct KnownPlayer
    {
        String Username{};
        uint16_t Level{};
        String CellName{};
        float Health{-1.f};
    };

    void RunDebugDataUpdates() noexcept;
    void RunPlayerHealthUpdates() noexcept;
    void RunPageRecovery() noexcept;
    void SendSessionToPage() noexcept;
    void SendPlayer3dLoaded(entt::entity aEntity) const noexcept;

    CefRefPtr<OverlayApp> m_pOverlay{nullptr};
    ::OverlayClient* m_pClient{nullptr};
    TiltedPhoques::UniquePtr<D3D11RenderProvider> m_pProvider;

    World& m_world;
    TransportService& m_transport;

    bool m_active = false;
    bool m_inGame = false;
    bool m_menuMode = false;

    std::atomic<PageState> m_pageState{PageState::kUp};
    std::chrono::steady_clock::time_point m_pageReloadTime{};
    std::vector<std::chrono::steady_clock::time_point> m_pageEnds;
    std::atomic<bool> m_pageRestartAsked{false};
    bool m_pageEnding = false; // told to end by RestartPage, and hasn't yet
    std::chrono::steady_clock::time_point m_pageEndingTime{};
    std::string m_pageUrl;

    bool m_connected = false;
    uint32_t m_localPlayerId = 0;
    std::map<uint32_t, KnownPlayer> m_players;
    std::chrono::steady_clock::time_point m_respawnEnd{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_connectionErrorConnection;
    entt::scoped_connection m_cellChangeEventConnection;
    entt::scoped_connection m_chatMessageConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_playerJoinedConnection;
    entt::scoped_connection m_playerLeftConnection;
    entt::scoped_connection m_playerAddedConnection;
    entt::scoped_connection m_playerRemovedConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_cellChangedConnection;
    entt::scoped_connection m_teleportConnection;
    entt::scoped_connection m_playerHealthConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
};

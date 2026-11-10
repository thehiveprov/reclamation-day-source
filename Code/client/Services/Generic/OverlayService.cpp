#include <HivemindPCH.h>

#include <Services/OverlayService.h>

#include <OverlayApp.hpp>

#include <D3D11Hook.hpp>
#include <OverlayRenderHandlerD3D11.hpp>

#include <Systems/RenderSystemD3D11.h>

#include <World.h>

#include <Services/OverlayClient.h>
#include <Services/TransportService.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include <Structs/GridCellCoords.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/OverlayRestartedEvent.h>

#include <PlayerCharacter.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>
#include <PerfProbe.h>

using TiltedPhoques::OverlayRenderHandler;
using TiltedPhoques::OverlayRenderHandlerD3D11;

namespace
{
// The page's process ending: reloaded this many times in this long, once more and the overlay stays off
constexpr size_t kPageRestarts = 3;
constexpr auto kPageRestartWindow = std::chrono::minutes(2);
// A reloaded page that hasn't asked for its state by then counts as another ending
constexpr auto kPageReloadTimeout = std::chrono::seconds(45);
// A process told to end (Shift+F2) that hasn't by then: the page is just loaded again
constexpr auto kPageEndingTimeout = std::chrono::seconds(5);
} // namespace

struct D3D11RenderProvider final : OverlayApp::RenderProvider, OverlayRenderHandlerD3D11::Renderer
{
    explicit D3D11RenderProvider(RenderSystemD3D11* apRenderSystem)
        : m_pRenderSystem(apRenderSystem)
    {
    }

    OverlayRenderHandler* Create() override
    {
        auto* pHandler = new OverlayRenderHandlerD3D11(this);
#if TP_SKYRIM64
        pHandler->SetVisible(true);
#else
        // Drawn only once in game, see OverlayService::Render
        pHandler->SetVisible(false);
#endif

        return pHandler;
    }

    [[nodiscard]] HWND GetWindow() override { return m_pRenderSystem->GetWindow(); }

    [[nodiscard]] IDXGISwapChain* GetSwapChain() const noexcept override { return m_pRenderSystem->GetSwapChain(); }

private:
    RenderSystemD3D11* m_pRenderSystem;
};

String GetCellName(const GameId& aWorldSpaceId, const GameId& aCellId) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    String cellName = "UNKNOWN";

    if (aWorldSpaceId)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(aWorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
            cellName = pWorldSpace->GetName();
    }
    else
    {
        const uint32_t cellId = modSystem.GetGameId(aCellId);
        TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
        if (pCell)
            cellName = pCell->GetName();
    }

    return cellName;
}

float CalculateHealthPercentage(Actor* apActor) noexcept
{
    const float maxHealth = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float tempModHealth = apActor->healthModifiers.temporaryModifier;

    if (maxHealth == 0.f)
        return 0.f;
    const float health = apActor->GetActorValue(ActorValueInfo::kHealth);

    float percentage = health / (maxHealth + tempModHealth) * 100.f;
    if (percentage < 0.f)
        percentage = 0.f;

    return percentage;
}

OverlayService::OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(transport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&OverlayService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&OverlayService::OnConnectedEvent>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&OverlayService::OnDisconnectedEvent>(this);
    m_connectionErrorConnection = aDispatcher.sink<ConnectionErrorEvent>().connect<&OverlayService::OnConnectionError>(this);
    m_chatMessageConnection = aDispatcher.sink<NotifyChatMessageBroadcast>().connect<&OverlayService::OnChatMessageReceived>(this);
    m_playerJoinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&OverlayService::OnPlayerJoined>(this);
    m_playerLeftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&OverlayService::OnPlayerLeft>(this);
    m_playerDialogueConnection = aDispatcher.sink<NotifyPlayerDialogue>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&OverlayService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&OverlayService::OnPlayerComponentRemoved>(this);
    m_playerLevelConnection = aDispatcher.sink<NotifyPlayerLevel>().connect<&OverlayService::OnPlayerLevel>(this);
    m_cellChangedConnection = aDispatcher.sink<NotifyPlayerCellChanged>().connect<&OverlayService::OnPlayerCellChanged>(this);
    m_teleportConnection = aDispatcher.sink<NotifyTeleport>().connect<&OverlayService::OnNotifyTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<NotifyPlayerHealthUpdate>().connect<&OverlayService::OnNotifyPlayerHealthUpdate>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&OverlayService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&OverlayService::OnPartyLeftEvent>(this);
}

OverlayService::~OverlayService() noexcept
{
}

void OverlayService::Create(RenderSystemD3D11* apRenderSystem) noexcept
{
    m_pProvider = TiltedPhoques::MakeUnique<D3D11RenderProvider>(apRenderSystem);
    m_pClient = new ::OverlayClient(m_transport, m_pProvider->Create());
    m_pOverlay = new OverlayApp(m_pProvider.get(), m_pClient);

    // NULL PATH FOR NOW
    if (!m_pOverlay->Initialize(""))
        spdlog::error("Overlay could not be initialised");

    m_pOverlay->GetClient()->Create();
}

void OverlayService::Render() noexcept
{
    static bool s_bi = false;
    if (!s_bi)
    {
        // Skyrim only: it crashed Fallout 4 sometimes
#if TP_SKYRIM64
        m_pOverlay->GetClient()->GetBrowser()->GetHost()->WasResized();
#endif

        s_bi = true;
    }

    auto pPlayer = PlayerCharacter::Get();
    bool inGame = pPlayer && pPlayer->GetNiNode();
    if (inGame && !m_inGame)
        SetInGame(true);
    else if (!inGame && m_inGame)
        SetInGame(false);

    RunPageRecovery();

#if TP_FALLOUT4
    // Drawn whenever in game, also with the UI closed, for HUD elements (party health, respawn countdown); whether the
    // UI is open is GetActive(). Not before: during startup the game has no viewport set yet and DirectXTK's
    // SpriteBatch throws, which aborts the game. At the main menu only while its multiplayer screens are open. Not
    // while the page is down either: what's left is the old page's last picture.
    if (const auto pRenderer = m_pOverlay->GetClient()->GetOverlayRenderHandler())
        pRenderer->SetVisible((m_inGame || m_menuMode) && m_pageState == PageState::kUp);

    try
    {
        m_pOverlay->GetClient()->Render();
    }
    catch (const std::exception& e)
    {
        static bool s_logged = false;
        if (!s_logged)
        {
            spdlog::error("Overlay render failed: {}", e.what());
            s_logged = true;
        }
    }
#else
    m_pOverlay->GetClient()->Render();
#endif
}

void OverlayService::Reset() const noexcept
{
    m_pOverlay->GetClient()->Reset();
}

void OverlayService::Reload() noexcept
{
    SetInGame(false);
    SetActive(false);
    GetOverlayApp()->GetClient()->GetBrowser()->Reload();
    Initialize();
    SetInGame(true);
    m_pOverlay->ExecuteAsync("enterGame");
    SetActive(true);
}

void OverlayService::Initialize() noexcept
{
    m_pOverlay->ExecuteAsync("init");
}

// Wherever the game is drawing, loading screens included (the main loop doesn't run through a load): nothing of the world
// is touched here, the state goes to the new page from OnPageReady
void OverlayService::RunPageRecovery() noexcept
{
    if (!m_pClient)
        return;

    bool ended = m_pClient->TakeProcessEnded();
    const auto now = std::chrono::steady_clock::now();
    if (m_pageState == PageState::kReloading && !m_pClient->HasPageAsked() && now - m_pageReloadTime > kPageReloadTimeout)
    {
        spdlog::error("Overlay: its page didn't come back");
        ended = true;
    }

    const auto pBrowser = m_pClient->GetBrowser();

    if (m_pageRestartAsked.exchange(false) && pBrowser)
    {
        if (m_pageState == PageState::kOff)
        {
            // Given up on: asked for, so start over
            spdlog::warn("Overlay: Shift+F2, reloading its page");
            m_pageEnds.clear();
            m_pageState = PageState::kReloading;
            m_pageReloadTime = now;
            m_pClient->ForgetPage();
            pBrowser->Reload();
        }
        else if (m_pageState == PageState::kUp && !m_pageEnding)
        {
            spdlog::warn("Overlay: Shift+F2, ending its process");
            m_pageEnding = true;
            m_pageEndingTime = now;
            const auto pFrame = pBrowser->GetMainFrame();
            m_pageUrl = pFrame->GetURL().ToString();
            // A debug address the browser hands to the page's process, which ends itself: the same as any other ending
            pFrame->LoadURL("chrome://kill");
        }
    }

    if (m_pageEnding && !ended && now - m_pageEndingTime > kPageEndingTimeout)
    {
        // Reload would load that address again if the frame went to it
        spdlog::warn("Overlay: its process didn't end, loading its page again");
        m_pageEnding = false;
        m_pageState = PageState::kReloading;
        m_pageReloadTime = now;
        m_pClient->ForgetPage();
        if (pBrowser)
            pBrowser->GetMainFrame()->LoadURL(m_pageUrl);
    }

    if (ended && m_pageState != PageState::kOff)
    {
        std::erase_if(m_pageEnds, [now](const auto& acEnd) { return now - acEnd > kPageRestartWindow; });
        if (m_pageEnding)
            m_pageEnding = false;
        else
            m_pageEnds.push_back(now);

        if (m_pageEnds.size() > kPageRestarts)
        {
            m_pageState = PageState::kOff;
            spdlog::error("Overlay: its process ended {} times in two minutes: the overlay stays off until the game restarts, the game's own HUD and menus are back",
                          m_pageEnds.size());
        }
        else
        {
            m_pageState = PageState::kReloading;
            m_pageReloadTime = now;
            m_pClient->ForgetPage();
            if (pBrowser)
                pBrowser->Reload();
            spdlog::warn("Overlay: reloading its page (time {} of {} in two minutes)", m_pageEnds.size(), kPageRestarts);
        }
    }

    // Nothing to take input for (at the main menu the overlay's menu mode ends as MainMenuIntegration closes its screens)
    if (m_pageState == PageState::kOff && m_active && !m_menuMode)
        m_active = false;
}

bool OverlayService::OnPageReady() noexcept
{
    // (a page that isn't there anymore asked: this one's own uiReady is still to come)
    if (m_pageState != PageState::kReloading || !m_pClient->HasPageAsked())
        return false;

    m_pageState = PageState::kUp;

    // Only what changes is copied into the picture, and all of the old page is still in it
    if (const auto pRenderer = m_pClient->GetOverlayRenderHandler())
        pRenderer->ClearFrame();
    if (const auto pBrowser = m_pClient->GetBrowser())
        pBrowser->GetHost()->Invalidate(PET_VIEW);

    SetVersion(BUILD_COMMIT);
    if (m_inGame)
        m_pOverlay->ExecuteAsync("enterGame");
    SendSessionToPage();
    m_world.GetDispatcher().trigger(OverlayRestartedEvent{});
    if (m_active)
        m_pOverlay->ExecuteAsync("activate");

    spdlog::info("Overlay: its page is back (in game {}, active {}, connected {}, {} other players)", m_inGame, m_active, m_connected, m_players.size());
    return true;
}

void OverlayService::SendSessionToPage() noexcept
{
    if (m_respawnEnd > std::chrono::steady_clock::now())
        SetRespawnCountdown(std::chrono::duration<double>(m_respawnEnd - std::chrono::steady_clock::now()).count());

    if (!m_connected)
        return;

    m_pOverlay->ExecuteAsync("connect");
    auto pLocalId = CefListValue::Create();
    pLocalId->SetInt(0, m_localPlayerId);
    m_pOverlay->ExecuteAsync("setLocalPlayerId", pLocalId);

    for (const auto& [id, player] : m_players)
    {
        auto pArguments = CefListValue::Create();
        pArguments->SetInt(0, id);
        pArguments->SetString(1, player.Username.c_str());
        pArguments->SetInt(2, player.Level);
        pArguments->SetString(3, player.CellName.c_str());
        m_pOverlay->ExecuteAsync("playerConnected", pArguments);
    }

    // Those whose characters are here
    auto view = m_world.view<PlayerComponent, FormIdComponent>();
    for (auto entity : view)
    {
        if (!m_world.try_get<WaitingFor3D>(entity))
            SendPlayer3dLoaded(entity);
    }

    for (const auto& [id, player] : m_players)
    {
        if (player.Health < 0.f)
            continue;
        auto pArguments = CefListValue::Create();
        pArguments->SetInt(0, id);
        pArguments->SetDouble(1, static_cast<double>(player.Health));
        m_pOverlay->ExecuteAsync("setHealth", pArguments);
    }

    const auto& party = m_world.GetPartyService();
    if (party.IsInParty())
    {
        if (party.IsLeader())
            m_pOverlay->ExecuteAsync("partyCreated");

        const auto& members = party.GetPartyMembers();
        if (!members.empty())
        {
            auto pPlayerIds = CefListValue::Create();
            for (int i = 0; i < members.size(); i++)
                pPlayerIds->SetInt(i, members[i]);
            auto pArguments = CefListValue::Create();
            pArguments->SetList(0, pPlayerIds);
            pArguments->SetInt(1, party.GetLeaderPlayerId());
            m_pOverlay->ExecuteAsync("partyInfo", pArguments);
        }
    }
}

void OverlayService::SetActive(bool aActive) noexcept
{
    if (!m_inGame && !m_menuMode)
        return;
    if (m_active == aActive)
        return;
    if (aActive && m_pageState == PageState::kOff)
        return;

    m_active = aActive;

    m_pOverlay->ExecuteAsync(m_active ? "activate" : "deactivate");
}

bool OverlayService::GetActive() const noexcept
{
    return m_active;
}

void OverlayService::SetInGame(bool aInGame) noexcept
{
    if (m_inGame == aInGame)
        return;
    m_inGame = aInGame;

    if (m_inGame)
    {
        SetVersion(BUILD_COMMIT);
        m_pOverlay->ExecuteAsync("enterGame");
    }
    else
    {
        m_pOverlay->ExecuteAsync("exitGame");
        // Closed with the game: SetActive() refuses once m_inGame is false, and an overlay left "active" kept blocking
        // the main menu's input
        if (m_active && !m_menuMode)
        {
            m_active = false;
            m_pOverlay->ExecuteAsync("deactivate");
        }
    }
}

bool OverlayService::GetInGame() const noexcept
{
    return m_inGame;
}

void OverlayService::SetMenuMode(bool aMenuMode) noexcept
{
    if (m_menuMode == aMenuMode)
        return;
    m_menuMode = aMenuMode;

    if (m_menuMode)
    {
        SetVersion(BUILD_COMMIT);
        m_active = true;
        m_pOverlay->ExecuteAsync("activate");
        while (ShowCursor(TRUE) <= 0)
            ;
    }
    else if (!m_inGame)
    {
        m_active = false;
        m_pOverlay->ExecuteAsync("deactivate");
        while (ShowCursor(FALSE) >= 0)
            ;
    }
}

void OverlayService::SendEvent(const char* acpEvent, const std::string& acArgument) const noexcept
{
    // Game events (quests starting or stopping as a save loads) can come before the overlay exists
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetString(0, acArgument);
    m_pOverlay->ExecuteAsync(acpEvent, pArguments);
}

void OverlayService::SetVersion(const std::string& acVersion)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();

    pArguments->SetString(0, acVersion);
    m_pOverlay->ExecuteAsync("setVersion", pArguments);
}

void OverlayService::SendSystemMessage(const std::string& acMessage)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kSystemMessage);
    pArguments->SetString(1, acMessage);

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::SetPlayerHealthPercentage(uint32_t aFormId) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    auto entityIt = std::find_if(view.begin(), view.end(), [view, aFormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == aFormId; });

    if (entityIt == view.end())
    {
        spdlog::error("{}: cannot find player entity for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    const auto& playerComponent = view.get<PlayerComponent>(*entityIt);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfProbe::Section _perf("Overlay");
    RunDebugDataUpdates();
    RunPlayerHealthUpdates();
}

void OverlayService::SetRespawnCountdown(double aSeconds) noexcept
{
    if (!m_pOverlay)
        return;

    m_respawnEnd = aSeconds > 0.0 ? std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(aSeconds))
                                  : std::chrono::steady_clock::time_point{};

    auto pArguments = CefListValue::Create();
    pArguments->SetDouble(0, aSeconds);
    m_pOverlay->ExecuteAsync("respawnCountdown", pArguments);
}

void OverlayService::OnConnectedEvent(const ConnectedEvent& acEvent) noexcept
{
    m_connected = true;
    m_localPlayerId = acEvent.PlayerId;

    m_pOverlay->ExecuteAsync("connect");

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acEvent.PlayerId);
    m_pOverlay->ExecuteAsync("setLocalPlayerId", pArguments);
}

void OverlayService::OnDisconnectedEvent(const DisconnectedEvent&) noexcept
{
    m_connected = false;
    m_players.clear();

    m_pOverlay->ExecuteAsync("disconnect");
}

void OverlayService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    SendPlayer3dLoaded(aEntity);
}

void OverlayService::SendPlayer3dLoaded(entt::entity aEntity) const noexcept
{
    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, formIdComponent.Id);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, pPlayerComponent->Id);
    pArguments->SetInt(1, static_cast<int>(percentage));

    m_pOverlay->ExecuteAsync("setPlayer3dLoaded", pArguments);
}

void OverlayService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    const auto& playerComponent = m_world.get<PlayerComponent>(aEntity);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);

    m_pOverlay->ExecuteAsync("setPlayer3dUnloaded", pArguments);
}

void OverlayService::OnChatMessageReceived(const NotifyChatMessageBroadcast& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, (int)acMessage.MessageType);
    pArguments->SetString(1, acMessage.ChatMessage.c_str());
    pArguments->SetString(2, acMessage.PlayerName.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnPlayerDialogue(const NotifyPlayerDialogue& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kPlayerDialogue);
    pArguments->SetString(1, acMessage.Text.c_str());
    pArguments->SetString(2, acMessage.Name.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) noexcept
{
    auto pArgs = CefListValue::Create();
    pArgs->SetString(0, acConnectedEvent.ErrorDetail.c_str());
    m_pOverlay->ExecuteAsync("triggerError", pArgs);
    // A refusal usually arrives in game (the main menu's join loads the save first): the overlay takes the mouse, so
    // its prompt can be answered (re-sort the load order, OK) without knowing to open it
    SetActive(true);
}

void OverlayService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    pArguments->SetInt(2, acMessage.Level);

    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(3, cellName.c_str());

    m_players[acMessage.PlayerId] = KnownPlayer{acMessage.Username, acMessage.Level, cellName};

    m_pOverlay->ExecuteAsync("playerConnected", pArguments);
}

void OverlayService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_players.erase(acMessage.PlayerId);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    m_pOverlay->ExecuteAsync("playerDisconnected", pArguments);
}

void OverlayService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    if (const auto it = m_players.find(acMessage.PlayerId); it != m_players.end())
        it->second.Level = acMessage.NewLevel;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetInt(1, acMessage.NewLevel);
    m_pOverlay->ExecuteAsync("setLevel", pArguments);
}

void OverlayService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    if (const auto it = m_players.find(acMessage.PlayerId); it != m_players.end())
        it->second.CellName = cellName;
    pArguments->SetString(1, cellName.c_str());
    m_pOverlay->ExecuteAsync("setCell", pArguments);
}

void OverlayService::OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cellId = modSystem.GetGameId(acMessage.CellId);
    TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
    if (!pCell)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(acMessage.WorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acMessage.Position);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }

        if (!pCell)
        {
            spdlog::error("Failed to fetch cell to teleport to.");
            m_world.GetOverlayService().SendSystemMessage("Teleporting to player failed.");
            return;
        }
    }

    PlayerCharacter::Get()->MoveTo(pCell, acMessage.Position);
}

void OverlayService::OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    const float percentage = acMessage.Percentage >= 0.f ? acMessage.Percentage : 0.f;

    if (const auto it = m_players.find(acMessage.PlayerId); it != m_players.end())
        it->second.Health = percentage;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (acEvent.IsLeader)
        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyCreated");
}

void OverlayService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyLeft");
}

void OverlayService::RunDebugDataUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto internalStats = m_transport.GetStatistics();
    auto steamStats = m_transport.GetConnectionStatus();

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, steamStats.m_flOutPacketsPerSec);
    pArguments->SetInt(1, steamStats.m_flInPacketsPerSec);
    pArguments->SetInt(2, steamStats.m_nPing);
    pArguments->SetInt(3, 0);
    pArguments->SetInt(4, internalStats.SentBytes);
    pArguments->SetInt(5, internalStats.RecvBytes);

    m_pOverlay->ExecuteAsync("debugData", pArguments);
}

// TODO(cosideci): this whole thing is a really hacky solution to
// health sync code being somewhat broken for players.
void OverlayService::RunPlayerHealthUpdates() noexcept
{
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static float s_previousPercentage = -1.f;

    const float newPercentage = CalculateHealthPercentage(PlayerCharacter::Get());
    if (newPercentage == s_previousPercentage)
        return;

    s_previousPercentage = newPercentage;

    RequestPlayerHealthUpdate request{};
    request.Percentage = newPercentage;

    m_transport.Send(request);
}

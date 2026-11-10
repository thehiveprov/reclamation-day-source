#include <HivemindPCH.h>

#include <OverlayRenderHandler.hpp>

#include <Services/OverlayClient.h>
#include <Services/TransportService.h>

#include <Messages/SendChatMessageRequest.h>
#include <Messages/TeleportRequest.h>

#include <Events/SetTimeCommandEvent.h>
#include <Events/ConnectionErrorEvent.h>

#include <World.h>

#if TP_FALLOUT4
#include <Interface/MainMenuIntegration.h>
#include <Interface/LanBrowser.h>
#include <Interface/MenuSounds.h>
#include <Interface/LoadOrderMatch.h>
#include <Services/BuildToolsService.h>
#include <Services/QuestService.h>
#include <Services/CompanionService.h>
#include <Services/WorkshopService.h>
#include <Services/NemesisService.h>
#include <Services/SteamService.h>
#include <Services/EmoteService.h>
#include <Services/DialogueVoteService.h>
#endif

OverlayClient::OverlayClient(TransportService& aTransport, TiltedPhoques::OverlayRenderHandler* apHandler)
    : TiltedPhoques::OverlayClient(apHandler)
    , m_transport(aTransport)
{
}

OverlayClient::~OverlayClient() noexcept
{
}

void OverlayClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status)
{
    const char* pStatus = "ended abnormally";
    switch (status)
    {
    case TS_PROCESS_WAS_KILLED: pStatus = "was killed"; break;
    case TS_PROCESS_CRASHED: pStatus = "crashed"; break;
    case TS_PROCESS_OOM: pStatus = "ran out of memory"; break;
    default: break;
    }
    spdlog::error("Overlay: its process {}", pStatus);
    m_processEnded = true;
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    if (message->GetName() == "ui-event")
    {
        auto pArguments = message->GetArgumentList();

        auto eventName = pArguments->GetString(0).ToString();
        auto eventArgs = pArguments->GetList(1);

        // One line per event, none for the menu sounds (one per button press), and only the first argument (a connect's
        // third is the server's password)
        if (eventName != "playSound")
            spdlog::info("Overlay: {} {}", eventName, eventArgs->GetSize() > 0 ? eventArgs->GetString(0).ToString() : std::string{});

#ifndef PUBLIC_BUILD
        LOG(INFO) << "event=ui_event name=" << eventName;
#endif

        if (eventName == "connect")
            ProcessConnectMessage(eventArgs);
        else if (eventName == "disconnect")
            ProcessDisconnectMessage();
        else if (eventName == "sendMessage")
            ProcessChatMessage(eventArgs);
        else if (eventName == "setTime")
            ProcessSetTimeCommand(eventArgs);
        else if (eventName == "launchParty")
            World::Get().GetPartyService().CreateParty();
        else if (eventName == "leaveParty")
            World::Get().GetPartyService().LeaveParty();
        else if (eventName == "createPartyInvite")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().CreateInvite(aPlayerId);
        }
        else if (eventName == "acceptPartyInvite")
        {
            uint32_t aInviterId = eventArgs->GetInt(0);
            // push to main thread because the party service has to check validity of invite thread safely
            World::Get().GetRunner().Queue([aInviterId]() { World::Get().GetPartyService().AcceptInvite(aInviterId); });
        }
        else if (eventName == "kickPartyMember")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().KickPartyMember(aPlayerId);
        }
        else if (eventName == "changePartyLeader")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().ChangePartyLeader(aPlayerId);
        }
        else if (eventName == "teleportToPlayer")
            ProcessTeleportMessage(eventArgs);
        else if (eventName == "toggleDebugUI")
            ProcessToggleDebugUI();
#if TP_FALLOUT4
        // Main menu multiplayer screens: handled on the game's main loop, the runner doesn't run at the main menu
        else if (eventName == "mainMenuAction")
        {
            std::string action = eventArgs->GetString(0).ToString();
            if (action == "uiReady")
                m_pageAsked = true;
            QueueMainMenuAction(std::move(action), eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{});
        }
        // The server browsers' LAN tab: lanScan(), answered with the lanServers event (see LanBrowser)
        else if (eventName == "lanScan")
            LanBrowser::StartScan();
        // The overlay's buttons: the game's own menu sounds, playSound("UIMenuOK")
        else if (eventName == "playSound")
            MenuSounds::Queue(eventArgs->GetString(0).ToString());
        // Refused for our plugins: matchLoadOrder("host plugins, one per line"), see LoadOrderMatch
        else if (eventName == "matchLoadOrder")
            LoadOrderMatch::Queue(eventArgs->GetString(0).ToString());
        // Settings, Build tools: buildTools(request, payload), see BuildToolsService::OnUiRequest
        else if (eventName == "buildTools")
        {
            std::string request = eventArgs->GetString(0).ToString();
            std::string payload = eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{};
            World::Get().GetRunner().Queue([request, payload]() { World::Get().ctx().at<BuildToolsService>().OnUiRequest(request, payload); });
        }
        // Player manager, Quests: quests(request, payload), see QuestService::OnUiRequest
        else if (eventName == "quests")
        {
            std::string request = eventArgs->GetString(0).ToString();
            std::string payload = eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{};
            World::Get().GetRunner().Queue([request, payload]() { World::Get().ctx().at<QuestService>().OnUiRequest(request, payload); });
        }
        // A dog's name card: companions(request, payload), see CompanionService::OnUiRequest
        else if (eventName == "companions")
        {
            std::string request = eventArgs->GetString(0).ToString();
            std::string payload = eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{};
            World::Get().GetRunner().Queue([request, payload]() { World::Get().ctx().at<CompanionService>().OnUiRequest(request, payload); });
        }
        // Player manager, Settlements: settlements(request, payload), see WorkshopService::OnUiRequest
        else if (eventName == "settlements")
        {
            std::string request = eventArgs->GetString(0).ToString();
            std::string payload = eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{};
            World::Get().GetRunner().Queue([request, payload]() { World::Get().ctx().at<WorkshopService>().OnUiRequest(request, payload); });
        }
        // The social sidebar (Steam friends): social(request, payload), see SteamService::OnUiRequest. The main loop runs
        // it (the runner doesn't run at the main menu)
        else if (eventName == "social")
            World::Get().ctx().at<SteamService>().QueueUiRequest(eventArgs->GetString(0).ToString(), eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{});
        // Nemeses, the questioning card: nemeses(request, payload), see NemesisService::OnUiRequest
        else if (eventName == "nemeses")
        {
            std::string request = eventArgs->GetString(0).ToString();
            std::string payload = eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{};
            World::Get().GetRunner().Queue([request, payload]() { World::Get().ctx().at<NemesisService>().OnUiRequest(request, payload); });
        }
        // Emotes, the wheel and the chat's commands: emotes(request, payload), see EmoteService::OnUiRequest
        else if (eventName == "emotes")
        {
            std::string request = eventArgs->GetString(0).ToString();
            std::string payload = eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{};
            World::Get().GetRunner().Queue([request, payload]() { World::Get().ctx().at<EmoteService>().OnUiRequest(request, payload); });
        }
        // Co-op dialogue, the vote card: dialogueVote(request, payload), see DialogueVoteService::OnUiRequest
        else if (eventName == "dialogueVote")
        {
            std::string request = eventArgs->GetString(0).ToString();
            std::string payload = eventArgs->GetSize() > 1 ? eventArgs->GetString(1).ToString() : std::string{};
            World::Get().GetRunner().Queue([request, payload]() { World::Get().ctx().at<DialogueVoteService>().OnUiRequest(request, payload); });
        }
#endif

        return true;
    }

    return false;
}

void OverlayClient::ProcessConnectMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string baseIp = aEventArgs->GetString(0);
    if (baseIp == "localhost")
    {
        baseIp = "127.0.0.1";
    }

    uint16_t port = aEventArgs->GetInt(1) ? aEventArgs->GetInt(1) : 10578;
    World::Get().GetTransport().SetServerPassword(aEventArgs->GetString(2));
    // Optional player name, older UIs don't send it
    std::string username = aEventArgs->GetSize() > 3 && aEventArgs->GetType(3) == VTYPE_STRING ? aEventArgs->GetString(3).ToString() : "";
    World::Get().GetTransport().SetUsername(username);

    std::string endpoint = baseIp + ":" + std::to_string(port);

    World::Get().GetRunner().Queue(
        [endpoint]
        {
#if TP_FALLOUT4
            // Co-op is only played in co-op saves, started from the main menu's MULTIPLAYER: connecting here from a
            // single-player game would write co-op progress into single-player saves
            if (!IsPlayingCoopSave())
            {
                spdlog::info("Not connecting to {} from a single-player game, co-op starts from MULTIPLAYER", endpoint);
                ConnectionErrorEvent errorEvent{};
                errorEvent.ErrorDetail = "{\"error\": \"use_main_menu\"}";
                World::Get().GetRunner().Trigger(errorEvent);
                return;
            }
#endif
            const bool result = World::Get().GetTransport().Connect(endpoint);
            spdlog::info("Connecting to {}, request accepted: {}", endpoint, result);
        });
}

void OverlayClient::ProcessDisconnectMessage()
{
    World::Get().GetRunner().Queue([]() { World::Get().GetTransport().Close(); });
}

void OverlayClient::ProcessChatMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string contents = aEventArgs->GetString(1).ToString();
    if (!contents.empty())
    {
        SendChatMessageRequest messageRequest;
        messageRequest.MessageType = static_cast<ChatMessageType>(aEventArgs->GetInt(0));
        messageRequest.ChatMessage = contents;

        spdlog::info(L"Send chat message of type {}: '{}' ", messageRequest.MessageType, aEventArgs->GetString(1).ToWString());

        m_transport.Send(messageRequest);
    }
}

void OverlayClient::ProcessSetTimeCommand(CefRefPtr<CefListValue> aEventArgs)
{
    const uint8_t hours = static_cast<uint8_t>(aEventArgs->GetInt(0));
    const uint8_t minutes = static_cast<uint8_t>(aEventArgs->GetInt(1));
    const uint32_t senderId = m_transport.GetLocalPlayerId();
    World::Get().GetDispatcher().trigger(SetTimeCommandEvent(hours, minutes, senderId));
}

void OverlayClient::ProcessTeleportMessage(CefRefPtr<CefListValue> aEventArgs)
{
    TeleportRequest request{};
    request.PlayerId = aEventArgs->GetInt(0);

    m_transport.Send(request);
}

void OverlayClient::ProcessToggleDebugUI()
{
    World::Get().GetDebugService().m_showDebugStuff = !World::Get().GetDebugService().m_showDebugStuff;
}

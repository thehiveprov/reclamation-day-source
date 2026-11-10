#pragma once

#include "OverlayClient.hpp"

struct TransportService;

namespace TiltedPhoques
{
struct OverlayRenderHandler;
}

/**
 * @brief Renders the UI overlay.
 */
struct OverlayClient : TiltedPhoques::OverlayClient, CefRequestHandler
{
    OverlayClient(TransportService& aTransport, TiltedPhoques::OverlayRenderHandler* apHandler);
    virtual ~OverlayClient() noexcept;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;

    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    // The page's process ended: nothing else says so, the last picture would stay on the screen. On CEF's thread, the
    // rest is OverlayService's
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status) override;

    // Did the page's process end since the last time this was asked?
    [[nodiscard]] bool TakeProcessEnded() noexcept { return m_processEnded.exchange(false); }
    // Has a page asked for the game's state since ForgetPage? (uiReady, the first thing it does as it starts)
    [[nodiscard]] bool HasPageAsked() const noexcept { return m_pageAsked; }
    void ForgetPage() noexcept { m_pageAsked = false; }

    TP_NOCOPYMOVE(OverlayClient);

private:
    void ProcessConnectMessage(CefRefPtr<CefListValue> aEventArgs);
    void ProcessDisconnectMessage();
    void ProcessChatMessage(CefRefPtr<CefListValue> aEventArgs);
    void ProcessSetTimeCommand(CefRefPtr<CefListValue> aEventArgs);
    void ProcessTeleportMessage(CefRefPtr<CefListValue> aEventArgs);
    void ProcessToggleDebugUI();

    TransportService& m_transport;
    std::atomic<bool> m_processEnded{false};
    std::atomic<bool> m_pageAsked{false};
};

#include <OverlayApp.hpp>
#include <filesystem>
#include <iostream>
#include <TiltedCore/Filesystem.hpp>

namespace TiltedPhoques
{
    OverlayApp::OverlayApp(RenderProvider *apRenderProvider, OverlayClient *apCustomClient, std::wstring aProcessName) noexcept
        : m_pBrowserProcessHandler(new OverlayBrowserProcessHandler)
        , m_pRenderProvider(apRenderProvider)
        , m_processName(std::move(aProcessName))
    {
        if (apCustomClient)
            m_pClient = apCustomClient;

    }

    bool OverlayApp::Initialize(const std::string& acPath) noexcept
    {
        CefMainArgs args(GetModuleHandleW(nullptr));

        const auto currentPath = TiltedPhoques::GetPath();

        CefSettings settings;
        settings.no_sandbox = true;
        settings.multi_threaded_message_loop = true;
        settings.windowless_rendering_enabled = true;

        // Warnings and errors only: verbose wrote a line for every frame the overlay's capture skipped, hundreds a
        // second while it animates
        settings.log_severity = LOGSEVERITY_WARNING;
#ifdef DEBUG
        // Chromium DevTools on localhost:8384, debug builds only: any program on the PC could drive the overlay
        // through it, and antivirus takes an open debugging port for a cookie stealer
        settings.remote_debugging_port = 8384;
#endif

        CefString(&settings.log_file).FromWString(currentPath / L"logs" / L"cef_debug.log");
        CefString(&settings.cache_path).FromWString(currentPath / L"cache");
        CefString(&settings.framework_dir_path).FromWString(currentPath);
        CefString(&settings.root_cache_path).FromWString(currentPath / L"cache");
        CefString(&settings.resources_dir_path).FromWString(currentPath);
        CefString(&settings.locales_dir_path).FromWString(currentPath / L"locales");
        CefString(&settings.browser_subprocess_path).FromWString(currentPath / m_processName);

        CefInitialize(args, settings, this, nullptr);

        if (!m_pClient)
            m_pClient = new OverlayClient(m_pRenderProvider->Create());

        CefBrowserSettings browserSettings{};

        // About as often as the game draws frames, which shows no more of them: CEF reads every paint back to memory
        browserSettings.windowless_frame_rate = 60;

        CefWindowInfo info;
        info.SetAsWindowless(m_pRenderProvider->GetWindow());

        const auto ret = CefBrowserHost::CreateBrowser(info, m_pClient.get(),
            (currentPath / L"UI" / acPath / L"index.html").wstring(), browserSettings, nullptr, nullptr);

        return ret;
    }

    void OverlayApp::Shutdown() noexcept
    {
        CefShutdown();
    }

    void OverlayApp::ExecuteAsync(const std::string& acFunction, const CefRefPtr<CefListValue>& apArguments) const noexcept
    {
        if (!m_pClient)
            return;

        auto pMessage = CefProcessMessage::Create("browser-event");
        auto pArguments = pMessage->GetArgumentList();

        const auto pFunctionArguments = apArguments ? apArguments : CefListValue::Create();

        pArguments->SetString(0, acFunction);
        pArguments->SetList(1, pFunctionArguments);

        auto pBrowser = m_pClient->GetBrowser();
        if (pBrowser)
        {
            // (it changes as the page reloads after its process ended)
            if (const auto pFrame = pBrowser->GetMainFrame())
                pFrame->SendProcessMessage(PID_RENDERER, pMessage);
        }
    }

    void OverlayApp::InjectKey(const cef_key_event_type_t aType, const uint32_t aModifiers, const uint16_t aKey, const uint16_t aScanCode) const noexcept
    {
        if (m_pClient)
        {
            CefKeyEvent ev;

            ev.type = aType;
            ev.modifiers = aModifiers;
            ev.windows_key_code = aKey;
            ev.native_key_code = aScanCode;

            if (m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendKeyEvent(ev);
        }
    }

    void OverlayApp::InjectMouseButton(const uint16_t aX, const uint16_t aY, const cef_mouse_button_type_t aButton, const bool aUp, const uint32_t aModifier) const noexcept
    {
        if (m_pClient)
        {
            CefMouseEvent ev;

            ev.x = aX;
            ev.y = aY;
            ev.modifiers = aModifier;

            if (m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendMouseClickEvent(ev, aButton, aUp, 1);
        }
    }

    void OverlayApp::InjectMouseMove(const uint16_t aX, const uint16_t aY, const uint32_t aModifier) const noexcept
    {
        if (m_pClient && m_pClient->GetOverlayRenderHandler())
        {
            CefMouseEvent ev;

            ev.x = aX;
            ev.y = aY;
            ev.modifiers = aModifier;

            m_pClient->GetOverlayRenderHandler()->SetCursorLocation(aX, aY);

            if(m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendMouseMoveEvent(ev, false);
        }
    }

    void OverlayApp::InjectMouseWheel(const uint16_t aX, const uint16_t aY, const int16_t aDelta, const uint32_t aModifier) const noexcept
    {
        if (m_pClient && m_pClient->GetOverlayRenderHandler())
        {
            CefMouseEvent ev;

            ev.x = aX;
            ev.y = aY;
            ev.modifiers = aModifier;

            if (m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendMouseWheelEvent(ev, 0, aDelta);
        }
    }

    void OverlayApp::OnBeforeCommandLineProcessing(const CefString& aProcessType, CefRefPtr<CefCommandLine> aCommandLine)
    {
        aCommandLine->AppendSwitch("allow-file-access-from-files");
        aCommandLine->AppendSwitch("allow-universal-access-from-files");
        // The page is drawn on the CPU. Drawn by CEF's GPU process, every frame had to be read back from a GPU the game
        // keeps busy: with a heavy ENB preset the overlay got 5 frames a second while it animated, now 60.
        aCommandLine->AppendSwitch("disable-gpu");
        aCommandLine->AppendSwitch("disable-gpu-compositing");
    }
}

#include "TargetConfig.h"
#include "ui/LauncherUi.h"

#include <TiltedCore/Filesystem.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(TARGET_FT)
#include "ui/LauncherApi.h"
#include "utils/ComUtils.h"

#include <client/Games/Fallout4/Interface/Updater.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_resource_handler.h>
#include <include/cef_scheme.h>

#include <ShellScalingApi.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <fstream>
#include <functional>
#include <sstream>
#endif

namespace launcher::ui
{
#if defined(TARGET_FT)
namespace
{
namespace fs = std::filesystem;

// The page and its /api/..., answered here (no network)
constexpr char kOrigin[] = "https://launcher.rd/";
constexpr wchar_t kWindowClass[] = L"ReclamationDayLauncher";
// The page's size at 100% scaling
constexpr int kWidth = 1280;
constexpr int kHeight = 760;

HWND s_window = nullptr;
int s_exitCode = kExitQuit;
bool s_closing = false;
CefRefPtr<CefBrowser> s_browser;

fs::path GetPageFolder()
{
    return TiltedPhoques::GetPath() / L"launcher";
}

// Each look's background, for the window before the page draws
COLORREF GetBackground()
{
    const std::string style = GetStyle();
    if (style == "brotherhood")
        return RGB(0x14, 0x17, 0x1a);
    if (style == "bulletin")
        return RGB(0x0f, 0x2a, 0x4d);
    if (style == "minutemen")
        return RGB(0x1c, 0x35, 0x57);
    return RGB(0x04, 0x12, 0x0a);
}

class Task : public CefTask
{
public:
    explicit Task(std::function<void()> aWork)
        : m_work(std::move(aWork))
    {
    }
    void Execute() override { m_work(); }

private:
    std::function<void()> m_work;
    IMPLEMENT_REFCOUNTING(Task);
};

std::string GetMimeType(const fs::path& acFile)
{
    const auto extension = acFile.extension().string();
    if (extension == ".html")
        return "text/html";
    if (extension == ".css")
        return "text/css";
    if (extension == ".js")
        return "text/javascript";
    if (extension == ".json")
        return "application/json";
    if (extension == ".png")
        return "image/png";
    if (extension == ".ttf")
        return "font/ttf";
    if (extension == ".txt")
        return "text/plain";
    return "application/octet-stream";
}

// One request to https://launcher.rd/: a file of the page from the launcher folder, or /api/<name> answered by
// LauncherApi on the UI thread
class Handler : public CefResourceHandler
{
public:
    bool Open(CefRefPtr<CefRequest> apRequest, bool& aHandleRequest, CefRefPtr<CefCallback> apCallback) override
    {
        std::string path = apRequest->GetURL().ToString().substr(std::size(kOrigin) - 1);
        path = path.substr(0, path.find_first_of("?#"));

        if (path.starts_with("api/"))
        {
            std::string body;
            if (const auto pPost = apRequest->GetPostData(); pPost && pPost->GetElementCount() > 0)
            {
                CefPostData::ElementVector elements;
                pPost->GetElements(elements);
                for (const auto& pElement : elements)
                {
                    if (pElement->GetType() != PDE_TYPE_BYTES)
                        continue;
                    std::string chunk(pElement->GetBytesCount(), '\0');
                    pElement->GetBytes(chunk.size(), chunk.data());
                    body += chunk;
                }
            }

            aHandleRequest = false;
            CefRefPtr<Handler> pSelf = this;
            CefPostTask(TID_UI, new Task(
                                    [pSelf, name = path.substr(4), body, apCallback]
                                    {
                                        pSelf->m_data = HandleApi(name, body);
                                        pSelf->m_mimeType = "application/json";
                                        apCallback->Continue();
                                    }));
            return true;
        }

        if (path.empty())
            path = "index.html";
        const auto root = GetPageFolder().lexically_normal();
        const auto file = (root / std::wstring(path.begin(), path.end())).lexically_normal();
        const auto rootText = root.wstring();
        std::ifstream stream(file, std::ios::binary);
        if (path.find("..") != std::string::npos || _wcsnicmp(file.c_str(), rootText.c_str(), rootText.size()) != 0 || !stream)
        {
            m_status = 404;
            m_data = "not found";
            m_mimeType = "text/plain";
            spdlog::warn("Launcher: the page asked for {}, which isn't there", path);
        }
        else
        {
            std::stringstream contents;
            contents << stream.rdbuf();
            m_data = contents.str();
            m_mimeType = GetMimeType(file);
        }
        aHandleRequest = true;
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> apResponse, int64& aLength, CefString& aRedirectUrl) override
    {
        apResponse->SetStatus(m_status);
        apResponse->SetStatusText(m_status == 200 ? "OK" : "Not Found");
        apResponse->SetMimeType(m_mimeType);
        CefResponse::HeaderMap headers;
        headers.emplace("Cache-Control", "no-store");
        apResponse->SetHeaderMap(headers);
        aLength = static_cast<int64>(m_data.size());
    }

    bool Read(void* apOut, int aToRead, int& aRead, CefRefPtr<CefResourceReadCallback> apCallback) override
    {
        aRead = 0;
        if (m_offset >= m_data.size())
            return false;
        aRead = static_cast<int>(std::min<size_t>(static_cast<size_t>(aToRead), m_data.size() - m_offset));
        std::memcpy(apOut, m_data.data() + m_offset, static_cast<size_t>(aRead));
        m_offset += static_cast<size_t>(aRead);
        return true;
    }

    void Cancel() override {}

private:
    int m_status = 200;
    std::string m_mimeType;
    std::string m_data;
    size_t m_offset = 0;
    IMPLEMENT_REFCOUNTING(Handler);
};

class Factory : public CefSchemeHandlerFactory
{
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString&, CefRefPtr<CefRequest>) override { return new Handler; }

private:
    IMPLEMENT_REFCOUNTING(Factory);
};

class Client : public CefClient, public CefLifeSpanHandler, public CefContextMenuHandler, public CefRequestHandler, public CefDisplayHandler, public CefLoadHandler
{
public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> apBrowser) override
    {
        s_browser = apBrowser;
        RECT rect{};
        GetClientRect(s_window, &rect);
        SetWindowPos(apBrowser->GetHost()->GetWindowHandle(), nullptr, 0, 0, rect.right, rect.bottom, SWP_NOZORDER);
    }

    bool DoClose(CefRefPtr<CefBrowser>) override { return false; }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        s_browser = nullptr;
        CefQuitMessageLoop();
    }

    // No right-click menu (back, reload, print...)
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> apModel) override { apModel->Clear(); }

    // The page stays the page: a link elsewhere opens in the player's browser
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> apRequest, bool aUserGesture, bool) override
    {
        const std::string url = apRequest->GetURL().ToString();
        if (url.starts_with(kOrigin))
            return false;
        if (aUserGesture && (url.starts_with("https://") || url.starts_with("http://")))
            ShellExecuteW(nullptr, L"open", std::wstring(url.begin(), url.end()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t aLevel, const CefString& acMessage, const CefString& acSource, int aLine) override
    {
        if (aLevel >= LOGSEVERITY_WARNING)
            spdlog::warn("Launcher page: {} ({}:{})", acMessage.ToString(), acSource.ToString(), aLine);
        return false;
    }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode aError, const CefString& acText, const CefString& acUrl) override
    {
        spdlog::error("Launcher: {} didn't load: {} ({})", acUrl.ToString(), acText.ToString(), static_cast<int>(aError));
    }

private:
    IMPLEMENT_REFCOUNTING(Client);
};

class App : public CefApp
{
public:
    // Drawn without the GPU: a still page doesn't need its process, and the game starts right after
    void OnBeforeCommandLineProcessing(const CefString& acProcessType, CefRefPtr<CefCommandLine> apCommandLine) override
    {
        if (acProcessType.empty())
        {
            apCommandLine->AppendSwitch("disable-gpu");
            apCommandLine->AppendSwitch("disable-gpu-compositing");
        }
    }

private:
    IMPLEMENT_REFCOUNTING(App);
};

LRESULT CALLBACK WindowProc(HWND aWindow, UINT aMessage, WPARAM aWParam, LPARAM aLParam)
{
    switch (aMessage)
    {
    case WM_SIZE:
        if (s_browser)
            SetWindowPos(s_browser->GetHost()->GetWindowHandle(), nullptr, 0, 0, LOWORD(aLParam), HIWORD(aLParam), SWP_NOZORDER);
        return 0;
    case WM_SETFOCUS:
        if (s_browser)
            s_browser->GetHost()->SetFocus(true);
        return 0;
    case WM_ERASEBKGND:
    {
        RECT rect{};
        GetClientRect(aWindow, &rect);
        const HBRUSH brush = CreateSolidBrush(GetBackground());
        FillRect(reinterpret_cast<HDC>(aWParam), &rect, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_DPICHANGED:
    {
        const auto* pRect = reinterpret_cast<const RECT*>(aLParam);
        SetWindowPos(aWindow, nullptr, pRect->left, pRect->top, pRect->right - pRect->left, pRect->bottom - pRect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case kCloseMessage:
        s_exitCode = static_cast<int>(aWParam);
        PostMessageW(aWindow, WM_CLOSE, 0, 0);
        return 0;
    case kDragMessage:
        // the move loop is a modal one, which CEF has to be told about
        CefSetOSModalLoop(true);
        ReleaseCapture();
        SendMessageW(aWindow, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        CefSetOSModalLoop(false);
        return 0;
    case kMinimizeMessage:
        ShowWindow(aWindow, SW_MINIMIZE);
        return 0;
    case WM_CLOSE:
        // Alt+F4 or the taskbar in the middle of an update: ending here could leave the files half swapped
        if (Updater::GetStatus().state == Updater::State::kUpdating)
            return 0;
        // the browser closes first; CEF sends WM_CLOSE again once it has, and the window goes
        if (s_browser && !s_closing)
        {
            s_closing = true;
            s_browser->GetHost()->CloseBrowser(false);
            return 0;
        }
        break;
    }
    return DefWindowProcW(aWindow, aMessage, aWParam, aLParam);
}

bool CreateLauncherWindow()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(102));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass))
        return false;

    // The page's size at the scaling of the screen the mouse is on, in the middle of it
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const RECT& work = info.rcWork;
    const int width = std::min<int>(MulDiv(kWidth, static_cast<int>(dpiX), 96), work.right - work.left);
    const int height = std::min<int>(MulDiv(kHeight, static_cast<int>(dpiX), 96), work.bottom - work.top);

    s_window = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Reclamation Day", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, work.left + (work.right - work.left - width) / 2,
                               work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!s_window)
        return false;

    // Windows 11's rounded corners
    constexpr DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr int kRound = 2;               // DWMWCP_ROUND
    DwmSetWindowAttribute(s_window, kCornerPreference, &kRound, sizeof(kRound));

    ShowWindow(s_window, SW_SHOW);
    SetForegroundWindow(s_window);
    return true;
}

int RunWindow()
{
    ComScope com;

    const auto appFolder = TiltedPhoques::GetPath();
    std::error_code ec;
    std::filesystem::create_directories(appFolder / L"logs", ec);
    try
    {
        auto logger = spdlog::basic_logger_mt("launcher", (appFolder / L"logs" / L"launcher.log").string(), true);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
    }
    catch (...)
    {
    }

    CefMainArgs args(GetModuleHandleW(nullptr));
    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = false;
    settings.log_severity = LOGSEVERITY_WARNING;
    settings.persist_session_cookies = false;
    const COLORREF background = GetBackground();
    settings.background_color = CefColorSetARGB(255, GetRValue(background), GetGValue(background), GetBValue(background));
    CefString(&settings.log_file).FromWString(appFolder / L"logs" / L"launcher_cef.log");
    CefString(&settings.root_cache_path).FromWString(appFolder / L"cache");
    CefString(&settings.cache_path).FromWString(appFolder / L"cache" / L"launcher");
    CefString(&settings.framework_dir_path).FromWString(appFolder);
    CefString(&settings.resources_dir_path).FromWString(appFolder);
    CefString(&settings.locales_dir_path).FromWString(appFolder / L"locales");
    CefString(&settings.browser_subprocess_path).FromWString(appFolder / L"TPProcess.exe");

    CefRefPtr<App> app = new App;
    if (!CefInitialize(args, settings, app, nullptr))
    {
        spdlog::error("Launcher: CEF didn't start");
        return kExitFailed;
    }
    CefRegisterSchemeHandlerFactory("https", "launcher.rd", new Factory);

    if (!CreateLauncherWindow())
    {
        spdlog::error("Launcher: the window couldn't be made (error {})", GetLastError());
        CefShutdown();
        return kExitFailed;
    }
    StartApi(s_window);

    RECT rect{};
    GetClientRect(s_window, &rect);
    CefWindowInfo windowInfo;
    windowInfo.SetAsChild(s_window, CefRect(0, 0, rect.right, rect.bottom));
    CefBrowserSettings browserSettings;
    browserSettings.background_color = settings.background_color;
    if (!CefBrowserHost::CreateBrowser(windowInfo, new Client, std::string(kOrigin) + "index.html", browserSettings, nullptr, nullptr))
    {
        spdlog::error("Launcher: the page couldn't be opened");
        DestroyWindow(s_window);
        CefShutdown();
        return kExitFailed;
    }

    spdlog::info("Launcher: the window is up");
    CefRunMessageLoop();
    if (IsWindow(s_window))
        DestroyWindow(s_window);
    CefShutdown();
    spdlog::info("Launcher: closed, {}", s_exitCode == kExitPlay ? "play" : s_exitCode == kExitUpdated ? "updated" : "quit");
    return s_exitCode;
}
} // namespace

Choice Show() noexcept
{
    const auto appFolder = TiltedPhoques::GetPath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(appFolder / L"launcher" / L"index.html", ec) || !std::filesystem::is_regular_file(appFolder / L"libcef.dll", ec))
        return Choice::kUnavailable;

    wchar_t exe[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" --launcher-ui";
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, appFolder.c_str(), &startupInfo, &processInfo))
        return Choice::kUnavailable;

    CloseHandle(processInfo.hThread);
    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = kExitFailed;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hProcess);

    // The window's process has gone and this one still runs the old version: it starts the new one and ends, the way the
    // main menu's update does
    if (exitCode == kExitUpdated)
        Updater::Restart();
    if (exitCode == kExitPlay)
        return Choice::kPlay;
    if (exitCode == kExitQuit)
        return Choice::kQuit;
    return Choice::kUnavailable;
}
#else
Choice Show() noexcept
{
    return Choice::kUnavailable;
}
#endif

int RunIfAsked(int aArgc, char** aArgv)
{
    for (int i = 1; i < aArgc; ++i)
    {
        if (std::strcmp(aArgv[i], "--launcher-ui") == 0)
        {
#if defined(TARGET_FT)
            return RunWindow();
#else
            return kExitFailed;
#endif
        }
    }
    return -1;
}
} // namespace launcher::ui

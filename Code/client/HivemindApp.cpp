#include <HivemindPCH.h>

#include <HivemindApp.h>

#include <DInputHook.hpp>
#include <WindowsHook.hpp>

#include <World.h>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Systems/RenderSystemD3D11.h>

#include <Services/OverlayService.h>
#include <Services/Vats76Service.h>
#include <Services/ImguiService.h>
#include <Services/DiscordService.h>

#include <ScriptExtender.h>
#include <NvidiaUtil.h>

#include <ShlObj.h>
#if TP_FALLOUT4
#include <Interface/MainMenuIntegration.h>
#include <Interface/UI.h>
#include <Interface/Updater.h>
#include <BundledMods.h>
#include <Services/BuildToolsService.h>
#include <Services/SteamService.h>
#include <SaveFolder.h>
#endif
#include <thread>
#include <Psapi.h>

using TiltedPhoques::Debug;

#if TP_FALLOUT4
namespace
{
// Whether another game instance was already running when this one started; it then plays as "player 2" with its own
// saves. Two instances on one PC shared the save folder, and one could load a world the other had just saved (a
// remote player's power armor frame and its fusion core went missing from both).
bool s_isSecondInstance = false;

void DetectSecondInstance() noexcept
{
    // Kept open for the whole process: the first instance owns it. The error is read right away, before anything else
    // (like a static's initialization guard) can overwrite it.
    HANDLE hInstanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\ReclamationDayFirstInstance");
    const DWORD error = GetLastError();
    s_isSecondInstance = hInstanceMutex && error == ERROR_ALREADY_EXISTS;
}

// Gives the second instance its own save folder (Saves\Player2\), seeded with the newest saves of the main folder so
// it has a character to load. Changes sLocalSavePath:General in memory only, the INI files are untouched.
void UseSecondInstanceSaves() noexcept
{
    const std::string mainFolder = SaveFolder::GetLocalSavePath();
    const std::string playerFolder = mainFolder + "Player2\\";
    const auto userFolder = SaveFolder::GetGameUserFolder();

    // Seed the new folder with the newest saves if it has none
    if (SaveFolder::ListSaves(userFolder / playerFolder).empty())
    {
        const auto saves = SaveFolder::ListSaves(userFolder / mainFolder);
        for (size_t i = 0; i < saves.size() && i < 3; ++i)
            SaveFolder::CopySave(saves[i], userFolder / playerFolder);
    }

    if (SaveFolder::SetLocalSavePath(playerFolder))
        spdlog::info("Second game instance: saving to {} instead of {}", playerFolder, mainFolder);
}
} // namespace
#endif

namespace
{
// Each game instance on the PC writes its own log: tp_client.log, tp_client_instance_2.log, ... (upstream f765aa6d).
// A slot is a named mutex, held until the process ends.
std::filesystem::path GetClientLogName() noexcept
{
    for (uint32_t instance = 1; instance < 16; ++instance)
    {
        const std::wstring mutexName = L"Local\\ReclamationDay.ClientLog.Instance_" + std::to_wstring(instance);
        const HANDLE hMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        const DWORD error = GetLastError();
        if (!hMutex)
            break;

        if (error == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(hMutex);
            continue;
        }

        return instance == 1 ? "tp_client.log" : "tp_client_instance_" + std::to_string(instance) + ".log";
    }

    // Never a name another instance may be writing to
    return "tp_client_pid_" + std::to_string(GetCurrentProcessId()) + ".log";
}

// The graphics libraries the game runs: a graphics mod's copy from its folder (ENB as d3d11.dll, ReShade as dxgi.dll),
// which loads Windows' own as well, or only Windows' own
void LogGraphicsLibraries() noexcept
{
    HMODULE modules[1024]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return;

    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(modules)));
    for (DWORD i = 0; i < count; ++i)
    {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH))
            continue;

        const std::filesystem::path file(path);
        const std::wstring name = file.filename().wstring();
        if (_wcsicmp(name.c_str(), L"d3d11.dll") == 0 || _wcsicmp(name.c_str(), L"dxgi.dll") == 0)
            spdlog::info("Graphics: {} from {}", file.filename().string(), file.parent_path().string());
    }
}
} // namespace

HivemindApp::HivemindApp()
{
#if TP_FALLOUT4
    DetectSecondInstance();
#endif

    // Set console code page to UTF-8 so console known how to interpret string data
    SetConsoleOutputCP(CP_UTF8);

    auto logPath = TiltedPhoques::GetPath() / "logs";

    std::error_code ec;
    create_directory(logPath, ec);

    auto rotatingLogger = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPath / GetClientLogName(), 1048576 * 5, 3);
    // rotatingLogger->set_level(spdlog::level::debug);
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern("%^[%H:%M:%S.%e] [%l] [tid %t] %$ %v");

    // Written by a thread of its own: the game's threads only hand lines over and never wait for the drive (on a busy
    // USB drive every line took ~200 ms of the frame, 2026-10-16). On disk within a second, at once for warnings and
    // errors, so the log stays readable while the game runs; crashes wait for it (CrashHandler::FlushLog).
    spdlog::init_thread_pool(8192, 1);
    auto logger = std::make_shared<spdlog::async_logger>("", spdlog::sinks_init_list{console, rotatingLogger}, spdlog::thread_pool(),
                                                         spdlog::async_overflow_policy::block);
    logger->flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(1));
    set_default_logger(logger);
}

HivemindApp::~HivemindApp() = default;

void* HivemindApp::GetMainAddress() const
{
    POINTER_SKYRIMSE(void, winMain, 36544);
    POINTER_FALLOUT4(void, winMain, 668529);

    return winMain.GetPtr();
}

bool HivemindApp::BeginMain()
{
    World::Create();
    World::Get().ctx().at<DiscordService>().Init();
    World::Get().ctx().emplace<RenderSystemD3D11>(World::Get().ctx().at<OverlayService>(), World::Get().ctx().at<ImguiService>());

#if TP_FALLOUT4
    // Before the script extender loads the mods, which read their settings then
    BundledMods::ApplyForcedSettings();
    // Before the game reads its load order
    Updater::InstallOurPlugins();
#endif
    LoadScriptExender();

    if (IsNvidiaOverlayLoaded())
        ApplyNvidiaFix();

    LogGraphicsLibraries();

    return true;
}

bool HivemindApp::EndMain()
{
    UninstallHooks();
    if (m_pDevice)
        m_pDevice->Release();

    return true;
}

void HivemindApp::Update()
{
    // Every frame make sure we won't use preprocessed facegen
    POINTER_SKYRIMSE(uint32_t, bUseFaceGenPreprocessedHeads, 378620);
    POINTER_FALLOUT4(uint32_t, bUseFaceGenPreprocessedHeads, 196397);

    *bUseFaceGenPreprocessedHeads = 0;

    KeepRunningInBackground();

    World::Get().Update();
}

void HivemindApp::KeepRunningInBackground() noexcept
{
    POINTER_SKYRIMSE(uint32_t, bAlwaysActive, 380768);
    POINTER_FALLOUT4(uint32_t, bAlwaysActive, 1420078);

    *bAlwaysActive = 1;
}

#if TP_FALLOUT4
void HivemindApp::OnMainLoop() noexcept
{
    {
        // the freeze watchdog gives a frame behind the loading screen far longer before it counts the game as stuck
        bool loading = false;
        if (const UI* pUI = UI::Get())
        {
            static const BSFixedString s_loadingMenu("LoadingMenu");
            loading = pUI->GetMenuOpen(s_loadingMenu);
        }
        CrashHandler::OnFrame(loading);
    }
    s_frameCount.fetch_add(1, std::memory_order_relaxed);

    // Update() only runs in the world, this keeps the main menu running in the background too
    KeepRunningInBackground();

    // Before the main menu's actions: a save the player chose to play starts loading as soon as the game's data is
    // loaded, settlements must be known by then
    UncombineSettlementCells();
    RunMainMenuIntegration();
    // Steam friends: the social sidebar, invites, friends joining through Steam (menus included)
    World::Get().ctx().at<SteamService>().OnMainLoop();
    Vats76Service::OnMainLoop();

    // Once, on the game's first frame: the INI files have been read and the main menu hasn't listed the saves yet.
    // Not in Update(), that only runs while the script VM is active, i.e. not at the main menu.
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;

    spdlog::info("Game instance: {}", s_isSecondInstance ? "second (player 2 saves)" : "first");
    if (s_isSecondInstance)
        UseSecondInstanceSaves();

    // Here rather than at startup: the log exists by now, and dumps of a crash right before aren't locked anymore
    char modulePath[MAX_PATH];
    GetModuleFileNameA(NULL, modulePath, sizeof(modulePath));
    const auto folder = std::filesystem::path(modulePath).parent_path();
    std::thread([folder] { CrashHandler::CompressDumps(folder); }).detach();
}
#endif

bool HivemindApp::Attach()
{
    TiltedPhoques::Debug::OnAttach();

    // TiltedPhoques::Nop(0x1405D3FA1, 6);
    return true;
}

bool HivemindApp::Detach()
{
    TiltedPhoques::Debug::OnDetach();
    return true;
}

void HivemindApp::InstallHooks2()
{
    TiltedPhoques::Initializer::RunAll();

    TiltedPhoques::DInputHook::Install();
}

void HivemindApp::UninstallHooks()
{
}

void HivemindApp::ApplyNvidiaFix() noexcept
{
    auto d3dFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = CreateEarlyDxDevice(m_pDevice, &d3dFeatureLevel);
    if (FAILED(hr))
        spdlog::error("D3D11CreateDevice failed. Detected an NVIDIA GPU, error code={0:x}", hr);

    if (d3dFeatureLevel < D3D_FEATURE_LEVEL_11_0)
        spdlog::warn("Unexpected D3D11 feature level detected (< 11.0), may cause issues");
}

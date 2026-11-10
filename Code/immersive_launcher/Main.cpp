
#include "Utils/Error.h"
#include "launcher.h"
#include <Windows.h>
#include <combaseapi.h>
#include <base/threading/ThreadUtils.h>

#include "utils/ComUtils.h"
#include "script_extender/SEMemoryBlock.h"
#include "ui/LauncherUi.h"

#include <TiltedCore/Platform.hpp>

extern void CoreStubsInit();
// The game's freeze watchdog starts this exe again to write the frozen game's dump from outside it
// (client/CrashHandler.cpp): -1 when the command line isn't that
extern int RunDumpWriter(int argc, char** argv);

extern "C"
{
    __declspec(dllexport) int NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

auto kSystemPreloadDlls = {
    L"\\dinput8.dll",   // < Skyrim early init hook
    L"\\dsound.dll",    // < breaks DSound init in game code
    L"\\nvspcap64.dll", // < Nvidia overlay, needs to be loaded before d3d11 device creation
                        // < X360CE v3 is buggy with COM hooks
    L"\\xinput9_1_0.dll", L"\\xinput1_1.dll", L"\\xinput1_2.dll", L"\\xinput1_3.dll", L"\\xinput1_4.dll", L"\\version.dll"};

static void PreloadSystemDlls()
{
    auto loadSystemDll = [](auto dll)
    {
        wchar_t systemPath[512];
        GetSystemDirectoryW(systemPath, _countof(systemPath));

        wcscat_s(systemPath, dll);

        LoadLibraryW(systemPath);
    };

    for (auto dll : kSystemPreloadDlls)
        loadSystemDll(dll);
}

// After an update (or a campaign switch) the old launcher starts the new one and ends: wait until it's gone, or this one
// would take itself for a second game instance (and the old one may still hold files). True when it was such a restart.
static bool WaitForPreviousLauncher()
{
    wchar_t pidText[32]{};
    if (!GetEnvironmentVariableW(L"RD_WAIT_PID", pidText, static_cast<DWORD>(std::size(pidText))))
        return false;
    SetEnvironmentVariableW(L"RD_WAIT_PID", nullptr);

    if (const HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(pidText, nullptr, 10)))
    {
        WaitForSingleObject(hProcess, 30000);
        CloseHandle(hProcess);
    }
    return true;
}

// A restart for a campaign switch or a load order matched to a host's comes straight back to the game (RD_SKIP_LAUNCHER,
// Updater::Restart); one after an update shows the launcher's window like any start. Read once and cleared, so a later
// restart of this process doesn't inherit it.
static bool SkipWindowOnce()
{
    wchar_t value[8]{};
    const bool skip = GetEnvironmentVariableW(L"RD_SKIP_LAUNCHER", value, static_cast<DWORD>(std::size(value))) > 0;
    SetEnvironmentVariableW(L"RD_SKIP_LAUNCHER", nullptr);
    return skip;
}

int main(int argc, char** argv)
{
    if (const int result = RunDumpWriter(argc, argv); result >= 0)
        return result;
    // The launcher's window, started by the launcher below as a process of its own
    if (const int result = launcher::ui::RunIfAsked(argc, argv); result >= 0)
        return result;

    const bool skipWindow = SkipWindowOnce();
    const bool restarted = WaitForPreviousLauncher();

    Base::SetCurrentThreadName("MainLauncherThread");

    // memory block for Script Extender reserved as early as we can
    script_extender::SEMemoryBlock b;
    if (!b.Good())
    {
        Die(L"Failed to pre-reserve script extender zone.\nAsk Force!");
        return -1;
    }

    PreloadSystemDlls();
    CoreStubsInit();

    ComScope cs;
    TP_UNUSED(cs);

    // No crash reporter here: Tilted Evolution's one uploads crash reports to its own team's tracker. The client writes
    // its own crash dumps, which stay on the player's PC (CrashHandler.cpp).
    auto ret = launcher::StartUp(argc, argv, restarted && skipWindow);

    return ret;
}

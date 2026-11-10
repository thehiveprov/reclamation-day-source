
#include <HivemindApp.h>
#include <HivemindPCH.h>

#include <Commctrl.h>
#include <Windows.h>

#include <base/dialogues/win/TaskDialog.h>

std::unique_ptr<HivemindApp> g_appInstance{nullptr};

extern HICON g_SharedWindowIcon;

static void ShowAddressLibraryError(const wchar_t* apGamePath)
{
#if TP_SKYRIM64
    auto errorDetail = fmt::format(L"Looking for it here: {}\\Data\\SKSE\\Plugins", apGamePath);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Failed to load Skyrim Address Library", L"Make sure to use \"All in one (1.6.X)\"", errorDetail.c_str());
#elif TP_FALLOUT4
    auto errorDetail = fmt::format(L"Looking for it here: {}\\Data\\F4SE\\Plugins", apGamePath);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Failed to load Fallout 4 Address Library", L"", errorDetail.c_str());
#endif

    dia.AppendButton(0xBEED, L"Visit troubleshooting page on wiki.tiltedphoques.com");
    dia.AppendButton(0xBEEF, L"Visit Address Library modpage on nexusmods.com");
    const int result = dia.Show();
    if (result == 0xBEEF)
    {
#if TP_SKYRIM64
        ShellExecuteW(nullptr, L"open", LR"(https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)", nullptr, nullptr, SW_SHOWNORMAL);
#elif TP_FALLOUT4
        ShellExecuteW(nullptr, L"open", LR"(https://www.nexusmods.com/fallout4/mods/47327)", nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    else if (result == 0xBEED)
    {
        ShellExecuteW(nullptr, L"open", LR"(https://wiki.tiltedphoques.com/tilted-online/guides/troubleshooting/address-library-error)", nullptr, nullptr, SW_SHOWNORMAL);
    }

    exit(4);
}

#if TP_FALLOUT4
// LoadFallout4() always reads the 1.10.163 address library, so any other exe would run with wrong addresses
static void EnsureSupportedFallout4Version(const String& aExeVersion)
{
    constexpr char kSupportedVersion[] = "1.10.163";
    if (aExeVersion.starts_with(kSupportedVersion))
        return;

    auto errorDetail = fmt::format(L"Found Fallout4.exe version {}. Downgrade the game to 1.10.163 to play.", std::wstring(aExeVersion.begin(), aExeVersion.end()));
    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Unsupported Fallout 4 version", L"Fallout 4: Reclamation Day requires Fallout 4 1.10.163", errorDetail.c_str());
    dia.Show();

    exit(5);
}
#endif

void RunTiltedInit(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
#if TP_FALLOUT4
    EnsureSupportedFallout4Version(aExeVersion);
#endif

#if TP_SKYRIM64
    if (!VersionDb::Get().Load(acGamePath, aExeVersion))
#else
    if (!VersionDb::Get().LoadFallout4(acGamePath))
#endif
    {
        ShowAddressLibraryError(acGamePath.c_str());
    }

    // VersionDb::Get().DumpToTextFile(R"(S:\Work\Tilted\fallout\_addresslib.txt)");

    g_appInstance = std::make_unique<HivemindApp>();

    HivemindApp::InstallHooks2();
    TP_HOOK_COMMIT;
}

void RunTiltedApp()
{
    g_appInstance->BeginMain();
}

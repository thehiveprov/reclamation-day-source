// Copyright (C) 2021 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <shobjidl.h>
#include <vector>

#include "TargetConfig.h"
#include "launcher.h"
#include "oobe/PathSelection.h"

#include "Utils/FileVersion.inl"
#include "utils/ComUtils.h"
#include "utils/Registry.h"

namespace oobe
{
using namespace TiltedPhoques;
namespace fs = std::filesystem;

namespace
{
constexpr wchar_t kBethesdaRegistryPath[] = LR"(Software\Wow6432Node\Bethesda Softworks\)" SHORT_NAME;
constexpr wchar_t kTiltedRegistryPath[] = LR"(Software\TiltedPhoques\TiltedEvolution\)" SHORT_NAME;
constexpr wchar_t kTargetExe[] = TARGET_NAME L".exe";

std::wstring SuggestTitlePath()
{
    return Registry::ReadString<wchar_t>(HKEY_LOCAL_MACHINE, kBethesdaRegistryPath, L"installed path");
}

std::wstring Widen(const std::string& acText)
{
    if (acText.empty())
        return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), result.data(), size);
    return result;
}

// The value of a `"key"  "value"` line in a Steam .vdf/.acf file, whose backslashes are escaped
std::optional<std::string> ReadVdfValue(const std::string& acLine, std::string_view aKey)
{
    const std::string quotedKey = "\"" + std::string(aKey) + "\"";
    const auto keyPos = acLine.find(quotedKey);
    if (keyPos == std::string::npos)
        return std::nullopt;

    const auto start = acLine.find('"', keyPos + quotedKey.size());
    if (start == std::string::npos)
        return std::nullopt;

    std::string value;
    for (auto i = start + 1; i < acLine.size() && acLine[i] != '"'; ++i)
    {
        if (acLine[i] == '\\' && i + 1 < acLine.size())
            ++i;
        value += acLine[i];
    }
    return value;
}

// The game's folder in every Steam library it is installed in (libraryfolders.vdf lists the libraries, each
// library's appmanifest_<app id>.acf names the folder under steamapps\common)
std::vector<fs::path> FindSteamInstalls()
{
    std::wstring steamPath = Registry::ReadString<wchar_t>(HKEY_CURRENT_USER, LR"(Software\Valve\Steam)", L"SteamPath");
    if (steamPath.empty())
        steamPath = Registry::ReadString<wchar_t>(HKEY_LOCAL_MACHINE, LR"(Software\Wow6432Node\Valve\Steam)", L"InstallPath");
    if (steamPath.empty())
        return {};

    std::vector<fs::path> libraries{fs::path(steamPath)};
    std::ifstream libraryFolders(fs::path(steamPath) / L"steamapps" / L"libraryfolders.vdf");
    for (std::string line; std::getline(libraryFolders, line);)
    {
        if (auto path = ReadVdfValue(line, "path"))
            libraries.emplace_back(Widen(*path));
    }

    std::vector<fs::path> installs;
    const auto manifestName = L"appmanifest_" + std::to_wstring(CurrentTarget.steamAppId) + L".acf";
    for (const auto& library : libraries)
    {
        std::ifstream manifest(library / L"steamapps" / manifestName);
        for (std::string line; std::getline(manifest, line);)
        {
            if (auto installDir = ReadVdfValue(line, "installdir"))
            {
                installs.push_back(library / L"steamapps" / L"common" / Widen(*installDir));
                break;
            }
        }
    }
    return installs;
}

std::wstring GetGameVersion(const fs::path& acExe)
{
    const auto version = QueryFileVersion(acExe.c_str());
    return std::wstring(version.begin(), version.end());
}

bool IsSupportedGameVersion(const fs::path& acExe)
{
#if defined(SUPPORTED_GAME_VERSION)
    return GetGameVersion(acExe).starts_with(L"" SUPPORTED_GAME_VERSION);
#else
    return true;
#endif
}

// The game exe from where Bethesda's and Steam's install records put the game, if it is a version we can run
std::optional<fs::path> DetectGameExe()
{
    std::vector<fs::path> folders;
    if (auto suggestion = SuggestTitlePath(); !suggestion.empty())
        folders.emplace_back(suggestion);
    for (auto& install : FindSteamInstalls())
        folders.push_back(std::move(install));

    for (const auto& folder : folders)
    {
        std::error_code ec;
        const auto exe = (folder / kTargetExe).lexically_normal();
        if (fs::is_regular_file(exe, ec) && IsSupportedGameVersion(exe))
            return exe;
    }

    return std::nullopt;
}

// Why the picked file can't be used, empty when it can
std::wstring CheckPickedExe(const fs::path& acExe)
{
    if (_wcsicmp(acExe.filename().c_str(), kTargetExe) != 0)
        return L"That isn't " TARGET_NAME L".exe. Select " TARGET_NAME L".exe in the game's install folder.";

    if (acExe.parent_path() == TiltedPhoques::GetPath())
        return L"That's the " PRODUCT_NAME L" folder. Select " TARGET_NAME L".exe in the game's install folder.";

#if defined(SUPPORTED_GAME_VERSION)
    if (!IsSupportedGameVersion(acExe))
    {
        auto version = GetGameVersion(acExe);
        if (version.empty())
            version = L"unknown";
        return L"This " TARGET_NAME L".exe is version " + version + L". " PRODUCT_NAME L" needs version " SUPPORTED_GAME_VERSION
               L". Downgrade the game, or select a downgraded copy.";
    }
#endif

    return {};
}

std::optional<std::wstring> OpenPathSelectionDialog2(const std::wstring& aPathSuggestion)
{
    // in case some other stuff already tried to unregister the global instance
    ComScope _;

    ComPtr<IFileOpenDialog> pFileDialog;
    auto hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(pFileDialog.GetAddressOf()));
    if (FAILED(hr))
        return std::nullopt;

    pFileDialog->SetTitle(L"Select the game executable (" TARGET_NAME L".exe)");

    // every program as a second choice, so the game can be picked even when the exact name filter hides it
    static constinit COMDLG_FILTERSPEC rgSpec[] = {{TARGET_NAME L".exe", TARGET_NAME L".exe"}, {L"All programs", L"*.exe"}};
    pFileDialog->SetFileTypes(2, &rgSpec[0]);

    DWORD options = 0;
    if (SUCCEEDED(pFileDialog->GetOptions(&options)))
        pFileDialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);

    // pre select suggested folder & exe
    if (!aPathSuggestion.empty())
    {
        ComPtr<IShellItem> pItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(aPathSuggestion.c_str(), nullptr, IID_PPV_ARGS(&pItem))))
        {
            pFileDialog->SetFolder(pItem.Get());
            pFileDialog->SetFileName(TARGET_NAME L".exe");
        }
    }

    hr = pFileDialog->Show(nullptr);
    if (FAILED(hr))
        return std::nullopt;

    ComPtr<IShellItem> pItem;
    hr = pFileDialog->GetResult(pItem.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        PWSTR filePath;
        hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);

        if (SUCCEEDED(hr))
        {
            auto rval = filePath;
            CoTaskMemFree(filePath);
            return rval;
        }
    }

    return std::nullopt;
}

bool SavePaths(const std::wstring& acTitlePath, const std::wstring& acExePath)
{
    return Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitlePath", acTitlePath) && Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitleExe", acExePath);
}
} // namespace

GameInstall DescribeInstall(const fs::path& acExe, std::wstring aSource)
{
    GameInstall install{acExe, std::move(aSource), GetGameVersion(acExe), IsSupportedGameVersion(acExe)};
    // "1.10.163.0" as the game's own version reads
    if (install.version.ends_with(L".0") && std::count(install.version.begin(), install.version.end(), L'.') == 3)
        install.version.resize(install.version.size() - 2);
    return install;
}

std::vector<GameInstall> FindGameInstalls()
{
    std::vector<std::pair<fs::path, std::wstring>> folders;
    for (auto& install : FindSteamInstalls())
    {
        const auto drive = install.root_name().wstring();
        folders.emplace_back(std::move(install), drive.empty() ? L"Steam library" : L"Steam library on " + drive);
    }
    if (auto suggestion = SuggestTitlePath(); !suggestion.empty())
        folders.emplace_back(suggestion, L"Installed copy");

    std::vector<GameInstall> installs;
    for (const auto& [folder, source] : folders)
    {
        std::error_code ec;
        const auto exe = (folder / kTargetExe).lexically_normal();
        if (!fs::is_regular_file(exe, ec))
            continue;
        const bool known = std::any_of(installs.begin(), installs.end(), [&exe](const GameInstall& acOther) { return _wcsicmp(acOther.exe.c_str(), exe.c_str()) == 0; });
        if (!known)
            installs.push_back(DescribeInstall(exe, source));
    }

    return installs;
}

std::optional<GameInstall> GetChosenInstall()
{
    const fs::path exe = Registry::ReadString<wchar_t>(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitleExe");
    std::error_code ec;
    if (exe.empty() || !fs::is_regular_file(exe, ec))
        return std::nullopt;

    for (auto& install : FindGameInstalls())
    {
        if (_wcsicmp(install.exe.c_str(), exe.lexically_normal().c_str()) == 0)
            return install;
    }
    return DescribeInstall(exe.lexically_normal(), L"Picked by hand");
}

std::wstring CheckGameExe(const fs::path& acExe)
{
    return CheckPickedExe(acExe);
}

bool SaveChosenInstall(const fs::path& acExe)
{
    return SavePaths(acExe.parent_path().wstring(), acExe.wstring());
}

bool SelectInstall(bool aForceSelect)
{
    // separate, so a custom executable can be chosen for TP
    auto titlePath = Registry::ReadString<wchar_t>(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitlePath");
    auto exePath = Registry::ReadString<wchar_t>(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitleExe");

    // first start: take the game from its install records without asking, the picker is the fallback
    if (!aForceSelect && (!std::filesystem::exists(titlePath) || !std::filesystem::exists(exePath)))
    {
        if (const auto exe = DetectGameExe())
        {
            exePath = exe->wstring();
            titlePath = exe->parent_path().wstring();
            SavePaths(titlePath, exePath);
        }
    }

    bool result = true;
    if (!std::filesystem::exists(titlePath) || !std::filesystem::exists(exePath) || aForceSelect)
    {
        constexpr int kSelectionAttempts = 3;

        for (int i = 0; i < kSelectionAttempts; i++)
        {
            if (auto path = OpenPathSelectionDialog2(SuggestTitlePath()))
            {
                if (const auto problem = CheckPickedExe(*path); !problem.empty())
                {
                    MessageBoxW(nullptr, problem.c_str(), PRODUCT_NAME, MB_OK | MB_ICONWARNING);
                    continue;
                }

                titlePath = fs::path(*path).parent_path().wstring();
                exePath = (*path);

                // if this fails, we try again
                result = SavePaths(titlePath, exePath);

                if (result)
                    break;
            }
            else
                break;
        }
    }

    // picker closed without a usable game
    if (!std::filesystem::exists(titlePath) || !std::filesystem::exists(exePath))
        return false;

    if (result)
    {
        auto* apContext = launcher::GetLaunchContext();
        apContext->gamePath = titlePath;
        apContext->exePath = exePath;
    }

    return result;
}
} // namespace oobe

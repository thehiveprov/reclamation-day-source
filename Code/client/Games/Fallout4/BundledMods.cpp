#include <BundledMods.h>

#include <filesystem>

namespace
{
struct ForcedSetting
{
    // The mod's plugin, by the start of its file name (its builds are named VATSUnchained.dll, VATSUnchainedRD.dll)
    const wchar_t* Plugin;
    // Its MCM settings file, in Data
    const wchar_t* SettingsFile;
    const wchar_t* Section;
    const wchar_t* Key;
    const wchar_t* Value;
};

constexpr ForcedSetting kForcedSettings[] = {
    // A player who still has VATS Unchained (RD VATS 76' stays off while it's there, and its setting could be anything):
    // realtime VATS at full speed, as in Fallout 76. Every game of a session runs at the same speed, one slowed down for
    // its player would fall out of step with the others
    {L"VATSUnchained", L"MCM\\Settings\\VATSUnchained.ini", L"Realtime", L"fTimePercent", L"100.0"},
};

bool IsInstalled(const std::filesystem::path& acPlugins, const wchar_t* apPlugin) noexcept
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it(acPlugins, ec), end; !ec && it != end; it.increment(ec))
    {
        const auto name = it->path().filename().wstring();
        if (it->path().extension() == L".dll" && _wcsnicmp(name.c_str(), apPlugin, wcslen(apPlugin)) == 0)
            return true;
    }
    return false;
}
} // namespace

namespace BundledMods
{
void ApplyForcedSettings() noexcept
{
    // The game's folder, as the script extender is found (ScriptExtender.cpp)
    const auto data = std::filesystem::current_path() / L"Data";
    const auto plugins = data / L"F4SE" / L"Plugins";

    for (const auto& setting : kForcedSettings)
    {
        if (!IsInstalled(plugins, setting.Plugin))
            continue;

        const auto file = data / setting.SettingsFile;
        wchar_t current[64]{};
        GetPrivateProfileStringW(setting.Section, setting.Key, L"", current, static_cast<DWORD>(std::size(current)), file.c_str());
        if (wcscmp(current, setting.Value) == 0)
            continue;

        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (WritePrivateProfileStringW(setting.Section, setting.Key, setting.Value, file.c_str()))
            spdlog::info(L"Bundled mods: {} [{}] {} set to {} (was '{}')", setting.SettingsFile, setting.Section, setting.Key, setting.Value, current);
        else
            spdlog::warn(L"Bundled mods: couldn't set {} [{}] {} (error {})", setting.SettingsFile, setting.Section, setting.Key, GetLastError());
    }
}
} // namespace BundledMods

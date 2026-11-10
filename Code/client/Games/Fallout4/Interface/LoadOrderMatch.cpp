#include <HivemindPCH.h>

#include <Interface/LoadOrderMatch.h>
#include <Interface/Updater.h>

#include <Services/OverlayService.h>
#include <MiniJson.h>
#include <World.h>

#include <ShlObj.h>

#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace LoadOrderMatch
{
namespace
{
std::mutex s_lock;
std::optional<std::string> s_queued;

// Loaded by the game itself, before plugins.txt's: the official masters (DLCUltraHighResolution.esm has only textures,
// the servers don't count it)
constexpr const char* kOfficialPlugins[]{
    "fallout4.esm", "dlcrobot.esm", "dlcworkshop01.esm", "dlccoast.esm", "dlcworkshop02.esm", "dlcworkshop03.esm", "dlcnukaworld.esm",
};
constexpr char kAssetOnlyPlugin[] = "dlcultrahighresolution.esm";

std::string ToLower(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return aText;
}

std::string Trim(const std::string& acText)
{
    const auto first = acText.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return acText.substr(first, acText.find_last_not_of(" \t\r\n") - first + 1);
}

std::filesystem::path GetGameFolder()
{
    wchar_t path[MAX_PATH * 2]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TiltedPhoques\\TiltedEvolution\\Fallout4", L"TitlePath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return {};
    return path;
}

std::filesystem::path GetPluginsFile()
{
    PWSTR pLocalAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pLocalAppData)))
        return {};
    std::filesystem::path path = std::filesystem::path(pLocalAppData) / L"Fallout4" / L"plugins.txt";
    CoTaskMemFree(pLocalAppData);
    return path;
}

// The plugins the game loads by itself: the official masters, and the Creation Club content listed in Fallout4.ccc
std::set<std::string> GetImplicitPlugins(const std::filesystem::path& acGameFolder)
{
    std::set<std::string> plugins(std::begin(kOfficialPlugins), std::end(kOfficialPlugins));
    std::ifstream ccc(acGameFolder / L"Fallout4.ccc");
    for (std::string line; std::getline(ccc, line);)
    {
        line = Trim(line);
        if (!line.empty())
            plugins.insert(ToLower(line));
    }
    return plugins;
}

void SendResult(const char* apReason, const std::vector<std::string>& acPlugins)
{
    std::string plugins;
    for (const auto& plugin : acPlugins)
        plugins += (plugins.empty() ? "" : ",") + MiniJson::Quote(plugin);
    World::Get().ctx().at<OverlayService>().SendEvent("loadOrderResult", fmt::format(R"({{"ok":false,"reason":"{}","plugins":[{}]}})", apReason, plugins));
}

void Match(const std::string& acHostOrder)
{
    std::vector<std::string> hostOrder;
    {
        std::istringstream lines(acHostOrder);
        for (std::string line; std::getline(lines, line);)
        {
            line = Trim(line);
            if (!line.empty() && ToLower(line) != kAssetOnlyPlugin)
                hostOrder.push_back(line);
        }
    }

    const auto gameFolder = GetGameFolder();
    const auto pluginsFile = GetPluginsFile();
    std::error_code ec;
    if (gameFolder.empty() || pluginsFile.empty() || !std::filesystem::exists(pluginsFile, ec))
    {
        spdlog::warn("Load order: no plugins.txt to change ({})", pluginsFile.string());
        return SendResult("no_plugins_txt", {});
    }

    const auto data = gameFolder / L"Data";
    const auto implicit = GetImplicitPlugins(gameFolder);

    // The host's plugins that aren't installed here: only their authors' downloads can fix that
    std::vector<std::string> missing;
    std::set<std::string> hostSet;
    for (const auto& plugin : hostOrder)
    {
        hostSet.insert(ToLower(plugin));
        // A name, never a path: the list comes from another player's game
        const bool isName = plugin.find_first_of("\\/:") == std::string::npos && plugin.find("..") == std::string::npos;
        if (!isName || !std::filesystem::exists(data / plugin, ec))
            missing.push_back(plugin);
    }
    if (!missing.empty())
    {
        spdlog::info("Load order: can't match the host's, {} plugin(s) aren't installed", missing.size());
        return SendResult("missing", missing);
    }

    // Plugins the game loads by itself that the host doesn't have: plugins.txt can't switch those off
    std::vector<std::string> fixed;
    for (const auto& plugin : implicit)
    {
        if (!hostSet.contains(plugin) && std::filesystem::exists(data / plugin, ec))
            fixed.push_back(plugin);
    }
    if (!fixed.empty())
    {
        spdlog::info("Load order: can't match the host's, {} plugin(s) are loaded by the game itself", fixed.size());
        return SendResult("fixed", fixed);
    }

    // The new plugins.txt: its comments, the host's plugins on in the host's order, everything else it listed off
    std::vector<std::string> comments, others;
    {
        std::ifstream file(pluginsFile);
        for (std::string line; std::getline(file, line);)
        {
            if (line.starts_with("\xEF\xBB\xBF")) // UTF-8 byte order mark
                line.erase(0, 3);
            line = Trim(line);
            if (line.empty())
                continue;
            if (line[0] == '#')
            {
                comments.push_back(line);
                continue;
            }
            const std::string name = line[0] == '*' ? line.substr(1) : line;
            if (!hostSet.contains(ToLower(name)) && !implicit.contains(ToLower(name)))
                others.push_back(name);
        }
    }

    std::string text;
    for (const auto& line : comments)
        text += line + "\r\n";
    for (const auto& plugin : hostOrder)
    {
        if (!implicit.contains(ToLower(plugin)))
            text += "*" + plugin + "\r\n";
    }
    for (const auto& plugin : others)
        text += plugin + "\r\n";

    // The old one stays next to it
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &time);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const auto backup = pluginsFile.parent_path() / fmt::format("plugins.txt.{}.reclamationday-backup", stamp);
    if (!std::filesystem::copy_file(pluginsFile, backup, ec))
    {
        spdlog::error("Load order: couldn't back up plugins.txt ({}), not changing it", ec.message());
        return SendResult("write_failed", {});
    }

    {
        std::ofstream file(pluginsFile, std::ios::binary | std::ios::trunc);
        file << text;
        if (!file)
        {
            spdlog::error("Load order: couldn't write plugins.txt");
            std::filesystem::copy_file(backup, pluginsFile, std::filesystem::copy_options::overwrite_existing, ec);
            return SendResult("write_failed", {});
        }
    }

    spdlog::info("Load order: plugins.txt now matches the host's ({} plugins, {} switched off, backup {}), restarting", hostOrder.size(), others.size(),
                 backup.filename().string());
    // straight back to the game, which then joins the host
    Updater::Restart(true);
}
} // namespace

void Queue(std::string aHostOrder) noexcept
{
    std::scoped_lock lock(s_lock);
    s_queued = std::move(aHostOrder);
}

void RunQueued() noexcept
{
    std::optional<std::string> hostOrder;
    {
        std::scoped_lock lock(s_lock);
        hostOrder.swap(s_queued);
    }
    if (hostOrder)
        Match(*hostOrder);
}
} // namespace LoadOrderMatch

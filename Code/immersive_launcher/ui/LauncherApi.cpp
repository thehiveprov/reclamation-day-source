#include "TargetConfig.h"

#if defined(TARGET_FT)

#include "ui/LauncherApi.h"
#include "ui/LauncherUi.h"

#include "oobe/PathSelection.h"
#include "utils/ComUtils.h"
#include "utils/Registry.h"

#include <BuildInfo.h>
#include <TiltedCore/Filesystem.hpp>
#include <spdlog/spdlog.h>

#include <client/Games/Fallout4/Interface/Updater.h>
#include <client/Games/Fallout4/MiniJson.h>
#include <client/Games/Fallout4/Requirements.h>

#include <include/cef_app.h>

#include <ShlObj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace launcher::ui
{
namespace
{
namespace fs = std::filesystem;

constexpr wchar_t kSettingsKey[] = LR"(Software\TiltedPhoques\TiltedEvolution\)" SHORT_NAME;
// The one mod Game Files becomes in a mod manager
constexpr wchar_t kModName[] = L"Reclamation Day - Game Files";
constexpr const char* kStyles[]{"pipboy", "brotherhood", "bulletin", "minutemen"};

HWND s_window = nullptr;

// Shared with the threads copying Game Files and fetching the notes
std::mutex s_lock;
struct Busy
{
    std::string message;
    size_t done = 0;
    size_t total = 0;
};
std::optional<Busy> s_busy;
std::string s_error; // the last copy's failure, for the page

enum class NotesState
{
    kFetching,
    kDone,
    kFailed
};
NotesState s_notesState = NotesState::kFetching;
std::string s_notes; // the releases, as GitHub lists them

// UI thread only
std::vector<oobe::GameInstall> s_installs;

// How long Play waits for the update check: a slow network mustn't keep anyone from the game, and the main menu checks too
constexpr auto kUpdateCheckPatience = std::chrono::seconds(10);
bool s_updateChecked = false; // this window asked for the check
std::chrono::steady_clock::time_point s_updateCheckStart;

// A place Game Files can go
struct Staging
{
    std::string kind; // vortex, mo2, folder (another mod manager's, or any), data (straight into Data), manual (nowhere)
    fs::path path;
    std::string name;
    std::string detail;
    bool detected = false;
};
std::vector<Staging> s_stagings;

std::string Narrow(const std::wstring& acText)
{
    if (acText.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& acText)
{
    if (acText.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), out.data(), size);
    return out;
}

std::string Quote(const std::string& acText)
{
    return MiniJson::Quote(acText);
}

std::string Quote(const fs::path& acPath)
{
    return MiniJson::Quote(Narrow(acPath.wstring()));
}

std::string Answer(bool aOk, const std::string& acError = {})
{
    return aOk ? R"({"ok":true})" : R"({"ok":false,"error":)" + Quote(acError) + "}";
}

std::wstring ReadSetting(const wchar_t* acpName)
{
    return Registry::ReadString<wchar_t>(HKEY_CURRENT_USER, kSettingsKey, acpName);
}

bool WriteSetting(const wchar_t* acpName, const std::wstring& acValue)
{
    return Registry::WriteString(HKEY_CURRENT_USER, kSettingsKey, acpName, acValue);
}

fs::path KnownFolder(REFKNOWNFOLDERID aId)
{
    PWSTR pPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(aId, 0, nullptr, &pPath)))
        return {};
    fs::path path(pPath);
    CoTaskMemFree(pPath);
    return path;
}

bool IsDirectory(const fs::path& acPath)
{
    std::error_code ec;
    return !acPath.empty() && fs::is_directory(acPath, ec);
}

bool SamePath(const fs::path& acA, const fs::path& acB)
{
    return _wcsicmp(acA.lexically_normal().c_str(), acB.lexically_normal().c_str()) == 0;
}

// --- The game install ---

std::optional<oobe::GameInstall> GetChosenGame()
{
    const fs::path exe = ReadSetting(L"TitleExe");
    std::error_code ec;
    if (exe.empty() || !fs::is_regular_file(exe, ec))
        return std::nullopt;
    for (const auto& install : s_installs)
    {
        if (SamePath(install.exe, exe))
            return install;
    }
    auto install = oobe::DescribeInstall(exe.lexically_normal(), L"Picked by hand");
    s_installs.push_back(install);
    return install;
}

fs::path GetDataFolder()
{
    const auto game = GetChosenGame();
    return game ? game->exe.parent_path() / L"Data" : fs::path{};
}

// --- Where Game Files' mods go ---

// Vortex's staging folder for Fallout 4: its deployment manifest in Data names it (vortex.deployment.json), and Vortex
// marks each staging folder with __vortex_staging_folder
std::optional<fs::path> FindVortexStaging(const fs::path& acData)
{
    std::ifstream manifest(acData / L"vortex.deployment.json", std::ios::binary);
    if (manifest)
    {
        std::stringstream text;
        text << manifest.rdbuf();
        const fs::path staging = Widen(MiniJson::GetString(text.str(), "stagingPath"));
        std::error_code ec;
        if (!staging.empty() && fs::exists(staging / L"__vortex_staging_folder", ec))
            return staging;
    }

    const auto fallback = KnownFolder(FOLDERID_RoamingAppData) / L"Vortex" / L"fallout4" / L"mods";
    std::error_code ec;
    if (fs::exists(fallback / L"__vortex_staging_folder", ec))
        return fallback;
    return std::nullopt;
}

std::wstring ReadIni(const fs::path& acIni, const wchar_t* acpSection, const wchar_t* acpKey)
{
    wchar_t value[2048]{};
    GetPrivateProfileStringW(acpSection, acpKey, L"", value, static_cast<DWORD>(std::size(value)), acIni.c_str());
    std::wstring text = value;
    // Qt writes some paths as @ByteArray(...)
    if (text.starts_with(L"@ByteArray(") && text.ends_with(L")"))
        text = text.substr(11, text.size() - 12);
    return text;
}

// A Mod Organizer 2 instance (its ModOrganizer.ini) of Fallout 4: its mods folder
void AddMo2Instance(const fs::path& acIni, const std::wstring& acName)
{
    std::error_code ec;
    if (!fs::is_regular_file(acIni, ec) || _wcsicmp(ReadIni(acIni, L"General", L"gameName").c_str(), L"Fallout 4") != 0)
        return;

    fs::path base = ReadIni(acIni, L"Settings", L"base_directory");
    if (base.empty())
        base = acIni.parent_path();
    std::wstring mods = ReadIni(acIni, L"Settings", L"mod_directory");
    if (mods.empty())
        mods = L"%BASE_DIR%/mods";
    if (const auto at = mods.find(L"%BASE_DIR%"); at != std::wstring::npos)
        mods.replace(at, 10, base.wstring());
    const fs::path folder = fs::path(mods).lexically_normal();
    if (!IsDirectory(folder) || std::any_of(s_stagings.begin(), s_stagings.end(), [&folder](const Staging& acOther) { return SamePath(acOther.path, folder); }))
        return;

    s_stagings.push_back({"mo2", folder, "Mod Organizer 2 \xC2\xB7 " + Narrow(acName),
                          "The instance's mods folder. Enable the mod in MO2's left pane and start Reclamation Day from MO2.", true});
}

void FindMo2Instances()
{
    // instances: %LOCALAPPDATA%\ModOrganizer\<name>\ModOrganizer.ini
    const auto root = KnownFolder(FOLDERID_LocalAppData) / L"ModOrganizer";
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_directory(ec))
            AddMo2Instance(it->path() / L"ModOrganizer.ini", it->path().filename().wstring());
    }

    // portable installs, as the nxm link handler lists them ("1\executable=C:\\MO2\\ModOrganizer.exe")
    const auto handler = root / L"nxmhandler.ini";
    const int count = GetPrivateProfileIntW(L"handlers", L"size", 0, handler.c_str());
    for (int i = 1; i <= count && i < 64; ++i)
    {
        std::wstring exe = ReadIni(handler, L"handlers", (std::to_wstring(i) + L"\\executable").c_str());
        for (size_t at = 0; (at = exe.find(L"\\\\", at)) != std::wstring::npos; ++at)
            exe.erase(at, 1);
        if (!exe.empty())
            AddMo2Instance(fs::path(exe).parent_path() / L"ModOrganizer.ini", fs::path(exe).parent_path().filename().wstring() + L" (portable)");
    }
}

// What a folder picked by hand is: Vortex's staging folder, an MO2 mods folder, or just a folder
Staging DescribeFolder(const fs::path& acFolder)
{
    std::error_code ec;
    if (fs::exists(acFolder / L"__vortex_staging_folder", ec))
        return {"vortex", acFolder, "Vortex", "Its Fallout 4 staging folder. Vortex lists the mod; enable it and deploy.", false};
    if (fs::is_regular_file(acFolder.parent_path() / L"ModOrganizer.ini", ec))
        return {"mo2", acFolder, "Mod Organizer 2", "Its mods folder. Enable the mod in MO2's left pane and start Reclamation Day from MO2.", false};
    return {"folder", acFolder, "Your folder", "Kept there as one mod folder, for a mod manager of your own.", false};
}

void FindStagings()
{
    s_stagings.clear();
    const auto data = GetDataFolder();
    if (!data.empty())
    {
        if (const auto vortex = FindVortexStaging(data))
            s_stagings.push_back({"vortex", *vortex, "Vortex", "Its Fallout 4 staging folder. Vortex lists the mod; enable it and deploy.", true});
    }
    FindMo2Instances();

    // the folder chosen before, if it's none of those
    const std::string kind = Narrow(ReadSetting(L"StagingKind"));
    const fs::path path = ReadSetting(L"StagingPath");
    if ((kind == "vortex" || kind == "mo2" || kind == "folder") && IsDirectory(path) &&
        std::none_of(s_stagings.begin(), s_stagings.end(), [&path](const Staging& acOther) { return SamePath(acOther.path, path); }))
        s_stagings.push_back(DescribeFolder(path));

    s_stagings.push_back({"data", data, "No mod manager", "The launcher puts them straight into the game's Data folder.", false});
    s_stagings.push_back({"manual", {}, "I install them myself", "Nothing is copied: install Game Files with your own tools, as the README says.", false});
}

std::optional<Staging> GetChosenStaging()
{
    const std::string kind = Narrow(ReadSetting(L"StagingKind"));
    if (kind.empty())
        return std::nullopt;
    const fs::path path = ReadSetting(L"StagingPath");
    for (const auto& staging : s_stagings)
    {
        // Data and nowhere have no path of their own to match
        if (staging.kind == kind && (kind == "data" || kind == "manual" || SamePath(staging.path, path)))
            return staging;
    }
    // gone (the folder was removed): chosen again
    return std::nullopt;
}

// Where the files go: the mod's folder in the staging folder, or Data
fs::path GetTarget(const Staging& acStaging)
{
    return acStaging.kind == "data" ? acStaging.path : acStaging.path / kModName;
}

fs::path GetGameFiles()
{
    return TiltedPhoques::GetPath().parent_path() / L"Game Files";
}

struct StagedFile
{
    fs::path relative;
    fs::path source;
};

// Game Files, but for what every start keeps in Data itself and the notes at its top (CREDITS.txt)
std::vector<StagedFile> ListStagedFiles()
{
    std::vector<StagedFile> files;
    const auto gameFiles = GetGameFiles();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(gameFiles, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const auto relative = it->path().lexically_relative(gameFiles);
        if (!relative.has_parent_path() && _wcsicmp(relative.extension().c_str(), L".txt") == 0)
            continue;
        // an update's files on their way in or out
        if (_wcsicmp(relative.extension().c_str(), L".rdnew") == 0 || _wcsicmp(relative.extension().c_str(), L".rdold") == 0)
            continue;
        if (Updater::IsKeptInData(relative))
            continue;
        files.push_back({relative, it->path()});
    }
    return files;
}

// Game Files' staged files are all in Data (a mod manager deployed them, or they were copied there). Listed once: Game
// Files doesn't change while the window is up.
bool IsDeployed(const fs::path& acData)
{
    static const std::vector<StagedFile> s_files = ListStagedFiles();
    std::error_code ec;
    return !acData.empty() && !s_files.empty() &&
           std::all_of(s_files.begin(), s_files.end(), [&acData, &ec](const StagedFile& acFile) { return fs::exists(acData / acFile.relative, ec); });
}

std::string StagingNote(const Staging& acStaging)
{
    const std::string mod = "\xE2\x80\x9C" + Narrow(kModName) + "\xE2\x80\x9D";
    const bool deployed = IsDeployed(GetDataFolder());
    if (acStaging.kind == "vortex")
        return deployed ? "In Vortex as " + mod + ", deployed." : "In Vortex as " + mod + ": enable it there and deploy.";
    if (acStaging.kind == "mo2")
        return "In Mod Organizer 2 as " + mod + ": enable it in the left pane and start Reclamation Day from MO2.";
    if (acStaging.kind == "folder")
        return "Kept as " + mod + " in your folder" + (deployed ? ", and in Data." : ": deploy it with your mod manager.");
    if (acStaging.kind == "data")
        return "Kept in the game's Data folder at every start.";
    return deployed ? "Nothing is copied; Game Files is in Data." : "Nothing is copied: install Game Files yourself, as the README says.";
}

// Copies what's missing or changed into the chosen place, in the background with progress for the page; the mod's
// folder in a staging folder loses files Game Files no longer has. Then the game starts, when asked.
void StartCopy(const Staging& acStaging, bool aThenPlay)
{
    {
        std::scoped_lock _(s_lock);
        if (s_busy)
            return;
        s_busy = Busy{"Checking Game Files"};
        s_error.clear();
    }

    std::thread(
        [staging = acStaging, aThenPlay]
        {
            const auto target = GetTarget(staging);
            const bool ownFolder = staging.kind != "data";
            const std::string where = staging.kind == "data" ? "the game's Data folder" : staging.kind == "vortex" ? "Vortex" : staging.kind == "mo2" ? "Mod Organizer 2" : "your folder";

            std::vector<StagedFile> work;
            const auto files = ListStagedFiles();
            std::error_code ec;
            for (const auto& file : files)
            {
                const auto destination = target / file.relative;
                if (fs::exists(destination, ec) && fs::file_size(destination, ec) == fs::file_size(file.source, ec) &&
                    fs::last_write_time(destination, ec) == fs::last_write_time(file.source, ec))
                    continue;
                work.push_back(file);
            }

            {
                std::scoped_lock _(s_lock);
                s_busy = Busy{"Putting Game Files in " + where, 0, work.size()};
            }

            std::string error;
            for (const auto& file : work)
            {
                const auto destination = target / file.relative;
                fs::create_directories(destination.parent_path(), ec);
                SetFileAttributesW(destination.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!CopyFileW(file.source.c_str(), destination.c_str(), FALSE))
                {
                    error = "Couldn't copy " + Narrow(file.relative.wstring()) + " to " + Narrow(target.wstring()) + " (Windows error " + std::to_string(GetLastError()) + ").";
                    break;
                }
                std::scoped_lock _(s_lock);
                ++s_busy->done;
            }

            // our own mod folder holds only what Game Files has now
            if (error.empty() && ownFolder)
            {
                std::vector<fs::path> stale;
                for (fs::recursive_directory_iterator it(target, ec), end; !ec && it != end; it.increment(ec))
                {
                    if (!it->is_regular_file(ec))
                        continue;
                    const auto relative = it->path().lexically_relative(target);
                    if (_wcsicmp(relative.c_str(), L"meta.ini") == 0)
                        continue;
                    if (std::none_of(files.begin(), files.end(), [&relative](const StagedFile& acFile) { return _wcsicmp(acFile.relative.c_str(), relative.c_str()) == 0; }))
                        stale.push_back(it->path());
                }
                for (const auto& path : stale)
                    fs::remove(path, ec);

                // Mod Organizer 2 shows a mod's version and notes from it
                if (staging.kind == "mo2")
                {
                    std::ofstream meta(target / L"meta.ini", std::ios::binary | std::ios::trunc);
                    meta << "[General]\r\ngameName=Fallout4\r\nmodid=0\r\nversion=" << BUILD_COMMIT << "\r\nnotes=Kept up to date by Reclamation Day's launcher\r\n";
                }
            }

            spdlog::info("Launcher: Game Files to {}: {} of {} files copied{}", Narrow(target.wstring()), error.empty() ? work.size() : 0, files.size(), error.empty() ? "" : ", " + error);

            std::scoped_lock _(s_lock);
            s_busy.reset();
            s_error = error;
            if (error.empty() && aThenPlay)
                PostMessageW(s_window, kCloseMessage, kExitPlay, 0);
        })
        .detach();
}

// --- The patch notes ---

std::optional<std::string> HttpsGet(const std::wstring& acHost, const std::wstring& acPath, const std::wstring& acHeaders)
{
    std::optional<std::string> result;
    HINTERNET session = WinHttpOpen(L"ReclamationDay-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return result;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connection = WinHttpConnect(session, acHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", acPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && WinHttpSendRequest(request, acHeaders.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        std::string body;
        for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available;)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                break;
            body.append(chunk.data(), read);
        }
        if (status == 200)
            result = std::move(body);
        else
            spdlog::warn("Launcher: the releases answered {}", status);
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

// The releases' notes, newest first, from the releases repo the updater uses ("Reclamation Day\updater.cfg")
void FetchNotes()
{
    std::string repo, token;
    std::ifstream config(TiltedPhoques::GetPath() / L"updater.cfg");
    for (std::string line; std::getline(config, line);)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.starts_with("repo="))
            repo = line.substr(5);
        else if (line.starts_with("token="))
            token = line.substr(6);
    }

    std::optional<std::string> body;
    if (!repo.empty() && !token.empty())
        body = HttpsGet(L"api.github.com", L"/repos/" + Widen(repo) + L"/releases?per_page=60",
                        L"Accept: application/vnd.github+json\r\nAuthorization: Bearer " + Widen(token) + L"\r\n");

    std::scoped_lock _(s_lock);
    if (body && !body->empty() && body->front() == '[')
    {
        s_notes = std::move(*body);
        s_notesState = NotesState::kDone;
    }
    else
    {
        s_notesState = NotesState::kFailed;
        spdlog::info("Launcher: the patch notes are the ones this build came with");
    }
}

// --- The mods by other authors Reclamation Day needs (Requirements.h) ---

// A requirement's file as a mod has it in a staging folder: where it goes under Data (F4SE itself at the mod's top, as
// mod managers stage it for the game's own folder)
fs::path StagedPath(const Requirements::Requirement& acRequirement, const char* apFile)
{
    const fs::path installed = Requirements::InstalledPath(acRequirement, apFile);
    const auto first = installed.begin();
    return first != installed.end() && _wcsicmp(first->c_str(), L"Data") == 0 ? installed.lexically_relative(L"Data") : installed;
}

// Each requirement: "ok" (all its files in the game's folder), "staged" (in a mod of the chosen staging folder, not in
// Data: Vortex hasn't deployed it, or Mod Organizer 2 shows it to the game itself), or "missing". Looking through a
// staging folder's mods is slow on a big one, so the answer is kept for 10 seconds.
std::string BuildRequirements()
{
    static std::string s_json;
    static std::chrono::steady_clock::time_point s_at;
    const auto now = std::chrono::steady_clock::now();
    if (!s_json.empty() && now - s_at < std::chrono::seconds(10))
        return s_json;

    const auto game = GetChosenGame();
    const fs::path folder = game ? game->exe.parent_path() : fs::path{};
    std::vector<fs::path> mods;
    if (const auto staging = GetChosenStaging(); staging && (staging->kind == "vortex" || staging->kind == "mo2" || staging->kind == "folder"))
    {
        std::error_code ec;
        for (fs::directory_iterator it(staging->path, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->is_directory(ec))
                mods.push_back(it->path());
        }
    }

    std::string json;
    for (const auto& requirement : Requirements::kAll)
    {
        std::error_code ec;
        const auto has = [&requirement, &ec](const fs::path& acRoot, bool aStaged)
        {
            return std::all_of(requirement.files.begin(), requirement.files.end(), [&](const char* apFile)
                                { return fs::exists(acRoot / (aStaged ? StagedPath(requirement, apFile) : fs::path(Requirements::InstalledPath(requirement, apFile))), ec); });
        };
        const char* pStatus = "missing";
        if (!folder.empty() && has(folder, false))
            pStatus = "ok";
        else if (std::any_of(mods.begin(), mods.end(), [&has](const fs::path& acMod) { return has(acMod, true); }))
            pStatus = "staged";
        json += (json.empty() ? "" : ",") + fmt::format(R"({{"name":{},"which":{},"exact":{},"status":"{}"}})", Quote(std::string(requirement.pName)),
                                                        Quote(std::string(requirement.pWhich)), requirement.nexusFile != 0, pStatus);
    }
    s_json = json;
    s_at = now;
    return json;
}

// --- The update ---

// A game already running here (HivemindApp's DetectSecondInstance holds this from its start): this start is a second
// instance, which has to stay on the first one's version to play with it, so the update waits for the main menu
bool IsGameRunning()
{
    const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ReclamationDayFirstInstance");
    if (!mutex)
        return false;
    CloseHandle(mutex);
    return true;
}

bool IsUpdateCheckRunning(Updater::State aState)
{
    return s_updateChecked && (aState == Updater::State::kIdle || aState == Updater::State::kChecking) &&
           std::chrono::steady_clock::now() - s_updateCheckStart < kUpdateCheckPatience;
}

// The update for the page; null when there's nothing to wait for or install
std::string BuildUpdate()
{
    if (!s_updateChecked)
        return "null";

    const auto status = Updater::GetStatus();
    const char* pState = nullptr;
    switch (status.state)
    {
    case Updater::State::kIdle:
    case Updater::State::kChecking:
        if (!IsUpdateCheckRunning(status.state))
            return "null";
        pState = "checking";
        break;
    case Updater::State::kUpToDate: return "null";
    case Updater::State::kAvailable: pState = "available"; break;
    case Updater::State::kUpdating: pState = "updating"; break;
    case Updater::State::kFailed: pState = "failed"; break;
    case Updater::State::kReady: pState = "ready"; break;
    }
    return fmt::format(R"({{"state":"{}","version":{},"message":{},"percent":{},"error":{}}})", pState, Quote(status.latestVersion), Quote(status.message), status.percent,
                       Quote(status.error));
}

// Play's answer while the update comes first; empty when Play can go on. An update that failed doesn't hold anyone back:
// the page offers to try again or to play without it.
std::string HoldPlayForUpdate()
{
    if (!s_updateChecked)
        return {};
    const auto state = Updater::GetStatus().state;
    if (IsUpdateCheckRunning(state))
        return R"({"ok":false,"checking":true})";
    if (state == Updater::State::kAvailable)
        return Answer(false, "Install the update first.");
    if (state == Updater::State::kUpdating || state == Updater::State::kReady)
        return Answer(false, "The update is installing.");
    return {};
}

// Installs the update the page was told about; the window closes once it's in, and the launcher starts the new version
std::string StartUpdate()
{
    {
        std::scoped_lock _(s_lock);
        if (s_busy)
            return Answer(false, "Wait for the copy in progress to finish.");
    }
    const auto state = Updater::GetStatus().state;
    if (state != Updater::State::kAvailable && state != Updater::State::kFailed)
        return Answer(false, "There's no update to install.");

    spdlog::info("Launcher: installing the update");
    Updater::StartUpdate();
    std::thread(
        []
        {
            auto current = Updater::GetStatus().state;
            for (; current == Updater::State::kUpdating; current = Updater::GetStatus().state)
                Sleep(200);
            if (current == Updater::State::kReady)
                PostMessageW(s_window, kCloseMessage, kExitUpdated, 0);
        })
        .detach();
    return Answer(true);
}

// --- The page's questions ---

std::string BuildState()
{
    const auto game = GetChosenGame();
    const auto staging = GetChosenStaging();

    std::string installs;
    for (const auto& install : s_installs)
    {
        installs += (installs.empty() ? "" : ",") + fmt::format(R"({{"exe":{},"folder":{},"source":{},"version":{},"supported":{}}})", Quote(install.exe), Quote(install.exe.parent_path()),
                                                                 Quote(Narrow(install.source)), Quote(Narrow(install.version)), install.supported);
    }
    std::string gameJson = "null";
    if (game)
        gameJson = fmt::format(R"({{"exe":{},"folder":{},"source":{},"version":{},"supported":{}}})", Quote(game->exe), Quote(game->exe.parent_path()), Quote(Narrow(game->source)),
                               Quote(Narrow(game->version)), game->supported);

    std::string stagings;
    for (const auto& option : s_stagings)
    {
        stagings += (stagings.empty() ? "" : ",") + fmt::format(R"({{"kind":{},"name":{},"detail":{},"path":{},"detected":{}}})", Quote(option.kind), Quote(option.name),
                                                                 Quote(option.detail), Quote(option.path), option.detected);
    }
    std::string stagingJson = "null";
    if (staging)
        stagingJson = fmt::format(R"({{"kind":{},"name":{},"path":{},"note":{}}})", Quote(staging->kind), Quote(staging->name), Quote(staging->path), Quote(StagingNote(*staging)));

    std::string busy = "null", error;
    {
        std::scoped_lock _(s_lock);
        if (s_busy)
            busy = fmt::format(R"({{"message":{},"done":{},"total":{}}})", Quote(s_busy->message), s_busy->done, s_busy->total);
        error = s_error;
    }

    return fmt::format(R"({{"version":{},"requiredVersion":{},"style":{},"setupDone":{},"game":{},"installs":[{}],"staging":{},"stagings":[{}],"requirements":[{}],"busy":{},"error":{},"update":{}}})",
                       Quote(std::string(BUILD_COMMIT)), Quote(std::string(SUPPORTED_GAME_VERSION)), Quote(GetStyle()), game && game->supported && staging.has_value(), gameJson,
                       installs, stagingJson, stagings, BuildRequirements(), busy, Quote(error), BuildUpdate());
}

std::optional<fs::path> PickWithDialog(bool aFolder)
{
    ComPtr<IFileOpenDialog> pDialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pDialog))))
        return std::nullopt;

    DWORD options = 0;
    pDialog->GetOptions(&options);
    if (aFolder)
    {
        pDialog->SetTitle(L"Choose the folder for Game Files' mods");
        pDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT);
    }
    else
    {
        pDialog->SetTitle(L"Select the game executable (" TARGET_NAME L".exe)");
        static constinit COMDLG_FILTERSPEC filters[] = {{TARGET_NAME L".exe", TARGET_NAME L".exe"}, {L"All programs", L"*.exe"}};
        pDialog->SetFileTypes(2, filters);
        pDialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);
        if (const auto game = GetChosenGame())
        {
            ComPtr<IShellItem> pFolder;
            if (SUCCEEDED(SHCreateItemFromParsingName(game->exe.parent_path().c_str(), nullptr, IID_PPV_ARGS(&pFolder))))
                pDialog->SetFolder(pFolder.Get());
        }
    }

    if (FAILED(pDialog->Show(s_window)))
        return std::nullopt;
    ComPtr<IShellItem> pItem;
    PWSTR pPath = nullptr;
    if (FAILED(pDialog->GetResult(&pItem)) || FAILED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)))
        return std::nullopt;
    fs::path path(pPath);
    CoTaskMemFree(pPath);
    return path;
}

std::string ChooseGame(const fs::path& acExe)
{
    if (const auto problem = oobe::CheckGameExe(acExe); !problem.empty())
        return Answer(false, Narrow(problem));
    if (!oobe::SaveChosenInstall(acExe.lexically_normal()))
        return Answer(false, "Couldn't save the choice in the registry.");
    GetChosenGame();
    // another game, another Data folder: Vortex's staging folder is found from it
    FindStagings();
    spdlog::info("Launcher: the game is {}", Narrow(acExe.wstring()));
    return Answer(true);
}

// By value: choosing finds the staging folders again, which the choice may have come from
std::string ChooseStaging(const Staging acStaging)
{
    {
        std::scoped_lock _(s_lock);
        if (s_busy)
            return Answer(false, "Wait for the copy in progress to finish.");
    }
    if (acStaging.kind != "manual" && acStaging.kind != "data" && !IsDirectory(acStaging.path))
        return Answer(false, "That folder isn't there any more.");
    if (acStaging.kind == "data" && GetDataFolder().empty())
        return Answer(false, "Pick the game first: its Data folder is where they'd go.");

    if (!WriteSetting(L"StagingKind", Widen(acStaging.kind)) || !WriteSetting(L"StagingPath", acStaging.kind == "data" || acStaging.kind == "manual" ? L"" : acStaging.path.wstring()))
        return Answer(false, "Couldn't save the choice in the registry.");
    FindStagings();
    spdlog::info("Launcher: Game Files go to {} {}", acStaging.kind, Narrow(acStaging.path.wstring()));

    if (const auto chosen = GetChosenStaging(); chosen && chosen->kind != "manual")
        StartCopy(*chosen, false);
    return Answer(true);
}
} // namespace

void StartApi(HWND aWindow)
{
    s_window = aWindow;
    s_installs = oobe::FindGameInstalls();
    GetChosenGame();
    FindStagings();
    std::thread(FetchNotes).detach();

    if (!IsGameRunning())
    {
        s_updateChecked = true;
        s_updateCheckStart = std::chrono::steady_clock::now();
        Updater::StartWindowCheck();
    }
    else
        spdlog::info("Launcher: a game is already running, the update check is left to its main menu");
}

std::string GetStyle()
{
    const std::string saved = Narrow(ReadSetting(L"LauncherStyle"));
    // The looks these two replaced, for a player who had picked one of them
    const std::string style = saved == "dusk" ? "brotherhood" : saved == "terminal" ? "minutemen" : saved;
    return std::any_of(std::begin(kStyles), std::end(kStyles), [&style](const char* acpStyle) { return style == acpStyle; }) ? style : "pipboy";
}

std::string HandleApi(const std::string& acName, const std::string& acBody)
{
    if (acName == "state")
        return BuildState();

    if (acName == "notes")
    {
        std::scoped_lock _(s_lock);
        if (s_notesState == NotesState::kFetching)
            return R"({"pending":true})";
        if (s_notesState == NotesState::kFailed)
            return R"({"source":"built-in"})";
        return R"({"source":"live","releases":)" + s_notes + "}";
    }

    if (acName == "style")
    {
        const std::string style = MiniJson::GetString(acBody, "style");
        if (std::none_of(std::begin(kStyles), std::end(kStyles), [&style](const char* acpStyle) { return style == acpStyle; }))
            return Answer(false, "No such style.");
        return Answer(WriteSetting(L"LauncherStyle", Widen(style)), "Couldn't save the style.");
    }

    if (acName == "game")
    {
        const fs::path exe = Widen(MiniJson::GetString(acBody, "exe"));
        if (std::none_of(s_installs.begin(), s_installs.end(), [&exe](const oobe::GameInstall& acInstall) { return SamePath(acInstall.exe, exe); }))
            return Answer(false, "That install isn't in the list any more.");
        return ChooseGame(exe);
    }

    if (acName == "browseGame")
    {
        CefSetOSModalLoop(true);
        const auto exe = PickWithDialog(false);
        CefSetOSModalLoop(false);
        if (!exe)
            return R"({"ok":false,"cancelled":true})";
        return ChooseGame(*exe);
    }

    if (acName == "staging")
    {
        const std::string kind = MiniJson::GetString(acBody, "kind");
        const fs::path path = Widen(MiniJson::GetString(acBody, "path"));
        for (const auto& option : s_stagings)
        {
            if (option.kind == kind && (kind == "data" || kind == "manual" || SamePath(option.path, path)))
                return ChooseStaging(option);
        }
        return Answer(false, "That choice isn't in the list any more.");
    }

    if (acName == "browseStaging")
    {
        CefSetOSModalLoop(true);
        const auto folder = PickWithDialog(true);
        CefSetOSModalLoop(false);
        if (!folder)
            return R"({"ok":false,"cancelled":true})";
        const auto gameFiles = GetGameFiles();
        if (SamePath(*folder, gameFiles) || SamePath(*folder, GetDataFolder()) || SamePath(*folder, TiltedPhoques::GetPath()))
            return Answer(false, "Pick a mod manager's staging folder, not that one.");
        return ChooseStaging(DescribeFolder(folder->lexically_normal()));
    }

    if (acName == "update")
        return StartUpdate();

    if (acName == "play")
    {
        if (const auto held = HoldPlayForUpdate(); !held.empty())
            return held;
        const auto game = GetChosenGame();
        if (!game || !game->supported)
            return Answer(false, "Pick a Fallout 4 " SUPPORTED_GAME_VERSION " install first.");
        const auto staging = GetChosenStaging();
        if (!staging)
            return Answer(false, "Pick where Game Files' mods go first.");
        {
            std::scoped_lock _(s_lock);
            if (s_busy)
                return Answer(false, "Wait for the copy in progress to finish.");
        }
        spdlog::info("Launcher: play");
        if (staging->kind == "manual")
            PostMessageW(s_window, kCloseMessage, kExitPlay, 0);
        else
            StartCopy(*staging, true);
        return Answer(true);
    }

    // A requirement's Download: its file on Nexus Mods as a mod manager download, in the player's browser
    if (acName == "getMod")
    {
        const std::string index = MiniJson::GetString(acBody, "index");
        const size_t at = index.empty() ? SIZE_MAX : std::strtoul(index.c_str(), nullptr, 10);
        if (at >= std::size(Requirements::kAll))
            return Answer(false, "No such mod.");
        const auto url = Widen(Requirements::DownloadUrl(Requirements::kAll[at]));
        spdlog::info("Launcher: download {}", Requirements::kAll[at].pName);
        const bool opened = reinterpret_cast<INT_PTR>(ShellExecuteW(s_window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        return Answer(opened, "Couldn't open the browser.");
    }

    if (acName == "window")
    {
        const std::string op = MiniJson::GetString(acBody, "op");
        if (op == "drag")
            PostMessageW(s_window, kDragMessage, 0, 0);
        else if (op == "minimize")
            PostMessageW(s_window, kMinimizeMessage, 0, 0);
        else if (op == "close")
            PostMessageW(s_window, kCloseMessage, kExitQuit, 0);
        return Answer(true);
    }

    return Answer(false, "Unknown request " + acName);
}
} // namespace launcher::ui

#endif

#include <HivemindPCH.h>

#include <Interface/Updater.h>

#include <BuildInfo.h>
#include <CrashHandler.h>
#include <Interface/PluginsTxt.h>
#include <SaveFolder.h>

#include <windows.h>
#include <bcrypt.h>
#include <ShlObj.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace Updater
{
namespace
{
constexpr wchar_t kUserAgent[] = L"ReclamationDayLauncher";
constexpr wchar_t kNewSuffix[] = L".rdnew";
constexpr wchar_t kOldSuffix[] = L".rdold";

std::mutex s_lock;
Status s_status;
std::atomic<bool> s_checkStarted{false};

void SetStatus(State aState, std::string aMessage = {}, int aPercent = -1)
{
    std::scoped_lock lock(s_lock);
    s_status.state = aState;
    s_status.message = std::move(aMessage);
    s_status.percent = aPercent;
}

// --- The install ---

// The launcher's folder ("Reclamation Day") and the install root above it (where "Launch Reclamation Day.exe" is)
std::filesystem::path GetAppFolder()
{
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path GetRoot()
{
    return GetAppFolder().parent_path();
}

std::string Trim(const std::string& acText)
{
    const auto first = acText.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = acText.find_last_not_of(" \t\r\n");
    return acText.substr(first, last - first + 1);
}

std::string ToUpper(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return aText;
}

std::string ToLower(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return aText;
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

struct Config
{
    std::string Repo;
    std::string Token;
};

// "Reclamation Day\updater.cfg" (repo=, token=): which repo has the releases, and a read-only key to it
bool LoadConfig(Config& aConfig)
{
    std::ifstream file(GetAppFolder() / L"updater.cfg");
    for (std::string line; std::getline(file, line);)
    {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, equals));
        if (key == "repo")
            aConfig.Repo = Trim(line.substr(equals + 1));
        else if (key == "token")
            aConfig.Token = Trim(line.substr(equals + 1));
    }
    return !aConfig.Repo.empty() && !aConfig.Token.empty();
}

// --- SHA-256 ---

class Sha256
{
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            BCryptCreateHash(m_algorithm, &m_hash, nullptr, 0, nullptr, 0, 0);
    }

    ~Sha256()
    {
        if (m_hash)
            BCryptDestroyHash(m_hash);
        if (m_algorithm)
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }

    void Update(const void* apData, size_t aSize) { BCryptHashData(m_hash, static_cast<PUCHAR>(const_cast<void*>(apData)), static_cast<ULONG>(aSize), 0); }

    std::string Finish()
    {
        UCHAR digest[32]{};
        BCryptFinishHash(m_hash, digest, sizeof(digest), 0);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string text;
        for (const UCHAR byte : digest)
        {
            text += kHex[byte >> 4];
            text += kHex[byte & 0xF];
        }
        return text;
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm{};
    BCRYPT_HASH_HANDLE m_hash{};
};

// Lowercase hex SHA-256 of a file, empty if it can't be read
std::string HashFile(const std::filesystem::path& acPath)
{
    std::ifstream file(acPath, std::ios::binary);
    if (!file)
        return {};

    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (file.gcount() > 0)
            hash.Update(buffer.data(), static_cast<size_t>(file.gcount()));
    }
    return hash.Finish();
}

// --- HTTPS (WinHTTP: Windows' own certificates and proxy settings) ---

struct InternetHandle
{
    HINTERNET handle{};
    ~InternetHandle()
    {
        if (handle)
            WinHttpCloseHandle(handle);
    }
};

// GETs a URL without following redirects (aLocation gets one). The body goes to aSink, which returns false to stop.
bool HttpGet(const std::wstring& acUrl, const std::vector<std::wstring>& acHeaders, DWORD& aStatus, std::wstring& aLocation, uint64_t& aLength,
             const std::function<bool(const char*, size_t)>& aSink, std::string& aError)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    std::vector<wchar_t> host(256), path(acUrl.size() + 1), extra(acUrl.size() + 1);
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    parts.lpszExtraInfo = extra.data();
    parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (!WinHttpCrackUrl(acUrl.c_str(), 0, 0, &parts))
    {
        aError = "bad address";
        return false;
    }

    InternetHandle session{WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle)
        session.handle = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    InternetHandle connection{session.handle ? WinHttpConnect(session.handle, host.data(), parts.nPort, 0) : nullptr};
    const std::wstring object = std::wstring(path.data()) + extra.data();
    InternetHandle request{connection.handle ? WinHttpOpenRequest(connection.handle, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                             : nullptr};
    if (!request.handle)
    {
        aError = fmt::format("couldn't open a connection (error {})", GetLastError());
        return false;
    }

    DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disableRedirects, sizeof(disableRedirects));

    std::wstring headers;
    for (const auto& header : acHeaders)
        headers += header + L"\r\n";

    if (!WinHttpSendRequest(request.handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr))
    {
        aError = fmt::format("no answer (error {})", GetLastError());
        return false;
    }

    DWORD size = sizeof(aStatus);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &aStatus, &size, WINHTTP_NO_HEADER_INDEX);

    aLocation.clear();
    if (aStatus >= 300 && aStatus < 400)
    {
        DWORD locationSize = 0;
        WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &locationSize, WINHTTP_NO_HEADER_INDEX);
        if (locationSize)
        {
            std::wstring location(locationSize / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &locationSize, WINHTTP_NO_HEADER_INDEX))
            {
                location.resize(locationSize / sizeof(wchar_t));
                aLocation = location;
            }
        }
        return true;
    }

    wchar_t lengthText[32]{};
    DWORD lengthSize = sizeof(lengthText);
    aLength = WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX)
                  ? _wcstoui64(lengthText, nullptr, 10)
                  : 0;

    std::vector<char> buffer(1 << 16);
    for (;;)
    {
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
        {
            aError = fmt::format("the download broke off (error {})", GetLastError());
            return false;
        }
        if (read == 0)
            return true;
        if (!aSink(buffer.data(), static_cast<size_t>(read)))
        {
            aError = "the download couldn't be saved";
            return false;
        }
    }
}

std::vector<std::wstring> ApiHeaders(const Config& acConfig, const wchar_t* apAccept)
{
    return {std::wstring(L"Accept: ") + apAccept, L"Authorization: Bearer " + Widen(acConfig.Token), L"X-GitHub-Api-Version: 2022-11-28"};
}

// A release asset: the API redirects to the file's storage, which must be fetched without our key
bool DownloadAsset(const Config& acConfig, const std::string& acApiUrl, const std::function<bool(const char*, size_t, uint64_t)>& aSink, std::string& aError)
{
    DWORD status = 0;
    std::wstring location;
    uint64_t length = 0;
    const auto sink = [&](const char* apData, size_t aSize) { return aSink(apData, aSize, length); };

    if (!HttpGet(Widen(acApiUrl), ApiHeaders(acConfig, L"application/octet-stream"), status, location, length, sink, aError))
        return false;
    if (status == 200)
        return true;
    if (status < 300 || status >= 400 || location.empty())
    {
        aError = fmt::format("the release server answered {}", status);
        return false;
    }

    if (!HttpGet(location, {}, status, location, length, sink, aError))
        return false;
    if (status != 200)
    {
        aError = fmt::format("the file server answered {}", status);
        return false;
    }
    return true;
}

// --- Minimal JSON, for GitHub's release answer ---

struct JsonValue
{
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    const JsonValue* Get(const std::string& acKey) const
    {
        for (const auto& [key, value] : members)
        {
            if (key == acKey)
                return &value;
        }
        return nullptr;
    }
};

class JsonParser
{
public:
    explicit JsonParser(const std::string& acText)
        : m_text(acText)
    {
    }

    bool Parse(JsonValue& aValue) { return ParseValue(aValue, 0); }

private:
    void SkipSpace()
    {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
            ++m_pos;
    }

    bool ParseString(std::string& aOut)
    {
        if (m_pos >= m_text.size() || m_text[m_pos] != '"')
            return false;
        ++m_pos;
        while (m_pos < m_text.size())
        {
            const char c = m_text[m_pos++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                aOut += c;
                continue;
            }
            if (m_pos >= m_text.size())
                return false;
            const char escaped = m_text[m_pos++];
            switch (escaped)
            {
            case 'n': aOut += '\n'; break;
            case 't': aOut += '\t'; break;
            case 'r': aOut += '\r'; break;
            case 'b': aOut += '\b'; break;
            case 'f': aOut += '\f'; break;
            case 'u':
                // Only ever in release notes here: not needed, skipped
                m_pos = std::min(m_pos + 4, m_text.size());
                aOut += '?';
                break;
            default: aOut += escaped; break;
            }
        }
        return false;
    }

    bool ParseValue(JsonValue& aValue, int aDepth)
    {
        SkipSpace();
        if (aDepth > 64 || m_pos >= m_text.size())
            return false;

        const char c = m_text[m_pos];
        if (c == '{' || c == '[')
        {
            const bool isObject = c == '{';
            const char close = isObject ? '}' : ']';
            ++m_pos;
            SkipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == close)
            {
                ++m_pos;
                return true;
            }
            for (;;)
            {
                JsonValue value;
                std::string key;
                if (isObject)
                {
                    SkipSpace();
                    if (!ParseString(key))
                        return false;
                    SkipSpace();
                    if (m_pos >= m_text.size() || m_text[m_pos++] != ':')
                        return false;
                }
                if (!ParseValue(value, aDepth + 1))
                    return false;
                if (isObject)
                    aValue.members.emplace_back(std::move(key), std::move(value));
                else
                    aValue.items.push_back(std::move(value));

                SkipSpace();
                if (m_pos >= m_text.size())
                    return false;
                const char next = m_text[m_pos++];
                if (next == close)
                    return true;
                if (next != ',')
                    return false;
            }
        }
        if (c == '"')
            return ParseString(aValue.text);

        // Numbers, true, false, null: kept as text
        const size_t start = m_pos;
        while (m_pos < m_text.size() && !std::strchr(",}] \t\r\n", m_text[m_pos]))
            ++m_pos;
        aValue.text = m_text.substr(start, m_pos - start);
        return m_pos > start;
    }

    const std::string& m_text;
    size_t m_pos{0};
};

// --- The release ---

struct ManifestFile
{
    std::string sha256;
    uint64_t size{};
    std::string pack;
    std::filesystem::path path; // relative to the install root
};

struct Release
{
    std::string version;
    std::vector<ManifestFile> files;
    std::map<std::string, std::string> assets; // name -> API url
};

// manifest.txt: "version <v>", then "file <sha256> <size> <pack> <path>" per file
bool ParseManifest(const std::string& acText, Release& aRelease)
{
    std::istringstream lines(acText);
    for (std::string line; std::getline(lines, line);)
    {
        line = Trim(line);
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "version")
        {
            fields >> aRelease.version;
        }
        else if (kind == "file")
        {
            ManifestFile file;
            std::string path;
            fields >> file.sha256 >> file.size >> file.pack;
            std::getline(fields, path);
            path = Trim(path);

            // Only ever inside the install
            if (file.sha256.size() != 64 || path.empty() || path.find("..") != std::string::npos || path[0] == '/' || path[0] == '\\' ||
                path.find(':') != std::string::npos)
                return false;

            file.sha256 = ToLower(file.sha256);
            file.path = std::filesystem::path(Widen(path)).make_preferred();
            aRelease.files.push_back(std::move(file));
        }
    }
    return !aRelease.version.empty() && !aRelease.files.empty();
}

bool FetchLatestRelease(const Config& acConfig, Release& aRelease, std::string& aError)
{
    DWORD status = 0;
    std::wstring location;
    uint64_t length = 0;
    std::string body;
    const auto toBody = [&](const char* apData, size_t aSize)
    {
        body.append(apData, aSize);
        return body.size() < (8 << 20);
    };
    if (!HttpGet(Widen("https://api.github.com/repos/" + acConfig.Repo + "/releases/latest"), ApiHeaders(acConfig, L"application/vnd.github+json"), status,
                 location, length, toBody, aError))
        return false;

    if (status == 404)
        return true; // Nothing released yet
    if (status != 200)
    {
        aError = fmt::format("the release server answered {}{}", status, status == 401 ? " (the launcher's access key was refused)" : "");
        return false;
    }

    JsonValue release;
    if (!JsonParser(body).Parse(release))
    {
        aError = "couldn't read the release information";
        return false;
    }

    if (const auto* pAssets = release.Get("assets"))
    {
        for (const auto& asset : pAssets->items)
        {
            const auto* pName = asset.Get("name");
            const auto* pUrl = asset.Get("url");
            if (pName && pUrl)
                aRelease.assets[pName->text] = pUrl->text;
        }
    }

    const auto manifestAsset = aRelease.assets.find("manifest.txt");
    if (manifestAsset == aRelease.assets.end())
    {
        aError = "the latest release has no manifest.txt";
        return false;
    }

    std::string manifest;
    if (!DownloadAsset(acConfig, manifestAsset->second,
                       [&](const char* apData, size_t aSize, uint64_t)
                       {
                           manifest.append(apData, aSize);
                           return manifest.size() < (16 << 20);
                       },
                       aError))
        return false;

    if (!ParseManifest(manifest, aRelease))
    {
        aError = "the release's manifest.txt is damaged";
        return false;
    }
    return true;
}

// Versions are the tag and the commits since it: v1.6.7-173. Releases up to v1.6.7-172 also named the commit
// (v1.6.7-121-ga620cde7). A test build newer than the latest release isn't offered the older one. Names that don't parse
// count as newer when they differ.
bool IsNewer(const std::string& acVersion, const std::string& acThan)
{
    const auto parse = [](std::string aText, std::string& aTag, uint64_t& aCount)
    {
        if (const auto commit = aText.rfind("-g"); commit != std::string::npos && commit + 2 < aText.size() &&
                                                   aText.find_first_not_of("0123456789abcdef", commit + 2) == std::string::npos)
            aText.resize(commit);
        const auto count = aText.rfind('-');
        if (count == std::string::npos || count + 1 == aText.size() || aText.find_first_not_of("0123456789", count + 1) != std::string::npos)
            return false;
        aTag = aText.substr(0, count);
        aCount = std::strtoull(aText.c_str() + count + 1, nullptr, 10);
        return true;
    };

    std::string tag, thanTag;
    uint64_t count = 0, thanCount = 0;
    if (!parse(acVersion, tag, count) || !parse(acThan, thanTag, thanCount) || tag != thanTag)
        return acVersion != acThan;
    return count > thanCount;
}

Config s_config;
Release s_release;

void RunCheck()
{
    SetStatus(State::kChecking);

    if (!LoadConfig(s_config))
    {
        spdlog::info("Updater: no updater.cfg, not checking for updates");
        SetStatus(State::kUpToDate);
        return;
    }

    std::string error;
    Release release;
    if (!FetchLatestRelease(s_config, release, error))
    {
        spdlog::warn("Updater: couldn't check for updates: {}", error);
        SetStatus(State::kUpToDate);
        return;
    }

    if (release.version.empty() || release.version == BUILD_COMMIT || !IsNewer(release.version, BUILD_COMMIT))
    {
        spdlog::info("Updater: up to date ({}, latest release {})", BUILD_COMMIT, release.version.empty() ? "none" : release.version);
        SetStatus(State::kUpToDate);
        return;
    }

    spdlog::info("Updater: {} is available, this is {}", release.version, BUILD_COMMIT);
    {
        std::scoped_lock lock(s_lock);
        s_release = std::move(release);
        s_status.latestVersion = s_release.version;
        s_status.state = State::kAvailable;
        s_status.message.clear();
    }
}

// --- Installing ---

std::vector<const ManifestFile*> FindChangedFiles(const std::filesystem::path& acRoot)
{
    std::vector<const ManifestFile*> changed;
    for (const auto& file : s_release.files)
    {
        const auto path = acRoot / file.path;
        std::error_code ec;
        if (std::filesystem::file_size(path, ec) != file.size || ec || HashFile(path) != file.sha256)
            changed.push_back(&file);
    }
    return changed;
}

// Unpacks a zip with Windows' own tar (as the crash report export does)
bool Unzip(const std::filesystem::path& acZip, const std::filesystem::path& acFolder)
{
    wchar_t systemDir[MAX_PATH]{};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    std::wstring commandLine = L"\"" + (std::filesystem::path(systemDir) / L"tar.exe").wstring() + L"\" -x -f \"" + acZip.wstring() + L"\" -C \"" + acFolder.wstring() + L"\"";

    STARTUPINFOW startupInfo{sizeof(startupInfo)};
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, acFolder.wstring().c_str(), &startupInfo, &processInfo))
        return false;

    WaitForSingleObject(processInfo.hProcess, 10 * 60 * 1000);
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return exitCode == 0;
}

// Servers running from this install: they'd keep running the old version (and they hold their files)
void StopOurServers(const std::filesystem::path& acAppFolder)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    const std::wstring folder = acAppFolder.wstring() + L"\\";
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        if (_wcsicmp(entry.szExeFile, L"ReclamationDayServer.exe") != 0)
            continue;

        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t path[MAX_PATH * 2]{};
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size) && _wcsnicmp(path, folder.c_str(), folder.size()) == 0)
        {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 5000);
            spdlog::info("Updater: stopped the server running from this install (process {})", entry.th32ProcessID);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
}

// The game's Data folder (where the launcher was pointed at)
std::filesystem::path GetGameDataFolder()
{
    wchar_t path[MAX_PATH * 2]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TiltedPhoques\\TiltedEvolution\\Fallout4", L"TitlePath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return {};
    return std::filesystem::path(path) / L"Data";
}

std::filesystem::path WithSuffix(const std::filesystem::path& acPath, const wchar_t* apSuffix)
{
    return acPath.wstring() + apSuffix;
}

bool RunUpdate(std::string& aError)
{
    const auto root = GetRoot();
    const auto appFolder = GetAppFolder();

    SetStatus(State::kUpdating, "CHECKING YOUR FILES...");
    const auto changed = FindChangedFiles(root);
    spdlog::info("Updater: {} of {} files to update to {}", changed.size(), s_release.files.size(), s_release.version);

    std::map<std::string, std::vector<const ManifestFile*>> byPack;
    for (const auto* pFile : changed)
        byPack[pFile->pack].push_back(pFile);

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    const std::filesystem::path work = std::filesystem::path(tempPath) / L"ReclamationDayUpdate";
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work, ec);

    // Download and unpack the packs with changes, check every file
    size_t packIndex = 0;
    for (const auto& [pack, files] : byPack)
    {
        ++packIndex;
        const std::string assetName = pack + ".zip";
        const auto asset = s_release.assets.find(assetName);
        if (asset == s_release.assets.end())
        {
            aError = "the release has no " + assetName;
            return false;
        }

        const auto zipPath = work / Widen(assetName);
        std::ofstream zip(zipPath, std::ios::binary);
        uint64_t done = 0;
        int lastPercent = -1;
        const bool downloaded = DownloadAsset(s_config, asset->second,
                                              [&](const char* apData, size_t aSize, uint64_t aLength)
                                              {
                                                  done += aSize;
                                                  const int percent = aLength ? static_cast<int>(done * 100 / aLength) : 0;
                                                  if (percent != lastPercent)
                                                  {
                                                      lastPercent = percent;
                                                      SetStatus(State::kUpdating, fmt::format("DOWNLOADING THE UPDATE ({} OF {})... {}%", packIndex, byPack.size(), percent), percent);
                                                  }
                                                  zip.write(apData, static_cast<std::streamsize>(aSize));
                                                  return zip.good();
                                              },
                                              aError);
        zip.close();
        if (!downloaded)
            return false;

        SetStatus(State::kUpdating, "UNPACKING THE UPDATE...");
        const auto unpacked = work / Widen(pack);
        std::filesystem::create_directories(unpacked, ec);
        if (!Unzip(zipPath, unpacked))
        {
            aError = "couldn't unpack " + assetName;
            return false;
        }

        for (const auto* pFile : files)
        {
            const auto source = unpacked / pFile->path;
            if (std::filesystem::file_size(source, ec) != pFile->size || ec || HashFile(source) != pFile->sha256)
            {
                aError = "a downloaded file didn't check out (" + pFile->path.string() + "), try again";
                return false;
            }
        }
    }

    // Everything checked out: put the new files next to the old ones, then swap them. A running exe can't be
    // overwritten, but it can be renamed.
    SetStatus(State::kUpdating, "INSTALLING THE UPDATE...");
    StopOurServers(appFolder);

    std::vector<std::filesystem::path> staged;
    const auto discardStaged = [&]
    {
        for (const auto& path : staged)
            std::filesystem::remove(path, ec);
    };
    for (const auto* pFile : changed)
    {
        const auto target = WithSuffix(root / pFile->path, kNewSuffix);
        std::filesystem::create_directories(target.parent_path(), ec);
        if (!CopyFileW((work / Widen(pFile->pack) / pFile->path).c_str(), target.c_str(), FALSE))
        {
            discardStaged();
            aError = fmt::format("couldn't write {} (error {})", target.string(), GetLastError());
            return false;
        }
        staged.push_back(target);
    }

    std::vector<std::pair<std::filesystem::path, bool>> swapped; // path, had an old copy
    for (const auto* pFile : changed)
    {
        const auto path = root / pFile->path;
        const auto oldPath = WithSuffix(path, kOldSuffix);
        const bool hadOld = std::filesystem::exists(path, ec);
        if ((hadOld && !MoveFileExW(path.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) ||
            !MoveFileExW(WithSuffix(path, kNewSuffix).c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            const DWORD error = GetLastError();
            if (hadOld && !std::filesystem::exists(path, ec))
                MoveFileExW(oldPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);

            // Put back what was already swapped
            for (const auto& [swappedPath, swappedHadOld] : swapped)
            {
                if (swappedHadOld)
                    MoveFileExW(WithSuffix(swappedPath, kOldSuffix).c_str(), swappedPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                else
                    std::filesystem::remove(swappedPath, ec);
            }
            discardStaged();
            aError = fmt::format("couldn't replace {} (error {}), is something using it?", pFile->path.string(), error);
            return false;
        }
        swapped.emplace_back(path, hadOld);
    }

    // The old copies go, except those still in use (this launcher, the overlay): the next start removes them
    for (const auto& [path, hadOld] : swapped)
    {
        if (hadOld)
            std::filesystem::remove(WithSuffix(path, kOldSuffix), ec);
    }

    // Our game files in the game's Data folder get the update too, new ones included: once any of ours is there, the
    // player installed them into Data (by hand or with a mod manager), so all of them belong there. (Only updating
    // the ones already there left 1.6.7-135's new climb sounds out.)
    if (const auto data = GetGameDataFolder(); !data.empty())
    {
        // Where a file of "Game Files" goes in Data; empty for the ones at its top (CREDITS.txt), which stay out
        const std::filesystem::path gameFiles = L"Game Files";
        const auto inData = [&gameFiles](const ManifestFile& acFile) -> std::filesystem::path
        {
            const auto relative = acFile.path.lexically_relative(gameFiles);
            if (relative.empty() || *relative.begin() == L".." || !relative.has_parent_path())
                return {};
            return relative;
        };

        const bool installedInData = std::ranges::any_of(s_release.files,
                                                         [&](const ManifestFile& acFile)
                                                         {
                                                             const auto relative = inData(acFile);
                                                             return !relative.empty() && std::filesystem::exists(data / relative, ec);
                                                         });

        if (!installedInData)
            spdlog::info("Updater: our game files aren't in the game's Data folder, leaving it alone");
        else
        {
            for (const auto* pFile : changed)
            {
                const auto relative = inData(*pFile);
                if (relative.empty())
                    continue;

                const auto target = data / relative;
                const bool existed = std::filesystem::exists(target, ec);
                std::filesystem::create_directories(target.parent_path(), ec);
                if (CopyFileW((root / pFile->path).c_str(), target.c_str(), FALSE))
                    spdlog::info("Updater: {} {} in the game's Data folder", existed ? "updated" : "added", relative.string());
                else
                    spdlog::warn("Updater: couldn't copy {} into the game's Data folder (error {})", relative.string(), GetLastError());
            }
        }
    }

    std::filesystem::remove_all(work, ec);
    spdlog::info("Updater: installed {}", s_release.version);
    return true;
}

// Old copies of files that were in use during the last update. Only in our folders (the root may be e.g. the desktop).
void RemoveOldCopies()
{
    std::error_code ec;
    const auto removeIfOld = [&](const std::filesystem::path& acPath)
    {
        if (acPath.extension() == kOldSuffix)
            std::filesystem::remove(acPath, ec);
    };

    for (std::filesystem::directory_iterator it(GetRoot(), ec), end; !ec && it != end; it.increment(ec))
        removeIfOld(it->path());

    for (const auto& folder : {GetAppFolder(), GetRoot() / L"Game Files"})
    {
        for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec))
            removeIfOld(it->path());
        ec.clear();
    }
}

// Our own plugins, at the top of "Game Files" (after the host's other plugins in the load order)
constexpr const char* kOurPlugins[]{"RDVehicles.esp", "RD_Companions.esp", "RD_Combat.esp", "RD_Perks.esp", "RD_Camps.esp", "RD_Nemeses.esp", "RD_GrapplingGun.esp", "RD_DualWield.esp", "RD_Social.esp"};

// A voice file of one of our plugins' lines: the game reads them from Sound\Voice\<plugin>\<voice type>\, a place the
// plugin's name fixes, so they can't be in a ReclamationDay folder
bool IsOurVoiceFile(const std::filesystem::path& acRelative)
{
    auto part = acRelative.begin();
    if (part == acRelative.end() || _wcsicmp(part->c_str(), L"sound") != 0 || ++part == acRelative.end() || _wcsicmp(part->c_str(), L"Voice") != 0 ||
        ++part == acRelative.end())
        return false;
    const std::string plugin = part->string();
    return std::any_of(std::begin(kOurPlugins), std::end(kOurPlugins), [&plugin](const char* acpOurs) { return _stricmp(plugin.c_str(), acpOurs) == 0; });
}

// Our own game files (those in a "ReclamationDay" folder of Game Files: our animations, sounds and scripts; and our
// plugins' voice files) are kept in the game's Data folder at every start: missing or different ones are copied in. A
// player updated by an older launcher, whose updater didn't add new game files (1.6.7-135's climb sounds), gets them
// this way without doing anything. The rest of Game Files (the motorcycle's F4SE plugin and models, the dogs' looks) goes
// where the launcher's window puts it: the player's mod manager, or Data.
void SyncGameFilesToData()
{
    const auto data = GetGameDataFolder();
    const auto gameFiles = GetRoot() / L"Game Files";
    std::error_code ec;
    if (data.empty() || !std::filesystem::is_directory(data, ec) || !std::filesystem::is_directory(gameFiles, ec))
        return;

    int added = 0, updated = 0;
    for (std::filesystem::recursive_directory_iterator it(gameFiles, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
    {
        if (!it->is_regular_file(ec) || it->path().extension() == kOldSuffix || it->path().extension() == kNewSuffix)
            continue;

        // Ours: somewhere under a folder named ReclamationDay, or one of our plugins' voice files
        const auto relative = it->path().lexically_relative(gameFiles);
        const auto folders = relative.parent_path();
        const bool ours = std::any_of(folders.begin(), folders.end(),
                                      [](const std::filesystem::path& acPart) { return _wcsicmp(acPart.c_str(), L"ReclamationDay") == 0; }) ||
                          IsOurVoiceFile(relative);
        if (!ours)
            continue;

        const auto target = data / relative;
        const bool existed = std::filesystem::exists(target, ec);
        if (existed && std::filesystem::file_size(target, ec) == it->file_size(ec) && HashFile(target) == HashFile(it->path()))
            continue;

        std::filesystem::create_directories(target.parent_path(), ec);
        if (CopyFileW(it->path().c_str(), target.c_str(), FALSE))
        {
            ++(existed ? updated : added);
            spdlog::info("Updater: {} {} in the game's Data folder", existed ? "updated" : "added", relative.string());
        }
        else
            spdlog::warn("Updater: couldn't copy {} into the game's Data folder (error {})", relative.string(), GetLastError());
    }

    if (added || updated)
        spdlog::info("Updater: our game files in the game's Data folder: {} added, {} updated", added, updated);
}

// Our other files the game reads from a fixed place in Data (PipboyTabs reads Data\PBT\*.ini): kept there as our plugins
constexpr const wchar_t* kOurDataFiles[]{L"PBT\\RD_NemesesTab.ini", L"PBT\\RD_SocialTab.ini"};

// Plugins one of ours replaces: switched off in plugins.txt once ours is in Data (both on would set the same game
// settings twice, and joining checks the plugins against the host's)
struct ReplacedPlugin
{
    const char* pPlugin;
    const char* pReplacedBy;
};
constexpr ReplacedPlugin kReplacedPlugins[]{
    {"Adaptive Combat Tactics.esp", "RD_Combat.esp"},
    {"Adaptive Combat Tactics - IncreasedPlayerDetectionRange.esp", "RD_Combat.esp"},
};

// plugins.txt, the game's load order (empty without a local app data folder)
std::filesystem::path GetPluginsFile()
{
    PWSTR pLocalAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pLocalAppData)))
        return {};
    std::filesystem::path path = std::filesystem::path(pLocalAppData) / L"Fallout4" / L"plugins.txt";
    CoTaskMemFree(pLocalAppData);
    return path;
}

std::string ReadPluginsFile(const std::filesystem::path& acPluginsFile)
{
    std::ifstream file(acPluginsFile, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

// Writes plugins.txt, the old one kept next to it as plugins.txt.<time>.reclamationday-backup (one already made this
// second, by an earlier change at the same start, is kept as it is). Returns the backup, or empty when plugins.txt
// wasn't changed (the old one is put back if writing fails).
std::filesystem::path WritePluginsFile(const std::filesystem::path& acPluginsFile, const std::string& acText)
{
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &time);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const auto backup = acPluginsFile.parent_path() / fmt::format("plugins.txt.{}.reclamationday-backup", stamp);
    std::error_code ec;
    if (!std::filesystem::exists(backup, ec) && !std::filesystem::copy_file(acPluginsFile, backup, ec))
    {
        spdlog::error("Updater: couldn't back up plugins.txt ({})", ec.message());
        return {};
    }

    {
        std::ofstream file(acPluginsFile, std::ios::binary | std::ios::trunc);
        file << acText;
        if (file)
            return backup;
    }
    spdlog::error("Updater: couldn't write plugins.txt");
    std::filesystem::copy_file(backup, acPluginsFile, std::filesystem::copy_options::overwrite_existing, ec);
    return {};
}

// The mods bundled into Reclamation Day (the nature overhaul remade from Boston Natural Surroundings): their files,
// listed in Game Files\ReclamationDay\BundledMods.txt (Tools/ListBundledMods.js), are kept in Data like our plugins,
// missing or different ones copied in. Everyone on a server needs them (joining checks the plugins), so they're not
// left to the player's mod manager. Returns the plugins among them. KARMA, Be Exceptional, PipboyTabs, LevelUpMenuEx and
// Extended Dialogue Interface were bundled up to 1.6.7-285 and are downloads since (Requirements.h): the copies earlier
// starts put in Data stay there.
std::vector<std::string> InstallBundledMods(const std::filesystem::path& acData)
{
    std::vector<std::string> plugins;
    const auto gameFiles = GetRoot() / L"Game Files";
    std::ifstream list(gameFiles / L"ReclamationDay" / L"BundledMods.txt");
    if (!list)
        return plugins;

    std::error_code ec;
    int added = 0, updated = 0, missing = 0;
    for (std::string line; std::getline(list, line);)
    {
        line = Trim(line);
        // comments, and "[folder]" naming the mod the lines below belong to
        if (line.empty() || line[0] == '#' || line[0] == '[')
            continue;

        const std::filesystem::path relative(line);
        const auto source = gameFiles / relative;
        if (!std::filesystem::is_regular_file(source, ec))
        {
            ++missing;
            continue;
        }
        const std::string extension = ToLower(relative.extension().string());
        if (!relative.has_parent_path() && (extension == ".esm" || extension == ".esp" || extension == ".esl"))
            plugins.push_back(line);

        // The same size and time (a copy keeps the time) is the same file: hashing the nature overhaul's archives (about
        // 870 MB) at every start would hold the game up for seconds. The same contents with another time get its time.
        const auto target = acData / relative;
        const bool existed = std::filesystem::exists(target, ec);
        if (existed && std::filesystem::file_size(target, ec) == std::filesystem::file_size(source, ec))
        {
            const auto time = std::filesystem::last_write_time(source, ec);
            if (std::filesystem::last_write_time(target, ec) == time)
                continue;
            if (HashFile(target) == HashFile(source))
            {
                std::filesystem::last_write_time(target, time, ec);
                continue;
            }
        }

        std::filesystem::create_directories(target.parent_path(), ec);
        if (CopyFileW(source.c_str(), target.c_str(), FALSE))
            ++(existed ? updated : added);
        else
            spdlog::warn("Updater: couldn't copy {} into the game's Data folder (error {})", line, GetLastError());
    }

    if (added || updated)
        spdlog::info("Updater: the bundled mods' files in the game's Data folder: {} added, {} updated", added, updated);
    if (missing)
        spdlog::warn("Updater: {} of the bundled mods' files aren't in Game Files", missing);
    return plugins;
}

// Our plugins in Data that plugins.txt doesn't list at all go on at its end (one the player switched off stays off), and
// the ones they replace go off; then the bundled mods' plugins, after ours (so a player who had ours already gets the
// same order as a new one). The old plugins.txt stays next to it. Without one (a mod manager keeping its own) nothing
// changes.
void SwitchOnOurPlugins(const std::filesystem::path& acData, const std::vector<std::string>& acBundled)
{
    const auto pluginsFile = GetPluginsFile();
    std::error_code ec;
    if (pluginsFile.empty() || !std::filesystem::is_regular_file(pluginsFile, ec))
        return;

    std::string text = ReadPluginsFile(pluginsFile);

    // One plugin per line, active ones start with '*'; the replaced ones lose their '*'
    std::set<std::string> listed;
    std::string switchedOff;
    std::string rewritten;
    std::istringstream lines(text);
    for (std::string rawLine; std::getline(lines, rawLine);)
    {
        std::string line = Trim(rawLine);
        const bool active = !line.empty() && line[0] == '*';
        if (active)
            line.erase(0, 1);
        listed.insert(ToLower(line));

        bool replaced = false;
        for (const auto& [pPlugin, pReplacedBy] : kReplacedPlugins)
            replaced |= active && ToLower(line) == ToLower(pPlugin) && std::filesystem::exists(acData / pReplacedBy, ec);
        if (replaced)
        {
            switchedOff += switchedOff.empty() ? line : fmt::format(", {}", line);
            rewritten += line + "\r\n";
        }
        else
            rewritten += rawLine + "\n";
    }
    if (!switchedOff.empty())
        text = rewritten;

    std::vector<std::string> plugins(std::begin(kOurPlugins), std::end(kOurPlugins));
    plugins.insert(plugins.end(), acBundled.begin(), acBundled.end());
    std::string missing;
    for (const std::string& plugin : plugins)
    {
        if (!std::filesystem::exists(acData / plugin, ec) || listed.contains(ToLower(plugin)))
            continue;
        if (!text.empty() && text.back() != '\n')
            text += "\r\n";
        text += fmt::format("*{}\r\n", plugin);
        missing += missing.empty() ? plugin : fmt::format(", {}", plugin);
    }
    if (missing.empty() && switchedOff.empty())
        return;

    const auto backup = WritePluginsFile(pluginsFile, text);
    if (backup.empty())
    {
        spdlog::error("Updater: {} not switched on", missing);
        return;
    }
    if (!missing.empty())
        spdlog::info("Updater: switched on {} in plugins.txt (backup {})", missing, backup.filename().string());
    if (!switchedOff.empty())
        spdlog::info("Updater: switched off {} in plugins.txt, replaced by our own (backup {})", switchedOff, backup.filename().string());
}

// Winter of Atom (RD_WinterOfAtom.esp, its own download) is a prequel in a snowbound Commonwealth: the plugins its
// Data\RDWoA\PrequelPlugins.txt lists (the winter mods and their patches) load only in that campaign, so 2287 games
// stay as they are. The campaign is Documents\My Games\Fallout4\ReclamationDay\campaign.txt: "winter-of-atom", or
// the 2287 game ("commonwealth", also with no file). The game reads its load order once, so changing campaign takes a
// restart.
constexpr char kWinterOfAtomPlugin[] = "RD_WinterOfAtom.esp";
constexpr char kPrequelCampaign[] = "winter-of-atom";
constexpr char kCommonwealthCampaign[] = "commonwealth";

// What this start switched for (GetCampaign; set at start on the main thread, read after): the campaign, and the
// prequel's plugins in lowercase
std::string s_campaign;
std::vector<std::string> s_prequelPlugins;

std::filesystem::path GetCampaignFile()
{
    const auto folder = SaveFolder::GetGameUserFolder();
    return folder.empty() ? std::filesystem::path{} : folder / L"ReclamationDay" / L"campaign.txt";
}

std::string ReadCampaign()
{
    std::string campaign;
    if (const auto file = GetCampaignFile(); !file.empty())
    {
        std::ifstream stream(file);
        std::getline(stream, campaign);
    }
    campaign = ToLower(Trim(campaign));
    return campaign.empty() ? kCommonwealthCampaign : campaign;
}

// A list of plugins, one per line in load order (# comments): those in Data
std::vector<std::string> ReadPluginList(const std::filesystem::path& acData, const std::filesystem::path& acList, const char* apWhat)
{
    std::error_code ec;
    std::vector<std::string> plugins;
    std::ifstream list(acList);
    for (std::string line; list && std::getline(list, line);)
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (std::filesystem::exists(acData / line, ec))
            plugins.push_back(line);
        else
            spdlog::warn("Updater: {} plugin {} isn't in the game's Data folder", apWhat, line);
    }
    return plugins;
}

// The prequel's plugins (Data\RDWoA\PrequelPlugins.txt) go on in plugins.txt for the prequel and off for any other
// campaign; the 2287 Commonwealth's own (Game Files\ReclamationDay\CommonwealthPlugins.txt, ours: what doesn't belong in
// a snowbound Commonwealth, such as the nature overhaul; read from Game Files, as Data gets its copy only after this
// start's check) the other way round. Their archives ("<plugin> - Main.ba2") load only with them. Nothing changes
// without Winter of Atom in Data, or without a plugins.txt (a mod manager keeping its own).
void SwitchPrequelPlugins(const std::filesystem::path& acData)
{
    std::error_code ec;
    const auto pluginsFile = GetPluginsFile();
    if (!std::filesystem::exists(acData / kWinterOfAtomPlugin, ec) || pluginsFile.empty() || !std::filesystem::is_regular_file(pluginsFile, ec))
        return;

    const std::vector<std::string> prequelPlugins = ReadPluginList(acData, acData / L"RDWoA" / L"PrequelPlugins.txt", "prequel");
    const std::vector<std::string> commonwealthPlugins = ReadPluginList(acData, GetRoot() / L"Game Files" / L"ReclamationDay" / L"CommonwealthPlugins.txt", "2287 Commonwealth");
    if (prequelPlugins.empty() && commonwealthPlugins.empty())
        return;

    const std::string campaign = ReadCampaign();
    const bool prequel = campaign == kPrequelCampaign;
    // The main menu offers the other campaign and keeps their saves apart once this start's plugins are the campaign's
    const auto remember = [&]()
    {
        if (prequelPlugins.empty())
            return;
        s_campaign = prequel ? kPrequelCampaign : kCommonwealthCampaign;
        s_prequelPlugins.clear();
        for (const std::string& plugin : prequelPlugins)
            s_prequelPlugins.push_back(ToLower(plugin));
    };
    std::vector<std::string> switchedOn, switchedOff;
    std::string text = PluginsTxt::SetActive(ReadPluginsFile(pluginsFile), prequelPlugins, prequel, prequel ? switchedOn : switchedOff);
    text = PluginsTxt::SetActive(text, commonwealthPlugins, !prequel, prequel ? switchedOff : switchedOn);
    if (switchedOn.empty() && switchedOff.empty())
    {
        spdlog::info("Updater: campaign {}, its {} prequel plugins already {}, its {} 2287 Commonwealth plugins already {}", campaign, prequelPlugins.size(),
                     prequel ? "on" : "off", commonwealthPlugins.size(), prequel ? "off" : "on");
        remember();
        return;
    }

    const auto join = [](const std::vector<std::string>& acNames)
    {
        std::string names;
        for (const std::string& name : acNames)
            names += names.empty() ? name : fmt::format(", {}", name);
        return names.empty() ? std::string("nothing") : names;
    };
    const auto backup = WritePluginsFile(pluginsFile, text);
    if (backup.empty())
    {
        spdlog::error("Updater: campaign {}, plugins.txt not written: {} not switched on, {} not switched off", campaign, join(switchedOn), join(switchedOff));
        return;
    }
    spdlog::info("Updater: campaign {}, switched on {}, switched off {} in plugins.txt (backup {})", campaign, join(switchedOn), join(switchedOff), backup.filename().string());
    remember();
}
} // namespace

const std::string& GetCampaign() noexcept
{
    return s_campaign;
}

bool SetCampaign(const std::string& acCampaign) noexcept
{
    const auto file = GetCampaignFile();
    if (file.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << acCampaign << '\n';
    if (!stream)
    {
        spdlog::error("Updater: couldn't write the campaign {} to {}", acCampaign, file.string());
        return false;
    }
    spdlog::info("Updater: the next start plays the campaign {}", acCampaign);
    return true;
}

bool IsKeptInData(const std::filesystem::path& acRelative) noexcept
{
    const auto folders = acRelative.parent_path();
    if (std::any_of(folders.begin(), folders.end(), [](const std::filesystem::path& acPart) { return _wcsicmp(acPart.c_str(), L"ReclamationDay") == 0; }) ||
        IsOurVoiceFile(acRelative))
        return true;

    if (!acRelative.has_parent_path() &&
        std::any_of(std::begin(kOurPlugins), std::end(kOurPlugins), [&acRelative](const char* acpPlugin) { return _stricmp(acRelative.string().c_str(), acpPlugin) == 0; }))
        return true;
    if (std::any_of(std::begin(kOurDataFiles), std::end(kOurDataFiles), [&acRelative](const wchar_t* acpFile) { return _wcsicmp(acRelative.c_str(), acpFile) == 0; }))
        return true;

    // the bundled mods' files, as InstallBundledMods reads their list
    static const std::set<std::string> s_bundled = []
    {
        std::set<std::string> files;
        std::ifstream list(GetRoot() / L"Game Files" / L"ReclamationDay" / L"BundledMods.txt");
        for (std::string line; std::getline(list, line);)
        {
            line = Trim(line);
            if (!line.empty() && line[0] != '#' && line[0] != '[')
                files.insert(ToLower(std::filesystem::path(line).lexically_normal().string()));
        }
        return files;
    }();
    return s_bundled.contains(ToLower(acRelative.lexically_normal().string()));
}

bool IsOfThisCampaign(const std::vector<std::string>& acSavePlugins) noexcept
{
    if (s_campaign.empty())
        return true;
    const bool prequelSave = std::ranges::any_of(acSavePlugins, [](const std::string& acPlugin) { return std::ranges::find(s_prequelPlugins, acPlugin) != s_prequelPlugins.end(); });
    return prequelSave == (s_campaign == kPrequelCampaign);
}

// Plugins that must load after every other: the Previsibines Repair Pack's PRP.esp rebuilds the precombined meshes and
// previs of the cells it covers, so any plugin after it that edits one of those cells undoes them there (flicker,
// objects missing, the frame rate it won back gone). Its framework PPF.esm is a master: the game loads it with the
// masters wherever plugins.txt lists it.
constexpr const char* kLastPlugins[]{"PRP.esp"};

// Every start, after our own changes to plugins.txt (which add plugins at its end): those plugins moved back to its end.
// A mod manager or a mod added later may have put plugins after them. Nothing changes when they're already last.
void KeepLastPlugins()
{
    const auto pluginsFile = GetPluginsFile();
    std::error_code ec;
    if (pluginsFile.empty() || !std::filesystem::is_regular_file(pluginsFile, ec))
        return;

    std::vector<std::string> moved;
    const std::string text = PluginsTxt::MoveToEnd(ReadPluginsFile(pluginsFile), {std::begin(kLastPlugins), std::end(kLastPlugins)}, moved);
    if (moved.empty())
        return;

    std::string names;
    for (const std::string& plugin : moved)
        names += names.empty() ? plugin : fmt::format(", {}", plugin);
    const auto backup = WritePluginsFile(pluginsFile, text);
    if (backup.empty())
        spdlog::error("Updater: {} not moved to the end of the load order", names);
    else
        spdlog::info("Updater: {} moved to the end of the load order, after every plugin that could undo it (backup {})", names, backup.filename().string());
}

void InstallOurPlugins() noexcept
{
    const auto data = GetGameDataFolder();
    std::error_code ec;
    if (data.empty() || !std::filesystem::is_directory(data, ec))
        return;

    const auto gameFiles = GetRoot() / L"Game Files";
    for (const wchar_t* pFile : kOurDataFiles)
    {
        const auto source = gameFiles / pFile;
        const auto target = data / pFile;
        if (!std::filesystem::is_regular_file(source, ec))
            continue;
        const bool existed = std::filesystem::exists(target, ec);
        if (existed && std::filesystem::file_size(target, ec) == std::filesystem::file_size(source, ec) && HashFile(target) == HashFile(source))
            continue;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (CopyFileW(source.c_str(), target.c_str(), FALSE))
            spdlog::info("Updater: {} {} in the game's Data folder", existed ? "updated" : "added", target.lexically_relative(data).string());
        else
            spdlog::warn("Updater: couldn't copy {} into the game's Data folder (error {})", target.lexically_relative(data).string(), GetLastError());
    }

    for (const char* pPlugin : kOurPlugins)
    {
        const auto source = gameFiles / pPlugin;
        if (!std::filesystem::is_regular_file(source, ec))
            continue;

        const auto target = data / pPlugin;
        const bool existed = std::filesystem::exists(target, ec);
        if (existed && std::filesystem::file_size(target, ec) == std::filesystem::file_size(source, ec) && HashFile(target) == HashFile(source))
            continue;

        if (CopyFileW(source.c_str(), target.c_str(), FALSE))
            spdlog::info("Updater: {} {} in the game's Data folder", existed ? "updated" : "added", pPlugin);
        else
            spdlog::warn("Updater: couldn't copy {} into the game's Data folder (error {})", pPlugin, GetLastError());
    }

    SwitchOnOurPlugins(data, InstallBundledMods(data));
    SwitchPrequelPlugins(data);
    KeepLastPlugins();
}

void StartCheck() noexcept
{
    if (s_checkStarted.exchange(true))
        return;

    std::thread(
        []
        {
            RemoveOldCopies();
            SyncGameFilesToData();
            RunCheck();
        })
        .detach();
}

void StartWindowCheck() noexcept
{
    if (s_checkStarted.exchange(true))
        return;

    std::thread(
        []
        {
            RemoveOldCopies();
            RunCheck();
        })
        .detach();
}

void StartUpdate() noexcept
{
    {
        std::scoped_lock lock(s_lock);
        if (s_status.state != State::kAvailable && s_status.state != State::kFailed)
            return;
        s_status.state = State::kUpdating;
        s_status.message = "PREPARING THE UPDATE...";
        s_status.percent = -1;
        s_status.error.clear();
    }

    std::thread(
        []
        {
            std::string error;
            if (RunUpdate(error))
            {
                SetStatus(State::kReady, "UPDATE INSTALLED, RESTARTING...");
                return;
            }

            spdlog::error("Updater: the update failed: {}", error);
            {
                std::scoped_lock lock(s_lock);
                s_status.error = error;
            }
            SetStatus(State::kFailed, "THE UPDATE FAILED: " + ToUpper(error) + ". NOTHING WAS CHANGED.");
        })
        .detach();
}

Status GetStatus() noexcept
{
    std::scoped_lock lock(s_lock);
    return s_status;
}

void Restart(bool aStraightToGame) noexcept
{
    wchar_t exePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

    SetEnvironmentVariableW(L"RD_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    if (aStraightToGame)
        SetEnvironmentVariableW(L"RD_SKIP_LAUNCHER", L"1");

    std::wstring commandLine = GetCommandLineW();
    STARTUPINFOW startupInfo{sizeof(startupInfo)};
    PROCESS_INFORMATION processInfo{};
    if (CreateProcessW(exePath, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, GetAppFolder().c_str(), &startupInfo, &processInfo))
    {
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        spdlog::info("Updater: restarting");
    }
    else
        spdlog::error("Updater: couldn't start the updated launcher (error {})", GetLastError());

    CrashHandler::FlushLog();
    TerminateProcess(GetCurrentProcess(), 0);
    for (;;)
        Sleep(INFINITE);
}
} // namespace Updater

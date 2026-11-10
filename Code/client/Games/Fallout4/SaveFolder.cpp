#include <HivemindPCH.h>

#include <SaveFolder.h>
#include <SaveLoad.h>

#include <ShlObj.h>
#include <shellapi.h>
#include <cctype>
#include <fstream>
#include <random>

namespace SaveFolder
{
namespace
{
struct Setting
{
    void* vtable;    // 00
    char* value;     // 08, for string settings
    const char* key; // 10, "name:Section"
};

struct Node
{
    Setting* item;
    Node* next;
};

Setting* FindLocalSavePathSetting() noexcept
{
    // INISettingCollection singleton, CommonLibF4 id 791183; its settings list (BSSimpleList) is at 0x118
    POINTER_FALLOUT4(uint8_t*, s_iniSettings, 791184);
    uint8_t* pCollection = *s_iniSettings.Get();
    if (!pCollection)
        return nullptr;

    for (auto* pNode = reinterpret_cast<Node*>(pCollection + 0x118); pNode; pNode = pNode->next)
    {
        if (pNode->item && pNode->item->key && _stricmp(pNode->item->key, "sLocalSavePath:General") == 0)
            return pNode->item;
    }
    return nullptr;
}

// FO4 save header strings: 16-bit length, then the characters
bool ReadString(std::ifstream& aFile, std::string& aOut) noexcept
{
    uint16_t length = 0;
    if (!aFile.read(reinterpret_cast<char*>(&length), sizeof(length)) || length > 512)
        return false;
    aOut.resize(length);
    return static_cast<bool>(aFile.read(aOut.data(), length));
}

// The save's plugins, right after the screenshot: u8 form version, the game's version (a string), u32 the plugin
// info's size, u8 count + names, and from form version 68 u16 light count + names (1.10.163 saves are 68). Lowercase.
std::vector<std::string> ReadPlugins(std::ifstream& aFile, uint64_t aOffset) noexcept
{
    std::vector<std::string> plugins;
    uint8_t formVersion = 0, count = 0;
    uint32_t infoSize = 0;
    std::string gameVersion;
    aFile.clear();
    if (!aFile.seekg(static_cast<std::streamoff>(aOffset)) || !aFile.read(reinterpret_cast<char*>(&formVersion), 1) || !ReadString(aFile, gameVersion) ||
        !aFile.read(reinterpret_cast<char*>(&infoSize), sizeof(infoSize)) || !aFile.read(reinterpret_cast<char*>(&count), 1))
        return {};

    const auto readNames = [&](size_t aCount)
    {
        for (size_t i = 0; i < aCount; ++i)
        {
            std::string name;
            if (!ReadString(aFile, name))
                return false;
            std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            plugins.push_back(std::move(name));
        }
        return true;
    };
    if (!readNames(count))
        return {};
    uint16_t lightCount = 0;
    if (formVersion >= 68 && (!aFile.read(reinterpret_cast<char*>(&lightCount), sizeof(lightCount)) || !readNames(lightCount)))
        return {};
    return plugins;
}

// "Save1_3A33F9D2_74657374_ConcordMuseum01_...": the character id is the second part
std::string ReadCharacterId(const std::string& acFileName) noexcept
{
    const size_t start = acFileName.find('_');
    if (start == std::string::npos || acFileName.size() < start + 1 + 8)
        return {};

    std::string id = acFileName.substr(start + 1, 8);
    const bool isHex = std::all_of(id.begin(), id.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
    return isHex && (acFileName.size() == start + 9 || acFileName[start + 9] == '_') ? id : std::string{};
}

// "Autosave2_..." -> "Autosave", 2
void ReadKind(const std::string& acFileName, std::string& aKind, uint32_t& aNumber) noexcept
{
    const std::string prefix = acFileName.substr(0, acFileName.find('_'));
    size_t digits = prefix.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(prefix[digits - 1])))
        --digits;

    aKind = prefix.substr(0, digits);
    aNumber = digits < prefix.size() ? static_cast<uint32_t>(std::strtoul(prefix.c_str() + digits, nullptr, 10)) : 0;
}

std::string Base64(const std::vector<uint8_t>& acData) noexcept
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve((acData.size() + 2) / 3 * 4);
    for (size_t i = 0; i < acData.size(); i += 3)
    {
        const uint32_t chunk = acData[i] << 16 | (i + 1 < acData.size() ? acData[i + 1] << 8 : 0) | (i + 2 < acData.size() ? acData[i + 2] : 0);
        out += kAlphabet[chunk >> 18 & 63];
        out += kAlphabet[chunk >> 12 & 63];
        out += i + 1 < acData.size() ? kAlphabet[chunk >> 6 & 63] : '=';
        out += i + 2 < acData.size() ? kAlphabet[chunk & 63] : '=';
    }
    return out;
}

// BGSSaveLoadManager::QueueSaveLoadTask (CommonLibF4 id 1487308; Game.RequestAutosave calls it with kAutoSave)
void QueueSaveLoadTask(int32_t aTask) noexcept
{
    using TQueueSaveLoadTask = void(BGSSaveLoadManager*, int32_t);
    POINTER_FALLOUT4(TQueueSaveLoadTask, s_queueSaveLoadTask, 1487309);

    if (auto* pManager = BGSSaveLoadManager::Get())
        s_queueSaveLoadTask.Get()(pManager, aTask);
}

std::string WithCharacterId(const std::string& acFileName, const std::string& acCharacterId) noexcept
{
    if (acCharacterId.empty() || ReadCharacterId(acFileName).empty())
        return acFileName;

    std::string name = acFileName;
    name.replace(name.find('_') + 1, 8, acCharacterId);
    return name;
}

// Copies or moves the save and its F4SE co-save, under the given character
bool TransferSave(const SaveInfo& acSave, const std::filesystem::path& acFolder, const std::string& acCharacterId, bool aMove, std::filesystem::path* apTarget = nullptr) noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(acFolder, ec);

    auto coSave = acSave.file;
    coSave.replace_extension(".f4se");
    const bool hasCoSave = std::filesystem::exists(coSave, ec);

    const auto target = acFolder / WithCharacterId(acSave.file.filename().string(), acCharacterId);
    auto coTarget = target;
    coTarget.replace_extension(".f4se");

    const auto transfer = [aMove](const std::filesystem::path& acFrom, const std::filesystem::path& acTo, std::error_code& aError)
    {
        if (aMove)
            std::filesystem::rename(acFrom, acTo, aError);
        else
            std::filesystem::copy_file(acFrom, acTo, std::filesystem::copy_options::overwrite_existing, aError);
    };

    transfer(acSave.file, target, ec);
    if (ec)
    {
        spdlog::error("Couldn't {} {} to {}: {}", aMove ? "move" : "copy", acSave.file.filename().string(), acFolder.string(), ec.message());
        return false;
    }
    if (hasCoSave)
        transfer(coSave, coTarget, ec);

    spdlog::info("{} {} to {}", aMove ? "Moved" : "Copied", acSave.file.filename().string(), target.string());
    if (apTarget)
        *apTarget = target;
    return true;
}
} // namespace

bool SaveInfo::IsPastVault111() const noexcept
{
    if (location == "Vault 111")
        return false;

    // The pre-war morning in Sanctuary Hills: level 1, a few minutes in
    if (location == "Sanctuary Hills" && level <= 1 && playTime.starts_with("0d.0h."))
        return false;

    return true;
}

std::string GetLocalSavePath() noexcept
{
    const Setting* pSetting = FindLocalSavePathSetting();
    return pSetting && pSetting->value ? pSetting->value : "Saves\\";
}

bool SetLocalSavePath(const std::string& acPath) noexcept
{
    Setting* pSetting = FindLocalSavePathSetting();
    if (!pSetting)
    {
        spdlog::error("{}: sLocalSavePath not found", __FUNCTION__);
        return false;
    }

    // The game keeps pointing at the string: kept alive for the whole process (a few short strings at most)
    static std::list<std::string> s_values;
    const auto it = std::find(s_values.begin(), s_values.end(), acPath);
    const std::string& value = it != s_values.end() ? *it : s_values.emplace_back(acPath);
    pSetting->value = const_cast<char*>(value.c_str());

    std::error_code ec;
    std::filesystem::create_directories(GetGameUserFolder() / acPath, ec);

    InvalidateSaveList();
    spdlog::info("Save folder is now {}", acPath);
    return true;
}

std::filesystem::path GetGameUserFolder() noexcept
{
    PWSTR pDocuments = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &pDocuments)))
        return {};
    std::filesystem::path folder = std::filesystem::path(pDocuments) / L"My Games" / L"Fallout4";
    CoTaskMemFree(pDocuments);
    return folder;
}

std::vector<SaveInfo> ListSaves(const std::filesystem::path& acFolder) noexcept
{
    std::vector<SaveInfo> saves;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(acFolder, ec))
    {
        if (entry.path().extension() != ".fos")
            continue;

        std::ifstream file(entry.path(), std::ios::binary);
        char magic[12]{};
        uint32_t headerSize = 0, version = 0, saveNumber = 0;
        file.read(magic, sizeof(magic));
        file.read(reinterpret_cast<char*>(&headerSize), sizeof(headerSize));
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        file.read(reinterpret_cast<char*>(&saveNumber), sizeof(saveNumber));
        if (!file || std::memcmp(magic, "FO4_SAVEGAME", sizeof(magic)) != 0)
            continue;

        SaveInfo save;
        save.file = entry.path();
        save.modified = entry.last_write_time(ec);
        save.characterId = ReadCharacterId(entry.path().filename().string());
        ReadKind(entry.path().filename().string(), save.kind, save.number);
        if (!ReadString(file, save.playerName) || !file.read(reinterpret_cast<char*>(&save.level), sizeof(save.level)) || !ReadString(file, save.location) ||
            !ReadString(file, save.playTime))
            continue;

        // The rest of the header: race, sex, current and next level XP, file time, then the screenshot's size. The
        // screenshot itself follows the header.
        std::string race;
        uint16_t sex = 0;
        float xp[2]{};
        uint64_t fileTime = 0;
        uint32_t size[2]{};
        if (ReadString(file, race) && file.read(reinterpret_cast<char*>(&sex), sizeof(sex)) && file.read(reinterpret_cast<char*>(xp), sizeof(xp)) &&
            file.read(reinterpret_cast<char*>(&fileTime), sizeof(fileTime)) && file.read(reinterpret_cast<char*>(size), sizeof(size)))
        {
            save.screenshotWidth = size[0];
            save.screenshotHeight = size[1];
            save.screenshotOffset = 16 + static_cast<uint64_t>(headerSize);
            save.plugins = ReadPlugins(file, save.screenshotOffset + static_cast<uint64_t>(size[0]) * size[1] * 4);
        }

        // "0d.4h.4m.0 days.4 hours.4 minutes": the short part is enough
        if (const size_t dot = save.playTime.find('m'); dot != std::string::npos)
            save.playTime.resize(dot + 1);

        saves.push_back(std::move(save));
    }

    std::sort(saves.begin(), saves.end(), [](const auto& acLhs, const auto& acRhs) { return acLhs.modified > acRhs.modified; });
    return saves;
}

void InvalidateSaveList() noexcept
{
    // The game's own clear (CommonLibF4 id 1513177), what its load panel calls before asking for the list again: empties
    // saveGameList (deleting its entries) and resets isSaveListBuilt and saveGameCount. Only resetting isSaveListBuilt
    // made the game add the new folder's saves to the old list, so the load screen showed the saves of every folder used.
    using TClearSaveGameList = void(BGSSaveLoadManager*);
    POINTER_FALLOUT4(TClearSaveGameList, s_clearSaveGameList, 1513178);

    if (auto* pManager = BGSSaveLoadManager::Get())
        s_clearSaveGameList.Get()(pManager);
}

bool CopySave(const SaveInfo& acSave, const std::filesystem::path& acFolder, const std::string& acCharacterId, std::filesystem::path* apTarget) noexcept
{
    return TransferSave(acSave, acFolder, acCharacterId, false, apTarget);
}

std::string ReadThumbnail(const SaveInfo& acSave, uint32_t aMaxWidth) noexcept
{
    const uint32_t width = acSave.screenshotWidth, height = acSave.screenshotHeight;
    if (width == 0 || height == 0 || width > 4096 || height > 4096 || aMaxWidth == 0)
        return {};

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    std::ifstream file(acSave.file, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(acSave.screenshotOffset));
    if (!file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size())))
        return {};

    // Box filter by a whole factor
    const uint32_t factor = std::max(1u, (width + aMaxWidth - 1) / aMaxWidth);
    const uint32_t outWidth = width / factor, outHeight = height / factor;
    const uint32_t rowSize = (outWidth * 3 + 3) & ~3u;

    // 24-bit BMP, top-down (negative height)
    std::vector<uint8_t> bmp(54 + static_cast<size_t>(rowSize) * outHeight);
    const auto put32 = [&bmp](size_t aOffset, uint32_t aValue) { std::memcpy(&bmp[aOffset], &aValue, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<uint32_t>(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, outWidth);
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(outHeight)));
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, static_cast<uint32_t>(bmp.size() - 54));

    for (uint32_t y = 0; y < outHeight; ++y)
    {
        for (uint32_t x = 0; x < outWidth; ++x)
        {
            uint32_t sum[3]{};
            for (uint32_t dy = 0; dy < factor; ++dy)
            {
                const uint8_t* pRow = &pixels[(static_cast<size_t>(y * factor + dy) * width + x * factor) * 4];
                for (uint32_t dx = 0; dx < factor; ++dx)
                {
                    sum[0] += pRow[dx * 4];
                    sum[1] += pRow[dx * 4 + 1];
                    sum[2] += pRow[dx * 4 + 2];
                }
            }
            uint8_t* pOut = &bmp[54 + static_cast<size_t>(y) * rowSize + x * 3];
            const uint32_t count = factor * factor;
            pOut[0] = static_cast<uint8_t>(sum[2] / count); // BMP is BGR
            pOut[1] = static_cast<uint8_t>(sum[1] / count);
            pOut[2] = static_cast<uint8_t>(sum[0] / count);
        }
    }

    return "data:image/bmp;base64," + Base64(bmp);
}

void BuildSaveList() noexcept
{
    // The game's builder (CommonLibF4 id 1342984): (manager, id of the character whose saves to list, 0 for all), how
    // the load screen calls it when the list isn't built. Queuing the kBuildSaveGameList task instead does nothing: the
    // manager's task processing doesn't handle that one.
    using TBuildSaveGameList = void(BGSSaveLoadManager*, uint64_t);
    POINTER_FALLOUT4(TBuildSaveGameList, s_buildSaveGameList, 1342985);

    if (auto* pManager = BGSSaveLoadManager::Get())
        s_buildSaveGameList.Get()(pManager, 0);
}

bool LoadSave(const std::string& acName) noexcept
{
    auto* pManager = reinterpret_cast<uint8_t*>(BGSSaveLoadManager::Get());
    if (!pManager)
        return false;

    // saveGameList, a BSTArray<BGSSaveLoadFileEntry*> at 0x08 (data at 0x08, size at 0x18). An entry starts with its file
    // name (without extension).
    auto** ppEntries = *reinterpret_cast<uint8_t***>(pManager + 0x08);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pManager + 0x18);
    for (uint32_t i = 0; ppEntries && i < count; ++i)
    {
        const char* pName = ppEntries[i] ? *reinterpret_cast<const char**>(ppEntries[i]) : nullptr;
        if (!pName || _stricmp(pName, acName.c_str()) != 0)
            continue;

        // What the load screen does: queuedEntryToLoad (0x48) and kLoadGame (0x40). The manager's task processing then
        // loads that entry (id 357407, the only caller of the load itself besides the console's "load").
        *reinterpret_cast<uint8_t**>(pManager + 0x48) = ppEntries[i];
        QueueSaveLoadTask(0x40);
        spdlog::info("Loading save {}", acName);
        return true;
    }

    spdlog::warn("Save {} not in the game's save list ({} saves)", acName, count);
    return false;
}

bool MoveSave(const SaveInfo& acSave, const std::filesystem::path& acFolder, const std::string& acCharacterId) noexcept
{
    return TransferSave(acSave, acFolder, acCharacterId, true);
}

bool DeleteSave(const SaveInfo& acSave) noexcept
{
    std::error_code ec;
    auto coSave = acSave.file;
    coSave.replace_extension(".f4se");

    // The Recycle Bin rather than gone for good, so a wrong pick can be put back. SHFileOperation takes the files as one
    // string, each ending in a null and the list in another.
    std::wstring files = acSave.file.wstring() + L'\0';
    if (std::filesystem::exists(coSave, ec))
        files += coSave.wstring() + L'\0';
    files += L'\0';

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = files.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&operation);

    if (std::filesystem::exists(acSave.file, ec))
    {
        spdlog::error("Couldn't delete {} (error {})", acSave.file.filename().string(), result);
        return false;
    }
    spdlog::info("Deleted {} (in the Recycle Bin)", acSave.file.filename().string());
    return true;
}

std::string NewCharacterId() noexcept
{
    std::random_device device;
    uint32_t id = 0;
    while (id == 0)
        id = device();
    return fmt::format("{:08X}", id);
}

uint64_t GetCurrentCharacterId() noexcept
{
    // BGSSaveLoadManager::currentPlayerID (0xD0, alandtse/CommonLibF4)
    auto* pManager = BGSSaveLoadManager::Get();
    return pManager ? *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(pManager) + 0xD0) : 0;
}

void SaveAndQuitToMainMenu() noexcept
{
    // kSaveAndQuit (0x4000): what the pause menu's quit to the main menu queues (kSaveAndQuitToDesktop is 0x10000)
    QueueSaveLoadTask(0x4000);
}
} // namespace SaveFolder

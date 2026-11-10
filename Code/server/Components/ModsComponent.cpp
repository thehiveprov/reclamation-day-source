
#include <Components.h>
#include <es_loader/ESLoader.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
// plugin-ids.txt: a header line, then "<id>\t<standard|lite>\t<file name>" per plugin the world has known
constexpr char kIdsFileName[] = "plugin-ids.txt";
constexpr char kIdsHeader[] = "# Reclamation Day plugin ids v1";
// Ids go out as 16 bits (Mods)
constexpr uint32_t kMaxId = 0xFFFF;

std::filesystem::path GetIdsFile() noexcept
{
    if (const wchar_t* pFolder = _wgetenv(L"RD_WORLD_DATA"); pFolder && *pFolder)
        return std::filesystem::path(pFolder) / kIdsFileName;
    return std::filesystem::current_path() / "data" / kIdsFileName;
}

String Lower(const String& acText) noexcept
{
    String text = acText;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const ModsComponent::Entry* FindByName(const ModsComponent::TModList& acList, const String& acpFilename) noexcept
{
    const String name = Lower(acpFilename);
    for (const auto& [filename, entry] : acList)
    {
        if (Lower(filename) == name)
            return &entry;
    }
    return nullptr;
}
} // namespace

uint32_t ModsComponent::Add(TModList& aList, const TModList& acOther, const String& acpFilename) noexcept
{
    const auto itor = aList.find(acpFilename);
    if (itor != std::end(aList))
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    // The same file known by another case, or as the other kind (a plugin flagged light since, or no longer): its id
    if (const Entry* pKnown = FindByName(aList, acpFilename); pKnown)
    {
        const uint32_t id = pKnown->id;
        aList.emplace(acpFilename, Entry{id, 1});
        return id;
    }
    if (const Entry* pKnown = FindByName(acOther, acpFilename); pKnown)
    {
        const uint32_t id = pKnown->id;
        aList.emplace(acpFilename, Entry{id, 1});
        SaveIds();
        return id;
    }

    if (m_seed > kMaxId)
        spdlog::error("Plugin ids: this world has given out all {} ids, {} gets {:X} again", kMaxId + 1, acpFilename.c_str(), kMaxId);
    const auto id = std::min(m_seed++, kMaxId);
    aList.emplace(acpFilename, Entry{id, 1});
    SaveIds();

    return id;
}

uint32_t ModsComponent::AddStandard(const String& acpFilename) noexcept
{
    return Add(m_standardMods, m_liteMods, acpFilename);
}

uint32_t ModsComponent::AddLite(const String& acpFilename) noexcept
{
    return Add(m_liteMods, m_standardMods, acpFilename);
}

void ModsComponent::LoadSavedIds() noexcept
{
    const auto file = GetIdsFile();
    std::ifstream stream(file);
    if (!stream)
        return;

    size_t count = 0;
    for (std::string line; std::getline(stream, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        const auto tab1 = line.find('\t');
        const auto tab2 = tab1 == std::string::npos ? std::string::npos : line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos)
        {
            spdlog::warn("Plugin ids: line isn't readable, skipped: {}", line);
            continue;
        }
        const uint32_t id = std::strtoul(line.substr(0, tab1).c_str(), nullptr, 10);
        const std::string kind = line.substr(tab1 + 1, tab2 - tab1 - 1);
        const String filename = line.substr(tab2 + 1).c_str();
        if (filename.empty() || id > kMaxId)
            continue;

        (kind == "lite" ? m_liteMods : m_standardMods).emplace(filename, Entry{id, 0});
        m_seed = std::max(m_seed, id + 1);
        ++count;
    }

    m_hadSavedIds = count != 0;
    spdlog::info("Plugin ids: {} plugins this world has known, from {} (new ones from id {})", count, file.string(), m_seed);
}

void ModsComponent::CompareLoadOrder(const Vector<String>& acFilenames, Vector<String>& aNew, Vector<String>& aGone) const noexcept
{
    if (!m_hadSavedIds)
        return;

    Vector<String> current;
    for (const String& filename : acFilenames)
    {
        current.push_back(Lower(filename));
        if (!FindByName(m_standardMods, filename) && !FindByName(m_liteMods, filename))
            aNew.push_back(filename);
    }
    for (const TModList* pList : {&m_standardMods, &m_liteMods})
    {
        for (const auto& [filename, entry] : *pList)
        {
            if (std::find(current.begin(), current.end(), Lower(filename)) == current.end() &&
                std::none_of(aGone.begin(), aGone.end(), [&filename](const String& acGone) { return Lower(acGone) == Lower(filename); }))
                aGone.push_back(filename);
        }
    }
}

void ModsComponent::SaveIds() const noexcept
{
    const auto file = GetIdsFile();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    // Written aside, then put in place, so a crash mid-write leaves the old one
    const auto temp = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream stream(temp, std::ios::trunc);
        stream << kIdsHeader << "\n";
        for (const auto& [filename, entry] : m_standardMods)
            stream << entry.id << "\tstandard\t" << filename.c_str() << "\n";
        for (const auto& [filename, entry] : m_liteMods)
            stream << entry.id << "\tlite\t" << filename.c_str() << "\n";
        if (!stream)
        {
            spdlog::error("Plugin ids: couldn't write {}", temp.string());
            return;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec)
        spdlog::error("Plugin ids: couldn't replace {} ({})", file.string(), ec.message());
}

void ModsComponent::AddServerMod(const ESLoader::PluginData& acData)
{
    // kind of a hack since we want to store both, so we take the two byte value
    m_serverMods.emplace(acData.m_filename, Entry{acData.m_liteId, 1});
}

bool ModsComponent::IsInstalled(const String& acpFilename) const noexcept
{
    auto it = std::find_if(m_serverMods.begin(), m_serverMods.end(), [&](const TModList::value_type& aEntry) { return aEntry.first == acpFilename; });

    return it != m_serverMods.end();
}

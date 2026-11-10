#include <HivemindPCH.h>

#include <CoopWorlds.h>
#include <MiniJson.h>
#include <Interface/Updater.h>

#include <fstream>
#include <map>
#include <sstream>

namespace CoopWorlds
{
namespace
{
constexpr char kWorldFile[] = "world.json";

void WriteWorldFile(const World& acWorld) noexcept
{
    std::ofstream file(SaveFolder::GetGameUserFolder() / acWorld.saveFolder / kWorldFile, std::ios::trunc);
    file << "{\n  \"name\": " << MiniJson::Quote(acWorld.name) << ",\n  \"type\": " << MiniJson::Quote(acWorld.hosted ? "hosted" : "joined")
         << ",\n  \"address\": " << MiniJson::Quote(acWorld.address) << ",\n  \"privacy\": " << MiniJson::Quote(acWorld.privacy) << "\n}\n";
}

// A world's folder: its world.json, or for the folders made before world.json existed, what the folder name says
// ("Hosted", or a joined server's address with ':' as '_')
World ReadWorld(const std::string& acBaseSaveFolder, const std::string& acId) noexcept
{
    World world;
    world.id = acId;
    world.saveFolder = GetRootFolder(acBaseSaveFolder) + acId + "\\";

    std::ifstream file(SaveFolder::GetGameUserFolder() / world.saveFolder / kWorldFile);
    if (file)
    {
        std::stringstream content;
        content << file.rdbuf();
        const std::string json = content.str();
        world.name = MiniJson::GetString(json, "name");
        world.hosted = MiniJson::GetString(json, "type") == "hosted";
        world.address = MiniJson::GetString(json, "address");
        // (worlds made before there was a privacy: open to friends, asked first)
        if (const std::string privacy = MiniJson::GetString(json, "privacy"); privacy == "invite" || privacy == "private")
            world.privacy = privacy;
    }
    else
    {
        world.hosted = acId == "Hosted";
        if (world.hosted)
            world.name = "Hosted World";
        else
        {
            world.address = acId;
            if (const size_t port = world.address.rfind('_'); port != std::string::npos)
                world.address[port] = ':';
            world.name = world.address;
        }
        WriteWorldFile(world);
        spdlog::info("CoopWorlds: described the older folder {} as {} world \"{}\"", acId, world.hosted ? "a hosted" : "a joined", world.name);
    }

    if (world.name.empty())
        world.name = world.hosted ? "Hosted World" : world.address;
    return world;
}
} // namespace

std::string GetRootFolder(const std::string& acBaseSaveFolder) noexcept
{
    return acBaseSaveFolder + "Coop\\";
}

std::vector<World> ListWorlds(const std::string& acBaseSaveFolder) noexcept
{
    std::vector<std::pair<std::filesystem::file_time_type, World>> worlds;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(SaveFolder::GetGameUserFolder() / GetRootFolder(acBaseSaveFolder), ec))
    {
        if (!entry.is_directory(ec))
            continue;

        World world = ReadWorld(acBaseSaveFolder, entry.path().filename().string());

        // Most recently played first, then newest
        auto lastPlayed = entry.last_write_time(ec);
        for (const auto& save : SaveFolder::ListSaves(entry.path()))
            lastPlayed = std::max(lastPlayed, save.modified);
        worlds.emplace_back(lastPlayed, std::move(world));
    }

    std::sort(worlds.begin(), worlds.end(), [](const auto& acLhs, const auto& acRhs) { return acLhs.first > acRhs.first; });

    std::vector<World> result;
    for (auto& [time, world] : worlds)
        result.push_back(std::move(world));
    return result;
}

std::optional<World> FindWorld(const std::string& acBaseSaveFolder, const std::string& acId) noexcept
{
    if (acId.empty() || acId.find_first_of("\\/.:") != std::string::npos)
        return std::nullopt;

    std::error_code ec;
    if (!std::filesystem::is_directory(SaveFolder::GetGameUserFolder() / GetRootFolder(acBaseSaveFolder) / acId, ec))
        return std::nullopt;

    return ReadWorld(acBaseSaveFolder, acId);
}

World CreateWorld(const std::string& acBaseSaveFolder, const std::string& acName, bool aHosted, const std::string& acAddress) noexcept
{
    World world;
    world.id = "W" + SaveFolder::NewCharacterId();
    world.name = acName.empty() ? (aHosted ? "Hosted World" : acAddress) : acName;
    world.hosted = aHosted;
    world.address = aHosted ? std::string{} : acAddress;
    world.saveFolder = GetRootFolder(acBaseSaveFolder) + world.id + "\\";

    std::error_code ec;
    std::filesystem::create_directories(SaveFolder::GetGameUserFolder() / world.saveFolder, ec);
    WriteWorldFile(world);

    spdlog::info("CoopWorlds: created {} world \"{}\" ({}){}", aHosted ? "hosted" : "joined", world.name, world.id, aHosted ? "" : " on " + world.address);
    return world;
}

void SaveWorld(const World& acWorld) noexcept
{
    WriteWorldFile(acWorld);
}

std::vector<Character> ListCharacters(const World& acWorld) noexcept
{
    std::vector<Character> characters;
    std::map<std::string, size_t> indices;

    // Saves come newest first, so characters end up in the order they were last played. The other campaign's saves
    // stay out: loaded with this campaign's plugins they would lose or gain plugins (Updater::IsOfThisCampaign).
    for (auto& save : SaveFolder::ListSaves(SaveFolder::GetGameUserFolder() / acWorld.saveFolder))
    {
        if (!Updater::IsOfThisCampaign(save.plugins))
            continue;
        auto [it, inserted] = indices.try_emplace(save.characterId, characters.size());
        if (inserted)
            characters.push_back(Character{save.characterId, {}});
        characters[it->second].saves.push_back(std::move(save));
    }
    return characters;
}

void AdoptLooseSaves(const std::string& acBaseSaveFolder) noexcept
{
    const auto userFolder = SaveFolder::GetGameUserFolder();
    const auto looseSaves = SaveFolder::ListSaves(userFolder / GetRootFolder(acBaseSaveFolder));
    if (looseSaves.empty())
        return;

    const World world = CreateWorld(acBaseSaveFolder, "Earlier Co-op Saves", true, {});

    // Each character becomes a co-op character of its own
    std::map<std::string, std::string> newIds;
    for (const auto& save : looseSaves)
    {
        auto& id = newIds[save.characterId];
        if (id.empty())
            id = SaveFolder::NewCharacterId();
        SaveFolder::MoveSave(save, userFolder / world.saveFolder, id);
    }
    spdlog::info("CoopWorlds: moved {} co-op saves from before worlds into \"{}\"", looseSaves.size(), world.name);
}
} // namespace CoopWorlds

#pragma once

#include <SaveFolder.h>

// Co-op worlds: a world is one multiplayer playthrough tied to its server, kept in a save folder of its own under the
// player's normal one (<saves>\Coop\<id>\) with a world.json naming it. A hosted world runs on this player's own
// server, a joined one on another player's (by its address). Its characters are the saves grouped by character id.
namespace CoopWorlds
{
struct World
{
    std::string id; // the folder name
    std::string name;
    bool hosted = false;
    std::string address;    // joined worlds: "host:port", or "steam:<SteamID>" for a friend's joined through Steam
    std::string saveFolder; // relative to Documents\My Games\Fallout4, for SaveFolder::SetLocalSavePath
    // hosted worlds: who may join through Steam ("friends", asked first; "invite", invited friends only; "private")
    std::string privacy = "friends";
};

struct Character
{
    std::string id; // from the save file names
    std::vector<SaveFolder::SaveInfo> saves; // newest first
};

// <base>Coop\, where the worlds' folders are
std::string GetRootFolder(const std::string& acBaseSaveFolder) noexcept;

std::vector<World> ListWorlds(const std::string& acBaseSaveFolder) noexcept;
std::optional<World> FindWorld(const std::string& acBaseSaveFolder, const std::string& acId) noexcept;
World CreateWorld(const std::string& acBaseSaveFolder, const std::string& acName, bool aHosted, const std::string& acAddress) noexcept;
// Its world.json written again (a changed privacy)
void SaveWorld(const World& acWorld) noexcept;

// Newest played first
std::vector<Character> ListCharacters(const World& acWorld) noexcept;

// Co-op saves left directly in Coop\ (from before there were worlds) become a hosted world of their own
void AdoptLooseSaves(const std::string& acBaseSaveFolder) noexcept;
} // namespace CoopWorlds

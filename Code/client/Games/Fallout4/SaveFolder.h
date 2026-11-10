#pragma once

// The game's save folder and its save files (Fallout 4).
namespace SaveFolder
{
struct SaveInfo
{
    std::filesystem::path file;
    std::string playerName;
    uint32_t level = 0;
    std::string location;
    std::string playTime; // "0d.4h.4m"
    std::filesystem::file_time_type modified{};
    // The character the save belongs to: the 8 hex digits after the first '_' of the file name. It isn't stored in the
    // file itself, the game groups saves by it and gives new saves the id of the loaded one.
    std::string characterId;
    // From the file name: "Save", "Autosave", "Quicksave" or "Exitsave", and its number
    std::string kind;
    uint32_t number = 0;
    // The screenshot right after the header: RGBA, screenshotWidth x screenshotHeight
    uint32_t screenshotWidth = 0;
    uint32_t screenshotHeight = 0;
    uint64_t screenshotOffset = 0;
    // The plugins the game had loaded, light ones too, in lowercase (after the screenshot); empty when unreadable
    std::vector<std::string> plugins;

    // Saves made in the prologue (before leaving Vault 111) can't be used for multiplayer
    [[nodiscard]] bool IsPastVault111() const noexcept;
    // The file name without its extension, how the game names saves
    [[nodiscard]] std::string GetName() const noexcept { return file.stem().string(); }
};

// sLocalSavePath:General, relative to Documents\My Games\Fallout4 (e.g. "Saves\"), changed in memory only
std::string GetLocalSavePath() noexcept;
bool SetLocalSavePath(const std::string& acPath) noexcept;

// Documents\My Games\Fallout4
std::filesystem::path GetGameUserFolder() noexcept;

// The .fos saves of a folder, newest first, with their header read
std::vector<SaveInfo> ListSaves(const std::filesystem::path& acFolder) noexcept;

// The save list is built once; after changing the save folder it must be read again
void InvalidateSaveList() noexcept;

// Copies or moves a save (and its F4SE co-save) into a folder, returns false if it failed. With a character id, the
// save is renamed to belong to that character instead.
bool CopySave(const SaveInfo& acSave, const std::filesystem::path& acFolder, const std::string& acCharacterId = {}, std::filesystem::path* apTarget = nullptr) noexcept;
bool MoveSave(const SaveInfo& acSave, const std::filesystem::path& acFolder, const std::string& acCharacterId = {}) noexcept;

// Sends a save (and its F4SE co-save) to the Recycle Bin, returns false if the save is still there
bool DeleteSave(const SaveInfo& acSave) noexcept;

// The save's screenshot, scaled down to at most aMaxWidth pixels wide, as a "data:image/bmp;base64," URL (empty if the
// save has none)
std::string ReadThumbnail(const SaveInfo& acSave, uint32_t aMaxWidth) noexcept;

// Loading a given save the way the game's load screen does: BuildSaveList() lists the saves of the current folder,
// then LoadSave() queues the load of the entry with that name; false if it isn't in the list
void BuildSaveList() noexcept;
bool LoadSave(const std::string& acName) noexcept;

// A new random character id, 8 hex digits like the game's
std::string NewCharacterId() noexcept;

// The character of the loaded game (BGSSaveLoadManager::currentPlayerID)
uint64_t GetCurrentCharacterId() noexcept;

// What the pause menu's QUIT does: an exit save, then back to the main menu
void SaveAndQuitToMainMenu() noexcept;
} // namespace SaveFolder

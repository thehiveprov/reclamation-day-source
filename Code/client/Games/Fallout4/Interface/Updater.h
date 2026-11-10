#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Updates Reclamation Day (launcher, server, overlay, our game files) to the latest released version, from the
// releases repo (Tools\PublishUpdate.ps1 makes the releases).
//
// The launcher's window checks when it opens and holds Play until a newer release is installed; the main menu checks too
// and offers it (for a start that skipped the window, or a release that came out since). An update needs a restart:
// everything is downloaded and checked first, then the files are swapped in, a server running from this install is
// stopped, and the launcher starts again.
namespace Updater
{
enum class State
{
    kIdle,
    kChecking,
    kUpToDate,  // or the check failed: nothing to offer
    kAvailable, // a newer release than this build
    kUpdating,
    kFailed, // the update failed, nothing was changed; it can be tried again
    kReady   // swapped in: restart
};

struct Status
{
    State state{State::kIdle};
    std::string latestVersion;
    std::string message; // progress or error, for the main menu
    int percent{-1};     // how far the download of the current pack is, -1 when no number applies
    std::string error;   // why the last try failed (kFailed), in plain case for the launcher's window
};

// Our own plugins (the dogs, the motorcycles) from "Game Files" into the game's Data folder, switched on in plugins.txt
// if it doesn't list them yet. At start, before the game reads its load order: an update only brings them into "Game
// Files" (it leaves the files at its top out of Data). Also switches Winter of Atom's prequel-only plugins on or off
// for the chosen campaign.
void InstallOurPlugins() noexcept;

// Starts the check in the background (once), after keeping our game files in Data up to date
void StartCheck() noexcept;

// The same for the launcher's window, without the game files: Play waits for this check, and the game does the rest
void StartWindowCheck() noexcept;

// Starts downloading and installing the available update in the background
void StartUpdate() noexcept;

Status GetStatus() noexcept;

// Starts the updated launcher (or this one again) and ends this one. The new one waits for this process to be gone first
// (RD_WAIT_PID, see the launcher's main), or it would take itself for a second game instance. After an update the
// launcher's window shows, with what's new in its patch notes; aStraightToGame skips it (RD_SKIP_LAUNCHER) for a
// campaign switch or a load order matched to a host's, which come straight back to the game. The skip is the signal,
// not the showing: an update's restart runs in the old version, which knows of no signal.
[[noreturn]] void Restart(bool aStraightToGame = false) noexcept;

// The campaign this start switched plugins.txt for (InstallOurPlugins): "commonwealth" (2287) or "winter-of-atom" (the
// prequel). Empty when there was nothing to switch: Winter of Atom isn't in Data, or there's no plugins.txt (a mod
// manager keeps the load order).
[[nodiscard]] const std::string& GetCampaign() noexcept;
// The campaign the next start switches to (Documents\My Games\Fallout4\ReclamationDay\campaign.txt); false when it
// couldn't be written
bool SetCampaign(const std::string& acCampaign) noexcept;
// A save of the campaign this start runs: one made with any of the prequel's plugins loaded is Winter of Atom's.
// Always true when there's no campaign (above). The save's plugins in lowercase.
[[nodiscard]] bool IsOfThisCampaign(const std::vector<std::string>& acSavePlugins) noexcept;

// A file of "Game Files" (its path there) that every start keeps in the game's Data folder itself: ours (in a
// ReclamationDay folder, our plugins, their voice files, the Pip-Boy tabs' settings) and the bundled mods' files. The
// launcher stages the rest of Game Files for the player's mod manager.
[[nodiscard]] bool IsKeptInData(const std::filesystem::path& acRelative) noexcept;
} // namespace Updater

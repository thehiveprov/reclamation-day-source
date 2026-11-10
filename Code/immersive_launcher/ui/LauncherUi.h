#pragma once

#include <Windows.h>

// The launcher's window: the game install, where Game Files' mods go (the mod staging folder), the patch notes, the
// launcher's look, and Play. Its page is Code/launcher_ui ("Reclamation Day\launcher" in an install), shown with the
// overlay's CEF by a second start of this exe (--launcher-ui): CEF starts only once per process, and this one starts
// it again for the game's overlay.
namespace launcher::ui
{
enum class Choice
{
    kPlay,       // start the game
    kQuit,       // the window was closed
    kUnavailable // the window couldn't show (no CEF, no page): go on as before it existed
};

// The window's process exit codes
constexpr int kExitPlay = 0;
constexpr int kExitFailed = 1;
constexpr int kExitQuit = 2;
constexpr int kExitUpdated = 3; // an update is in: the launcher starts the new version, whose window shows

// To the window, from any thread: end it with the exit code in wParam; drag it by its title bar; minimize it
constexpr UINT kCloseMessage = WM_APP + 1;
constexpr UINT kDragMessage = WM_APP + 2;
constexpr UINT kMinimizeMessage = WM_APP + 3;

// In the launcher: shows the window and waits for the choice. After an update installed from the window this doesn't
// return: the new version starts instead.
Choice Show() noexcept;

// In the window's process: runs it and returns its exit code; -1 when the command line isn't --launcher-ui
int RunIfAsked(int aArgc, char** aArgv);
} // namespace launcher::ui

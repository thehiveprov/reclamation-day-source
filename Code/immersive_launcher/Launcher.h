#pragma once

#include "Loader/ExeLoader.h"
#include <TiltedCore/Filesystem.hpp>
#include <TiltedCore/Stl.hpp>

namespace launcher
{
namespace fs = std::filesystem;

enum class Result
{
    kSuccess,
    kBadPlatform,
    kBadInstall
};

// stays alive through the entire duration of the game.
struct LaunchContext
{
    fs::path exePath;
    fs::path gamePath;
    TiltedPhoques::String Version;
    ExeLoader::TEntryPoint gameMain = nullptr;
};

LaunchContext* GetLaunchContext();

bool LoadProgram(LaunchContext&);
// aStraightToGame: started again by the game itself for a campaign switch or a load order matched to a host's, which
// skips the launcher's window (an update's restart shows it)
int StartUp(int argc, char** argv, bool aStraightToGame);

void InitClient();

// aShowWindow is cleared when the arguments pick the game themselves (--exePath, --no-launcher)
bool HandleArguments(int, char**, bool& aAskSelect, bool& aShowWindow);

} // namespace launcher

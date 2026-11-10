#pragma once

class CrashHandler
{
public:
    CrashHandler();
    ~CrashHandler();

    static void RemovePreviousDump(std::filesystem::path path);
    // Every frame of the game, on its main thread: a game that stops drawing frames for a while gets a dump of where
    // it's stuck (crash_..._freeze.dmp). aLoadingScreen: the frame is behind the game's loading screen, where a save's
    // load can draw nothing for a minute or more, so it gets far longer before it counts as stuck
    static void OnFrame(bool aLoadingScreen = false) noexcept;
    // Zips leftover crash dumps in the background (crash_*.dmp -> crash_*.zip)
    static void CompressDumps(std::filesystem::path path);
    // The log is written by a thread of its own (HivemindApp): what's queued is on disk before this returns (up to a
    // second), for a game about to crash or end
    static void FlushLog() noexcept;
};

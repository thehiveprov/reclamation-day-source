#pragma once

// Runs every frame from the main loop (menus included), see MainMenuIntegration.cpp
void RunMainMenuIntegration() noexcept;

// Whether the loaded game is a co-op save, the only kind multiplayer is played in (started from MULTIPLAYER)
bool IsPlayingCoopSave() noexcept;

// This game hosts the world it plays: it started the world's server, which runs on this PC (the host decides its
// weather, time and difficulty)
bool IsHostingWorld() noexcept;

// An action from the overlay's multiplayer screens (any thread), run on the next main loop frame
void QueueMainMenuAction(std::string aAction, std::string aPayload) noexcept;

// Joining a friend's world through Steam (SteamService, main thread): its world here (one for each host,
// "steam:<SteamID>", made the first time) opens on the multiplayer screen to pick a character; from a world being
// played, once it's saved and left
void JoinSteamWorld(uint64_t aHostSteamId, std::string aWorldName) noexcept;
// A hosted world's privacy set and kept in its world.json
void SetHostedWorldPrivacy(const std::string& acWorldId, const std::string& acPrivacy) noexcept;

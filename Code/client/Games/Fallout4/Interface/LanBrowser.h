#pragma once

// The servers on the local network, for the server browsers' LAN tab (the main menu's and the in-game overlay's): a
// broadcast every server answers (see Structs/LanDiscoveryProtocol.h).
namespace LanBrowser
{
// Starts a search in the background (ignored while one runs)
void StartScan() noexcept;

// The overlay event of a finished search: "lanServers" with a JSON array of
// {"address", "name", "players", "maxPlayers", "version", "password", "compatible"}. Sent from the game's main loop.
void SendResults() noexcept;
} // namespace LanBrowser

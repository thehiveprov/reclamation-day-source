#pragma once

#include <cstdint>

// Finding servers on the local network (the main menu's LAN tab): the game broadcasts kLanProbe to kLanDiscoveryPort,
// every server there answers the sender with kLanReplyPrefix followed by a JSON object:
// {"name", "port" (its game port), "players", "maxPlayers", "version", "password" (true if it needs one)}.
namespace LanDiscovery
{
// Not next to the game port: a second server on the same PC takes the game port + 1
constexpr uint16_t kLanDiscoveryPort = 10590;

constexpr char kLanProbe[] = "RDLAN?1";
constexpr char kLanReplyPrefix[] = "RDLAN!1";
} // namespace LanDiscovery

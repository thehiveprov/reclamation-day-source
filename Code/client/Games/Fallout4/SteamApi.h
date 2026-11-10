#pragma once

#include <functional>

class ISteamNetworkingSockets;
class ISteamNetworkingUtils;

// The game's own Steam library (Fallout 4's steam_api64.dll, the Steamworks SDK of 2015-16: SteamFriends015,
// SteamClient017, SteamUser018; started by the game for app 377160, the launcher writes steam_appid.txt) through its flat
// exports. A SteamID goes in and comes back as a plain 64-bit value (read in the DLL's code, 2026-10-25: the flat
// functions load the CSteamID the interface returned and pass the one they're given straight on). The Steam client's
// newer interfaces that SDK predates come from ISteamClient::GetISteamGenericInterface: SteamNetworkingSockets012 and
// SteamNetworkingUtils004, the versions of this build's GameNetworkingSockets 1.4.1 headers (the same classes, the
// same vtables), so TiltedConnect can drive Steam's peer to peer sockets. See docs/STEAM_SOCIAL.md. Main thread.
namespace SteamApi
{
// Steam's persona states (EPersonaState)
enum class Persona : int
{
    kOffline = 0,
    kOnline = 1,
    kBusy = 2,
    kAway = 3,
    kSnooze = 4,
    kLookingToTrade = 5,
    kLookingToPlay = 6,
    kInvisible = 7,
};

// Callbacks this uses (their k_iCallback): a friend's name, state or game changed (PersonaStateChange_t), a friend's
// rich presence changed (FriendRichPresenceUpdate_t), the player chose to join a friend from Steam (an accepted invite,
// the friends list's Join Game: GameRichPresenceJoinRequested_t), a friend's avatar finished loading
// (AvatarImageLoaded_t)
constexpr int kPersonaStateChange = 304;
constexpr int kAvatarImageLoaded = 334;
constexpr int kFriendRichPresenceUpdate = 336;
constexpr int kGameRichPresenceJoinRequested = 337;

// Fallout 4's Steam app (what friends playing it run)
constexpr uint32_t kFallout4AppId = 377160;

// Once the game has started Steam (its SteamFriends() is there): the exports and the newer interfaces looked up.
// Tried again every call until it works; false when Steam isn't running for this game.
bool Init() noexcept;
[[nodiscard]] bool IsReady() noexcept;

[[nodiscard]] uint64_t MySteamId() noexcept;
[[nodiscard]] std::string MyName() noexcept;

// Immediate friends (k_EFriendFlagImmediate)
[[nodiscard]] std::vector<uint64_t> Friends() noexcept;
[[nodiscard]] bool IsFriend(uint64_t aSteamId) noexcept;
[[nodiscard]] std::string FriendName(uint64_t aSteamId) noexcept;
[[nodiscard]] Persona FriendState(uint64_t aSteamId) noexcept;
// The Steam app the friend is playing (0: none)
[[nodiscard]] uint32_t FriendAppId(uint64_t aSteamId) noexcept;
[[nodiscard]] std::string FriendPresence(uint64_t aSteamId, const char* acpKey) noexcept;
void RequestFriendPresence(uint64_t aSteamId) noexcept;

// This player's rich presence (seen by friends; "connect" is what Steam's Join Game and invites pass on)
bool SetPresence(const char* acpKey, const char* acpValue) noexcept;
void ClearPresence() noexcept;
// Steam's own invite (a chat message with Accept), carrying a connect string
bool InviteToGame(uint64_t aSteamId, const std::string& acConnect) noexcept;
// Steam's overlay on a friend's profile ("steamid") or chat ("chat")
void OpenOverlayTo(const char* acpDialog, uint64_t aSteamId) noexcept;

// A friend's 64x64 avatar: Steam's image handle (0: none, -1: still loading), and its pixels (RGBA)
[[nodiscard]] int FriendAvatar(uint64_t aSteamId) noexcept;
bool ImageRgba(int aImage, uint32_t& aWidth, uint32_t& aHeight, std::vector<uint8_t>& aPixels) noexcept;

// The Steam client's peer to peer sockets and their utilities (null until Init worked)
[[nodiscard]] ISteamNetworkingSockets* Sockets() noexcept;
[[nodiscard]] ISteamNetworkingUtils* NetworkingUtils() noexcept;

// A Steam callback (by its id) handed to this function, with the callback's data; for as long as the game runs
void Listen(int aCallbackId, int aSize, std::function<void(const void*)> aHandler) noexcept;
} // namespace SteamApi

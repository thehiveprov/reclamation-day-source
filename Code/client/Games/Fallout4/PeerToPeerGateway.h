#pragma once

#include <steam/steamnetworkingtypes.h>

#include <chrono>
#include <functional>

class ISteamNetworkingSockets;

// The host's side of a friend joining peer to peer (docs/STEAM_SOCIAL.md). While this game hosts a world it listens on
// the Steam client's sockets (Client::kPeerToPeerVirtualPort): a Steam user asking in is let in, turned down
// (Client::kPeerToPeerRefused) or kept waiting for the host's answer, and one let in is passed on to this game's own
// server over 127.0.0.1, each message both ways as it was sent (reliable or not). The server sees them as anyone joining
// by address (the host is who has its key, not whoever comes through 127.0.0.1). Main thread.
class PeerToPeerGateway
{
public:
    enum class Decision
    {
        kAccept,
        kRefuse,
        kAsk,
    };

    PeerToPeerGateway() noexcept = default;
    ~PeerToPeerGateway() noexcept;

    TP_NOCOPYMOVE(PeerToPeerGateway);

    // Listens for Steam users joining the server on this port of 127.0.0.1, who's let in decided by aDecide; false
    // without the Steam client's sockets
    bool Start(uint16_t aServerPort, std::function<Decision(uint64_t)> aDecide) noexcept;
    // Everyone through it dropped, the listening stopped
    void Stop() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept { return m_listen != k_HSteamListenSocket_Invalid; }

    // Every frame: the connections' callbacks, the messages both ways, the asks that waited too long
    void Update() noexcept;

    // The host's answer to someone waiting
    void Answer(uint64_t aSteamId, bool aAccept) noexcept;
    // Everyone waiting turned down (the world went private, invite only)
    void RefuseWaiting() noexcept;

    [[nodiscard]] std::vector<uint64_t> Waiting() const noexcept;
    [[nodiscard]] std::vector<uint64_t> Joined() const noexcept;

private:
    struct Peer
    {
        HSteamNetConnection Steam{k_HSteamNetConnection_Invalid};
        HSteamNetConnection Local{k_HSteamNetConnection_Invalid};
        uint64_t SteamId{0};
        std::chrono::steady_clock::time_point Since{};
        bool Accepted{false};
        bool LocalReady{false};
        // what came from the friend before the server's side was connected: the data and its send flags
        std::vector<std::pair<std::vector<uint8_t>, int>> Queued;
    };

    // The two sockets' callbacks (each its own: their connection handles are counted apart and may be equal)
    static void SteamStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo);
    static void LocalStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo);
    void OnSteamStatus(const SteamNetConnectionStatusChangedCallback_t& acInfo) noexcept;
    void OnLocalStatus(const SteamNetConnectionStatusChangedCallback_t& acInfo) noexcept;

    void Accept(Peer& aPeer) noexcept;
    void OpenLocal(Peer& aPeer) noexcept;
    // Closes both sides and forgets it (aReason on the friend's side)
    void Drop(HSteamNetConnection aSteam, int aReason, const char* acpWhy) noexcept;
    void Pump() noexcept;

    [[nodiscard]] Peer* FindSteam(HSteamNetConnection aConnection) noexcept;
    [[nodiscard]] Peer* FindLocal(HSteamNetConnection aConnection) noexcept;

    ISteamNetworkingSockets* m_pSteam{nullptr};
    ISteamNetworkingSockets* m_pLocal{nullptr};
    HSteamListenSocket m_listen{k_HSteamListenSocket_Invalid};
    HSteamNetPollGroup m_steamGroup{k_HSteamNetPollGroup_Invalid};
    HSteamNetPollGroup m_localGroup{k_HSteamNetPollGroup_Invalid};
    uint16_t m_port{0};
    std::function<Decision(uint64_t)> m_decide;
    std::vector<Peer> m_peers;
};

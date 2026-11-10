#pragma once

#include <memory>
#include <string>
#include "steam/steamnetworkingsockets.h"
#include "SteamInterface.hpp"
#include "SynchronizedClock.hpp"

namespace TiltedPhoques
{
    struct Packet;
    struct Client
    {
        enum EDisconnectReason
        {
            kTimeout,
            kLocalProblem,
            kKicked,
            kCannotResolve,
            kAborted,
            kNormal,
            // a peer to peer join the host's game turned down (its privacy, or the host refused it)
            kRefused
        };

        // Peer to peer over Steam's own network (a friend's game, through Valve's relays: no port to open): an address
        // "steam:<SteamID64>" connects to that Steam user's game on this virtual port, given this long (ms) to accept
        // (the host may be asked first); the host's game closes a join it turns down with this reason
        static constexpr const char* kPeerToPeerPrefix = "steam:";
        static constexpr int kPeerToPeerVirtualPort = 25681;
        static constexpr int kPeerToPeerTimeoutMs = 60000;
        static constexpr int kPeerToPeerRefused = k_ESteamNetConnectionEnd_App_Min + 1;

        struct Statistics
        {
            uint32_t SentBytes{};
            uint32_t RecvBytes{};
            uint32_t UncompressedSentBytes{};
            uint32_t UncompressedRecvBytes{};
        };

        Client() noexcept;
        virtual ~Client();

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        Client(Client&&) noexcept;
        Client& operator=(Client&&) noexcept;

        bool Connect(const SteamNetworkingIPAddr& acEndpoint) noexcept;
        bool Connect(const std::string& acEndpoint) noexcept;
        bool ConnectByIp(const std::string& acEndpoint) noexcept;
        bool ConnectPeerToPeer(uint64_t aSteamId) noexcept;
        void Close() noexcept;

        // The Steam client's own sockets (its ISteamNetworkingSockets012, the same interface as this library's), for
        // the "steam:" addresses; none: those can't connect
        static void SetPeerToPeerInterface(ISteamNetworkingSockets* apSteam) noexcept;
        [[nodiscard]] static ISteamNetworkingSockets* GetPeerToPeerInterface() noexcept;

        void Update() noexcept;

        virtual void OnConsume(const void* apData, uint32_t aSize) = 0;
        virtual void OnConnected() = 0;
        virtual void OnDisconnected(EDisconnectReason aReason) = 0;
        virtual void OnUpdate() = 0;

        void Send(Packet* apPacket, EPacketFlags acPacketFlags = kReliable) const noexcept;

        [[nodiscard]] bool IsConnected() const noexcept;
        [[nodiscard]] SteamNetConnectionRealTimeStatus_t GetConnectionStatus() const noexcept;
        [[nodiscard]] Statistics GetStatistics() const noexcept;
        [[nodiscard]] const SynchronizedClock& GetClock() const noexcept;

    private:

        struct Lookup;

        static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* apInfo);
        void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo);

        void HandleMessage(const void* apData, uint32_t aSize) noexcept;
        void HandleServerTime(const void* apData, uint32_t aSize) noexcept;
        void HandleCompressedPayload(const void* apData, uint32_t aSize) noexcept;

        HSteamNetConnection m_connection;
        ISteamNetworkingSockets* m_pInterface;
        SynchronizedClock m_clock;
        uint64_t m_lastStatisticsPoint{};
        mutable Statistics m_currentFrame{};
        Statistics m_previousFrame{};
        std::shared_ptr<Lookup> m_pLookup;
    };
}

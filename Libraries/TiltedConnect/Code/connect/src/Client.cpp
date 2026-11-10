#include <chrono>
#include "Client.hpp"
#include "SteamInterface.hpp"
#include <steam/isteamnetworkingutils.h>
#include <cassert>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Allocator.hpp>
#include "Packet.hpp"
#include <google/protobuf/stubs/port.h>
#include <snappy.h>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace TiltedPhoques
{
    static thread_local Client* s_pClient = nullptr;
    // The Steam client's sockets for "steam:" addresses
    static ISteamNetworkingSockets* s_pPeerToPeer = nullptr;
    // Every client there is: a connection's callback can run outside its client's Update (Steam's own dispatch, the
    // game's SteamAPI_RunCallbacks; another user of the same sockets running their callbacks), and goes to the client
    // with that connection
    static std::vector<Client*> s_clients;

    // Used when the endpoint names no port
    static constexpr uint16_t kDefaultPort = 25681;

    // A host name looked up on a thread of its own, picked up by Update
    struct Client::Lookup
    {
        std::atomic<bool> Done{false};
        bool Valid{false};
        SteamNetworkingIPAddr Address{};
    };

    Client::Client() noexcept
    {
        SteamInterface::Acquire();

        m_connection = k_HSteamNetConnection_Invalid;
        m_pInterface = SteamNetworkingSockets();
        s_clients.push_back(this);
    }

    Client::~Client()
    {
        std::erase(s_clients, this);

        SteamInterface::Release();
    }

    Client::Client(Client&& aRhs) noexcept
        : m_connection(k_HSteamNetConnection_Invalid)
        , m_pInterface(nullptr)
    {
        SteamInterface::Acquire();
        s_clients.push_back(this);

        this->operator=(std::move(aRhs));
    }

    Client& Client::operator=(Client&& aRhs) noexcept
    {
        std::swap(m_connection, aRhs.m_connection);
        std::swap(m_pInterface, aRhs.m_pInterface);

        return *this;
    }

    void Client::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* apInfo)
    {
        if (!apInfo || apInfo->m_hConn == k_HSteamNetConnection_Invalid) [[unlikely]]
            return;

        if (s_pClient && s_pClient->m_connection == apInfo->m_hConn) [[likely]]
        {
            s_pClient->OnSteamNetConnectionStatusChanged(apInfo);
            return;
        }

        for (Client* pClient : s_clients)
        {
            if (pClient->m_connection == apInfo->m_hConn)
            {
                pClient->OnSteamNetConnectionStatusChanged(apInfo);
                return;
            }
        }
    }

    void Client::SetPeerToPeerInterface(ISteamNetworkingSockets* apSteam) noexcept
    {
        s_pPeerToPeer = apSteam;
    }

    ISteamNetworkingSockets* Client::GetPeerToPeerInterface() noexcept
    {
        return s_pPeerToPeer;
    }

    bool Client::Connect(const std::string& acEndpoint) noexcept
    {
        // Joining again while a connection or a lookup is still there replaces it (left open, the server got both)
        Close();

        // A Steam user's game, peer to peer
        if (acEndpoint.rfind(kPeerToPeerPrefix, 0) == 0)
        {
            const char* pId = acEndpoint.c_str() + std::char_traits<char>::length(kPeerToPeerPrefix);
            char* pEnd = nullptr;
            const uint64_t steamId = std::strtoull(pId, &pEnd, 10);
            if (steamId == 0 || !pEnd || *pEnd != '\0')
            {
                OnDisconnected(kCannotResolve);
                return false;
            }
            return ConnectPeerToPeer(steamId);
        }

        // An address needs no lookup
        SteamNetworkingIPAddr address{};
        if (address.ParseString(acEndpoint.c_str()))
        {
            if (address.m_port == 0)
                address.m_port = kDefaultPort;

            if (Connect(address))
                return true;

            OnDisconnected(kLocalProblem);
            return false;
        }

        const auto pos = acEndpoint.find_last_of(':');

        std::string endpoint = acEndpoint;
        std::string serviceName = std::to_string(kDefaultPort);
        if(pos != std::string::npos)
        {
            serviceName = acEndpoint.c_str() + 1 + pos;
            endpoint = endpoint.substr(0, pos);
        }

        // On a thread of its own: in the game, libuv's lookups (run on its thread pool, answered through its loop)
        // came back tens of seconds late or never once the player had left a server
        auto pLookup = std::make_shared<Lookup>();
        try
        {
            std::thread(
                [pLookup, endpoint = std::move(endpoint), serviceName = std::move(serviceName)]()
                {
                    addrinfo* pResult = nullptr;
                    if (getaddrinfo(endpoint.c_str(), serviceName.c_str(), nullptr, &pResult) == 0 && pResult)
                    {
                        switch (pResult->ai_family)
                        {
                        case AF_INET:
                        {
                            const auto port = ntohs(reinterpret_cast<sockaddr_in*>(pResult->ai_addr)->sin_port);
                            pLookup->Address.SetIPv4(ntohl(reinterpret_cast<sockaddr_in*>(pResult->ai_addr)->sin_addr.s_addr), port);
                            pLookup->Valid = true;
                        } break;
                        case AF_INET6:
                        {
                            const auto port = ntohs(reinterpret_cast<sockaddr_in6*>(pResult->ai_addr)->sin6_port);
                            pLookup->Address.SetIPv6(reinterpret_cast<sockaddr_in6*>(pResult->ai_addr)->sin6_addr.s6_addr, port);
                            pLookup->Valid = true;
                        } break;
                        }
                    }

                    if (pResult)
                        freeaddrinfo(pResult);

                    pLookup->Done.store(true, std::memory_order_release);
                })
                .detach();
        }
        catch (...)
        {
            OnDisconnected(kLocalProblem);
            return false;
        }

        m_pLookup = std::move(pLookup);

        return true;
    }

    bool Client::Connect(const SteamNetworkingIPAddr& acEndpoint) noexcept
    {
        if (m_connection != k_HSteamNetConnection_Invalid || m_pLookup)
            Close();

        // (this library's sockets: a peer to peer connection before may have left Steam's in place)
        m_pInterface = SteamNetworkingSockets();

        SteamNetworkingConfigValue_t opt = {};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&SteamNetConnectionStatusChangedCallback));
        m_connection = m_pInterface->ConnectByIPAddress(acEndpoint, 1, &opt);

        return m_connection != k_HSteamNetConnection_Invalid;
    }

    bool Client::ConnectPeerToPeer(uint64_t aSteamId) noexcept
    {
        if (m_connection != k_HSteamNetConnection_Invalid || m_pLookup)
            Close();

        if (!s_pPeerToPeer)
        {
            OnDisconnected(kLocalProblem);
            return false;
        }

        m_pInterface = s_pPeerToPeer;

        SteamNetworkingIdentity identity{};
        identity.SetSteamID64(aSteamId);
        SteamNetworkingConfigValue_t opts[2] = {};
        opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&SteamNetConnectionStatusChangedCallback));
        opts[1].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, kPeerToPeerTimeoutMs);
        m_connection = m_pInterface->ConnectP2P(identity, kPeerToPeerVirtualPort, 2, opts);

        if (m_connection == k_HSteamNetConnection_Invalid)
        {
            OnDisconnected(kLocalProblem);
            return false;
        }
        return true;
    }

    bool Client::ConnectByIp(const std::string& acEndpoint) noexcept
    {
        SteamNetworkingIPAddr remoteAddress{};
        remoteAddress.ParseString(acEndpoint.c_str());

        return Connect(remoteAddress);
    }

    void Client::Close() noexcept
    {
        if (m_connection == k_HSteamNetConnection_Invalid && !m_pLookup)
            return;

        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->CloseConnection(m_connection, 0, nullptr, true);
            m_connection = k_HSteamNetConnection_Invalid;

            m_clock.Reset();
        }

        // A lookup still running finishes on its thread, its answer unused
        m_pLookup.reset();

        OnDisconnected(kAborted);
    }

    void Client::Update() noexcept
    {
        m_clock.Update();

        if (m_clock.GetCurrentTick() - m_lastStatisticsPoint >= 1000)
        {
            m_lastStatisticsPoint = m_clock.GetCurrentTick();
            m_previousFrame = m_currentFrame;
            m_currentFrame = {};
        }

        s_pClient = this;
        m_pInterface->RunCallbacks();
        s_pClient = nullptr;

        if (m_pLookup && m_pLookup->Done.load(std::memory_order_acquire))
        {
            const auto pLookup = std::move(m_pLookup);

            if (!pLookup->Valid)
                OnDisconnected(kCannotResolve);
            else if (!Connect(pLookup->Address))
                OnDisconnected(kLocalProblem);
        }

        while (true)
        {
            ISteamNetworkingMessage* pIncomingMsg = nullptr;
            const auto cMessageCount = m_pInterface->ReceiveMessagesOnConnection(m_connection, &pIncomingMsg, 1);
            if (cMessageCount <= 0 || pIncomingMsg == nullptr)
            {
                // TODO: Handle error when messageCount < 0
                break;
            }

            m_currentFrame.RecvBytes += pIncomingMsg->GetSize();
            m_currentFrame.UncompressedRecvBytes += pIncomingMsg->GetSize();

            HandleMessage(pIncomingMsg->GetData(), pIncomingMsg->GetSize());

            pIncomingMsg->Release();
        }

        OnUpdate();
    }

    void Client::Send(Packet* apPacket, const EPacketFlags acPacketFlags) const noexcept
    {
        m_currentFrame.UncompressedSentBytes += apPacket->m_size;

        if (apPacket->m_pData[0] == kPayload)
        {
            std::string data;
            snappy::Compress(apPacket->GetData(), apPacket->GetSize(), &data);

            if (data.size() < apPacket->GetSize())
            {
                apPacket->m_pData[0] = kCompressedPayload;
                std::copy(std::begin(data), std::end(data), apPacket->GetData());
                apPacket->m_size = (data.size() + 1) & 0xFFFFFFFF;
            }
        }

        m_currentFrame.SentBytes += apPacket->m_size;

        m_pInterface->SendMessageToConnection(m_connection, apPacket->m_pData, apPacket->m_size,
            acPacketFlags == kReliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable, nullptr);
    }

    bool Client::IsConnected() const noexcept
    {
        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            SteamNetConnectionInfo_t info{};
            if (m_pInterface->GetConnectionInfo(m_connection, &info))
            {
                if(info.m_eState == k_ESteamNetworkingConnectionState_Connected)
                {
                    return GetClock().IsSynchronized();
                }
            }
        }

        return false;
    }

    SteamNetConnectionRealTimeStatus_t Client::GetConnectionStatus() const noexcept
    {
        SteamNetConnectionRealTimeStatus_t status{};

        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->GetConnectionRealTimeStatus(m_connection, &status, 0, nullptr);
        }

        return status;
    }

    Client::Statistics Client::GetStatistics() const noexcept
    {
        return m_previousFrame;
    }

    const SynchronizedClock& Client::GetClock() const noexcept
    {
        return m_clock;
    }

    void Client::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo)
    {
        // A change queued for a connection left or replaced since: closing "it" would close the one there is now
        if (apInfo->m_hConn != m_connection)
            return;

        switch (apInfo->m_info.m_eState)
        {
        case k_ESteamNetworkingConnectionState_None:
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            m_pInterface->CloseConnection(m_connection, 0, nullptr, false);
            m_connection = k_HSteamNetConnection_Invalid;

            m_clock.Reset();

            if (apInfo->m_info.m_eEndReason == kPeerToPeerRefused)
            {
                OnDisconnected(kRefused);
            }
            else if (apInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting)
            {
                OnDisconnected(kTimeout);
            }
            else if (apInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
            {
                OnDisconnected(kLocalProblem);
            }
            else
            {
                OnDisconnected(kKicked);
            }

            break;
        }
        case k_ESteamNetworkingConnectionState_Connecting:
            break;
        case k_ESteamNetworkingConnectionState_Connected:
            // We don't notify here, wait for clock sync
            break;
        default:
            break;
        }
    }

    void Client::HandleMessage(const void* apData, uint32_t aSize) noexcept
    {
        // We handle the cases where packets target the current stack or the user stack
        if (aSize == 0)
            return;

        auto pData = static_cast<const uint8_t*>(apData);

        const auto cOpcode = pData[0];

        pData += 1;
        aSize -= 1;

        switch (cOpcode)
        {
        case kPayload:
            OnConsume(pData, aSize);
            break;
        case kServerTime:
            HandleServerTime(pData, aSize);
            break;
        case kCompressedPayload:
            HandleCompressedPayload(pData, aSize);
            break;
        default:
            assert(false);
            break;
        }
    }

    void Client::HandleServerTime(const void* apData, uint32_t aSize) noexcept
    {
        if (aSize < 8)
            return;

        const auto cConnectionStatus = GetConnectionStatus();

        const auto cServerTime = google::protobuf::BigEndian::Load64(apData);
        const auto cWasSynchronized = GetClock().IsSynchronized();

        m_clock.Synchronize(cServerTime, cConnectionStatus.m_nPing);

        if (!cWasSynchronized)
            OnConnected();
    }

    void Client::HandleCompressedPayload(const void* apData, uint32_t aSize) noexcept
    {
        std::string data;
        snappy::Uncompress((const char*)apData, aSize, &data);

        m_currentFrame.UncompressedRecvBytes -= aSize;
        m_currentFrame.UncompressedRecvBytes += data.size() & 0xFFFFFFFF;

        if (!data.empty()) [[likely]]
        {
            OnConsume(data.data(), data.size() & 0xFFFFFFFF);
        }
    }
}

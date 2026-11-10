#include <Game/LanResponder.h>

#include <Structs/LanDiscoveryProtocol.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

LanResponder::LanResponder() noexcept
    : m_socket(INVALID_SOCKET)
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return;

    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return;

    // Several servers on one PC all answer
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(LanDiscovery::kLanDiscoveryPort);

    u_long nonBlocking = 1;
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ioctlsocket(s, FIONBIO, &nonBlocking) != 0)
    {
        spdlog::warn("LAN: games on the local network won't find this server (port {} unavailable, error {})", LanDiscovery::kLanDiscoveryPort,
                     WSAGetLastError());
        closesocket(s);
        return;
    }

    m_socket = s;
    spdlog::info("LAN: games on the local network can find this server (port {})", LanDiscovery::kLanDiscoveryPort);
}

LanResponder::~LanResponder() noexcept
{
    if (m_socket != INVALID_SOCKET)
        closesocket(static_cast<SOCKET>(m_socket));
    WSACleanup();
}

bool LanResponder::HasProbe() noexcept
{
    if (m_socket == INVALID_SOCKET)
        return false;

    static_assert(sizeof(m_sender) >= sizeof(sockaddr_in));
    for (;;)
    {
        char buffer[64];
        m_senderSize = sizeof(m_sender);
        const int read = recvfrom(static_cast<SOCKET>(m_socket), buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(m_sender), &m_senderSize);
        if (read == SOCKET_ERROR)
        {
            // A game that left before our answer reached it (WSAECONNRESET): the next one. Nothing waiting: done.
            if (WSAGetLastError() == WSAECONNRESET)
                continue;
            return false;
        }

        if (static_cast<size_t>(read) == sizeof(LanDiscovery::kLanProbe) - 1 && std::memcmp(buffer, LanDiscovery::kLanProbe, read) == 0)
            return true;
    }
}

void LanResponder::Reply(const String& acDescription) noexcept
{
    const String reply = String(LanDiscovery::kLanReplyPrefix) + acDescription;
    sendto(static_cast<SOCKET>(m_socket), reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<const sockaddr*>(m_sender), m_senderSize);
}

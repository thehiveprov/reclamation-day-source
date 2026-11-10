#include <HivemindPCH.h>

#include <Interface/LanBrowser.h>

#include <Structs/LanDiscoveryProtocol.h>
#include <Services/OverlayService.h>
#include <MiniJson.h>
#include <World.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <mutex>
#include <thread>

namespace LanBrowser
{
namespace
{
constexpr auto kAnswerTime = std::chrono::milliseconds(1500);

std::atomic<bool> s_scanning{false};
std::mutex s_lock;
std::optional<std::string> s_results; // a finished search, not sent yet

// A number field of a flat JSON object, 0 if absent
uint64_t GetNumber(std::string_view aJson, std::string_view aKey) noexcept
{
    const std::string pattern = MiniJson::Quote(aKey) + ":";
    const auto pos = aJson.find(pattern);
    return pos == std::string_view::npos ? 0 : std::strtoull(std::string(aJson.substr(pos + pattern.size(), 20)).c_str(), nullptr, 10);
}

// Where the probe goes: this PC, and the broadcast address of every network it's on (255.255.255.255 alone often
// only leaves through one of them)
std::vector<in_addr> GetProbeTargets() noexcept
{
    std::vector<in_addr> targets;
    in_addr address{};
    inet_pton(AF_INET, "127.0.0.1", &address);
    targets.push_back(address);
    address.s_addr = INADDR_BROADCAST;
    targets.push_back(address);

    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* pAdapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, pAdapters, &size) == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        pAdapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, pAdapters, &size) != NO_ERROR)
            return targets;
    }

    for (auto* pAdapter = pAdapters; pAdapter; pAdapter = pAdapter->Next)
    {
        if (pAdapter->OperStatus != IfOperStatusUp || pAdapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        for (auto* pUnicast = pAdapter->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next)
        {
            if (pUnicast->Address.lpSockaddr->sa_family != AF_INET || pUnicast->OnLinkPrefixLength == 0 || pUnicast->OnLinkPrefixLength > 30)
                continue;

            const auto* pAddress = reinterpret_cast<const sockaddr_in*>(pUnicast->Address.lpSockaddr);
            const uint32_t mask = pUnicast->OnLinkPrefixLength >= 32 ? 0xFFFFFFFF : ~(0xFFFFFFFFu >> pUnicast->OnLinkPrefixLength);
            in_addr broadcast{};
            broadcast.s_addr = htonl((ntohl(pAddress->sin_addr.s_addr) & mask) | ~mask);
            targets.push_back(broadcast);
        }
    }
    return targets;
}

struct Found
{
    std::string address; // host:port to join
    std::string description;
    bool loopback;
};

std::string Scan() noexcept
{
    std::vector<Found> servers;

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return "[]";

    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET)
    {
        BOOL broadcast = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));

        for (const in_addr& target : GetProbeTargets())
        {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(LanDiscovery::kLanDiscoveryPort);
            to.sin_addr = target;
            sendto(s, LanDiscovery::kLanProbe, static_cast<int>(sizeof(LanDiscovery::kLanProbe) - 1), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }

        const auto deadline = std::chrono::steady_clock::now() + kAnswerTime;
        for (;;)
        {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0)
                break;

            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(s, &readable);
            timeval timeout{static_cast<long>(left / 1000000), static_cast<long>(left % 1000000)};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
                break;

            char buffer[1024];
            sockaddr_in from{};
            int fromSize = sizeof(from);
            const int read = recvfrom(s, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
            if (read <= 0)
                continue;

            const std::string_view reply(buffer, static_cast<size_t>(read));
            const std::string_view prefix(LanDiscovery::kLanReplyPrefix);
            if (!reply.starts_with(prefix) || reply.size() < prefix.size() + 2 || reply[prefix.size()] != '{')
                continue;

            char host[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
            const std::string description(reply.substr(prefix.size()));
            const auto port = GetNumber(description, "port");
            if (port == 0 || port > 65535)
                continue;

            servers.push_back({fmt::format("{}:{}", host, port), description, std::string_view(host).starts_with("127.")});
        }
        closesocket(s);
    }
    WSACleanup();

    // A server on this PC answers on every network it's on: listed once, with the address others would use
    std::vector<Found> unique;
    for (const auto& server : servers)
    {
        const auto same = std::ranges::find_if(unique, [&server](const Found& acOther) { return acOther.description == server.description; });
        if (same == unique.end())
            unique.push_back(server);
        else if (same->loopback && !server.loopback)
            *same = server;
    }

    std::string json;
    for (const auto& server : unique)
    {
        const bool compatible = MiniJson::GetString(server.description, "version") == BUILD_COMMIT;
        json += fmt::format(R"({}{{"address":{},"compatible":{},{})", json.empty() ? "" : ",", MiniJson::Quote(server.address), compatible ? "true" : "false",
                            server.description.substr(1));
    }
    spdlog::info("LAN: found {} server(s)", unique.size());
    return "[" + json + "]";
}
} // namespace

void StartScan() noexcept
{
    if (s_scanning.exchange(true))
        return;

    std::thread(
        []
        {
            std::string results = Scan();
            {
                std::scoped_lock lock(s_lock);
                s_results = std::move(results);
            }
            s_scanning = false;
        })
        .detach();
}

void SendResults() noexcept
{
    std::optional<std::string> results;
    {
        std::scoped_lock lock(s_lock);
        results.swap(s_results);
    }
    if (results)
        World::Get().ctx().at<OverlayService>().SendEvent("lanServers", *results);
}
} // namespace LanBrowser

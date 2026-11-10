#include <Games/Fallout4/PeerToPeerGateway.h>

#include <Games/Fallout4/SteamApi.h>

#include <Client.hpp>
#include <steam/steamnetworkingsockets.h>

namespace
{
using TiltedPhoques::Client;

PeerToPeerGateway* s_pGateway = nullptr;

// Someone waiting for the host's answer is turned down after this long, before their own wait (60 s) runs out
constexpr auto kAskTimeout = std::chrono::seconds(50);
// The friend's side closed: the host stopped hosting, the server closed its side
constexpr int kHostLeft = k_ESteamNetConnectionEnd_App_Min + 2;
constexpr int kServerClosed = k_ESteamNetConnectionEnd_App_Min + 3;
// Messages taken from each side per call
constexpr int kBatch = 64;

// A message passed on as it was sent: reliable or not, at once (the relay adds no wait of its own)
int ForwardFlags(const SteamNetworkingMessage_t& acMessage) noexcept
{
    return (acMessage.m_nFlags & k_nSteamNetworkingSend_Reliable) ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle;
}
} // namespace

PeerToPeerGateway::~PeerToPeerGateway() noexcept
{
    Stop();
}

bool PeerToPeerGateway::Start(uint16_t aServerPort, std::function<Decision(uint64_t)> aDecide) noexcept
{
    Stop();

    m_pSteam = SteamApi::Sockets();
    m_pLocal = SteamNetworkingSockets();
    if (!m_pSteam || !m_pLocal)
        return false;

    SteamNetworkingConfigValue_t option{};
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&SteamStatusChanged));
    m_listen = m_pSteam->CreateListenSocketP2P(Client::kPeerToPeerVirtualPort, 1, &option);
    if (m_listen == k_HSteamListenSocket_Invalid)
    {
        spdlog::error("Steam: couldn't listen for friends joining");
        return false;
    }
    m_steamGroup = m_pSteam->CreatePollGroup();
    m_localGroup = m_pLocal->CreatePollGroup();
    m_port = aServerPort;
    m_decide = std::move(aDecide);
    s_pGateway = this;
    spdlog::info("Steam: friends can ask to join, passed on to the server on port {}", aServerPort);
    return true;
}

void PeerToPeerGateway::Stop() noexcept
{
    if (!IsRunning())
        return;

    while (!m_peers.empty())
        Drop(m_peers.back().Steam, kHostLeft, "Host left");
    m_pSteam->CloseListenSocket(m_listen);
    m_pSteam->DestroyPollGroup(m_steamGroup);
    m_pLocal->DestroyPollGroup(m_localGroup);
    m_listen = k_HSteamListenSocket_Invalid;
    m_steamGroup = m_localGroup = k_HSteamNetPollGroup_Invalid;
    m_decide = nullptr;
    if (s_pGateway == this)
        s_pGateway = nullptr;
    spdlog::info("Steam: no longer letting friends join");
}

void PeerToPeerGateway::Update() noexcept
{
    if (!IsRunning())
        return;

    // the connections' changes (a friend asking in, connected, gone; the server's side connected, closed)
    m_pSteam->RunCallbacks();
    m_pLocal->RunCallbacks();

    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < m_peers.size();)
    {
        const Peer& peer = m_peers[i];
        if (!peer.Accepted && now - peer.Since > kAskTimeout)
        {
            spdlog::info("Steam: {} waited too long for an answer, turned down", peer.SteamId);
            Drop(peer.Steam, Client::kPeerToPeerRefused, "No answer");
            continue;
        }
        ++i;
    }

    Pump();
}

void PeerToPeerGateway::Answer(uint64_t aSteamId, bool aAccept) noexcept
{
    for (Peer& peer : m_peers)
    {
        if (peer.SteamId != aSteamId || peer.Accepted)
            continue;
        if (aAccept)
        {
            spdlog::info("Steam: the host let {} in", aSteamId);
            Accept(peer);
        }
        else
        {
            spdlog::info("Steam: the host turned {} down", aSteamId);
            Drop(peer.Steam, Client::kPeerToPeerRefused, "Refused");
        }
        return;
    }
}

void PeerToPeerGateway::RefuseWaiting() noexcept
{
    for (size_t i = 0; i < m_peers.size();)
    {
        if (!m_peers[i].Accepted)
        {
            Drop(m_peers[i].Steam, Client::kPeerToPeerRefused, "Refused");
            continue;
        }
        ++i;
    }
}

std::vector<uint64_t> PeerToPeerGateway::Waiting() const noexcept
{
    std::vector<uint64_t> ids;
    for (const Peer& peer : m_peers)
    {
        if (!peer.Accepted)
            ids.push_back(peer.SteamId);
    }
    return ids;
}

std::vector<uint64_t> PeerToPeerGateway::Joined() const noexcept
{
    std::vector<uint64_t> ids;
    for (const Peer& peer : m_peers)
    {
        if (peer.Accepted)
            ids.push_back(peer.SteamId);
    }
    return ids;
}

void PeerToPeerGateway::SteamStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo)
{
    if (s_pGateway && apInfo)
        s_pGateway->OnSteamStatus(*apInfo);
}

void PeerToPeerGateway::LocalStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo)
{
    if (s_pGateway && apInfo)
        s_pGateway->OnLocalStatus(*apInfo);
}

void PeerToPeerGateway::OnSteamStatus(const SteamNetConnectionStatusChangedCallback_t& acInfo) noexcept
{
    const HSteamNetConnection connection = acInfo.m_hConn;
    switch (acInfo.m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
    {
        // someone asking in on our listen socket
        if (acInfo.m_info.m_hListenSocket != m_listen || FindSteam(connection))
            return;
        const uint64_t steamId = acInfo.m_info.m_identityRemote.GetSteamID64();
        const Decision decision = steamId && m_decide ? m_decide(steamId) : Decision::kRefuse;
        if (decision == Decision::kRefuse)
        {
            spdlog::info("Steam: {} asked to join, turned down", steamId);
            m_pSteam->CloseConnection(connection, Client::kPeerToPeerRefused, "Refused", false);
            return;
        }
        Peer peer;
        peer.Steam = connection;
        peer.SteamId = steamId;
        peer.Since = std::chrono::steady_clock::now();
        m_peers.push_back(std::move(peer));
        if (decision == Decision::kAccept)
        {
            spdlog::info("Steam: {} asked to join, let in", steamId);
            Accept(m_peers.back());
        }
        else
            spdlog::info("Steam: {} asked to join, waiting for the host", steamId);
        break;
    }
    case k_ESteamNetworkingConnectionState_Connected:
        if (Peer* pPeer = FindSteam(connection); pPeer && pPeer->Local == k_HSteamNetConnection_Invalid)
            OpenLocal(*pPeer);
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        if (const Peer* pPeer = FindSteam(connection))
        {
            spdlog::info("Steam: {} left ({})", pPeer->SteamId, acInfo.m_info.m_szEndDebug);
            Drop(connection, 0, nullptr);
        }
        else
            m_pSteam->CloseConnection(connection, 0, nullptr, false);
        break;
    default: break;
    }
}

void PeerToPeerGateway::OnLocalStatus(const SteamNetConnectionStatusChangedCallback_t& acInfo) noexcept
{
    Peer* pPeer = FindLocal(acInfo.m_hConn);
    if (!pPeer)
        return;
    switch (acInfo.m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connected:
        pPeer->LocalReady = true;
        for (const auto& [data, flags] : pPeer->Queued)
            m_pLocal->SendMessageToConnection(pPeer->Local, data.data(), static_cast<uint32_t>(data.size()), flags, nullptr);
        pPeer->Queued.clear();
        spdlog::info("Steam: {} is through to the server", pPeer->SteamId);
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        spdlog::info("Steam: the server closed {}'s side ({})", pPeer->SteamId, acInfo.m_info.m_szEndDebug);
        Drop(pPeer->Steam, kServerClosed, "Server closed");
        break;
    default: break;
    }
}

void PeerToPeerGateway::Accept(Peer& aPeer) noexcept
{
    if (m_pSteam->AcceptConnection(aPeer.Steam) != k_EResultOK)
    {
        spdlog::warn("Steam: couldn't take {}'s connection", aPeer.SteamId);
        Drop(aPeer.Steam, 0, nullptr);
        return;
    }
    aPeer.Accepted = true;
    m_pSteam->SetConnectionPollGroup(aPeer.Steam, m_steamGroup);
}

void PeerToPeerGateway::OpenLocal(Peer& aPeer) noexcept
{
    SteamNetworkingIPAddr address{};
    address.SetIPv4(0x7F000001, m_port);
    SteamNetworkingConfigValue_t option{};
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&LocalStatusChanged));
    aPeer.Local = m_pLocal->ConnectByIPAddress(address, 1, &option);
    if (aPeer.Local == k_HSteamNetConnection_Invalid)
    {
        spdlog::error("Steam: couldn't reach the server for {}", aPeer.SteamId);
        Drop(aPeer.Steam, kServerClosed, "Server unreachable");
        return;
    }
    m_pLocal->SetConnectionPollGroup(aPeer.Local, m_localGroup);
}

void PeerToPeerGateway::Drop(HSteamNetConnection aSteam, int aReason, const char* acpWhy) noexcept
{
    const auto it = std::find_if(m_peers.begin(), m_peers.end(), [aSteam](const Peer& acPeer) { return acPeer.Steam == aSteam; });
    if (it == m_peers.end())
        return;
    m_pSteam->CloseConnection(it->Steam, aReason, acpWhy, aReason == kHostLeft || aReason == kServerClosed);
    if (it->Local != k_HSteamNetConnection_Invalid)
        m_pLocal->CloseConnection(it->Local, 0, "Left", true);
    m_peers.erase(it);
}

void PeerToPeerGateway::Pump() noexcept
{
    SteamNetworkingMessage_t* messages[kBatch];

    // the friends' messages to the server (held back for one whose server side isn't connected yet)
    for (int count; (count = m_pSteam->ReceiveMessagesOnPollGroup(m_steamGroup, messages, kBatch)) > 0;)
    {
        for (int i = 0; i < count; ++i)
        {
            SteamNetworkingMessage_t* pMessage = messages[i];
            if (Peer* pPeer = FindSteam(pMessage->m_conn))
            {
                const int flags = ForwardFlags(*pMessage);
                if (pPeer->LocalReady)
                    m_pLocal->SendMessageToConnection(pPeer->Local, pMessage->m_pData, pMessage->m_cbSize, flags, nullptr);
                else
                {
                    const auto* pData = static_cast<const uint8_t*>(pMessage->m_pData);
                    pPeer->Queued.emplace_back(std::vector<uint8_t>(pData, pData + pMessage->m_cbSize), flags);
                }
            }
            pMessage->Release();
        }
        if (count < kBatch)
            break;
    }

    // the server's messages to them
    for (int count; (count = m_pLocal->ReceiveMessagesOnPollGroup(m_localGroup, messages, kBatch)) > 0;)
    {
        for (int i = 0; i < count; ++i)
        {
            SteamNetworkingMessage_t* pMessage = messages[i];
            if (const Peer* pPeer = FindLocal(pMessage->m_conn))
                m_pSteam->SendMessageToConnection(pPeer->Steam, pMessage->m_pData, pMessage->m_cbSize, ForwardFlags(*pMessage), nullptr);
            pMessage->Release();
        }
        if (count < kBatch)
            break;
    }
}

PeerToPeerGateway::Peer* PeerToPeerGateway::FindSteam(HSteamNetConnection aConnection) noexcept
{
    const auto it = std::find_if(m_peers.begin(), m_peers.end(), [aConnection](const Peer& acPeer) { return acPeer.Steam == aConnection; });
    return it != m_peers.end() ? &*it : nullptr;
}

PeerToPeerGateway::Peer* PeerToPeerGateway::FindLocal(HSteamNetConnection aConnection) noexcept
{
    const auto it = std::find_if(m_peers.begin(), m_peers.end(), [aConnection](const Peer& acPeer) { return acPeer.Local == aConnection; });
    return it != m_peers.end() ? &*it : nullptr;
}

#include <Games/Fallout4/SteamApi.h>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace SteamApi
{
namespace
{
// k_EFriendFlagImmediate: a regular friend
constexpr int kImmediateFriends = 0x04;

// FriendGameInfo_t (pack 8): the game played (CGameID: the app id in its low 24 bits), its server's address, the lobby
struct FriendGameInfo
{
    uint64_t GameId;
    uint32_t GameIp;
    uint16_t GamePort;
    uint16_t QueryPort;
    uint64_t LobbyId;
};
static_assert(sizeof(FriendGameInfo) == 24);

// The SDK's CCallbackBase as Steam's DLL calls it (declared as the SDK declares it, so MSVC lays out the same vtable:
// Run, Run with the call result, GetCallbackSizeBytes; the flags at 8, the callback's id at 12)
class CCallbackBase
{
public:
    virtual void Run(void* apParam) = 0;
    virtual void Run(void* apParam, bool aIoFailure, uint64_t aCall) = 0;
    virtual int GetCallbackSizeBytes() = 0;

protected:
    uint8_t m_nCallbackFlags{0};
    int m_iCallback{0};
};

class Callback final : public CCallbackBase
{
public:
    Callback(int aId, int aSize, std::function<void(const void*)> aHandler) noexcept
        : m_size(aSize)
        , m_handler(std::move(aHandler))
    {
        m_iCallback = aId;
    }

    void Run(void* apParam) override
    {
        if (m_handler && apParam)
            m_handler(apParam);
    }
    void Run(void* apParam, bool, uint64_t) override { Run(apParam); }
    int GetCallbackSizeBytes() override { return m_size; }

    [[nodiscard]] int Id() const noexcept { return m_iCallback; }

private:
    int m_size;
    std::function<void(const void*)> m_handler;
};
static_assert(sizeof(CCallbackBase) == 16);

struct Exports
{
    void* (*SteamClient)(){};
    void* (*SteamFriends)(){};
    void* (*SteamUser)(){};
    void* (*SteamUtils)(){};
    int32_t (*GetHSteamUser)(){};
    int32_t (*GetHSteamPipe)(){};
    void (*RegisterCallback)(CCallbackBase*, int){};
    void* (*GetGenericInterface)(void*, int32_t, int32_t, const char*){};
    uint64_t (*GetSteamId)(void*){};
    const char* (*GetPersonaName)(void*){};
    int (*GetFriendCount)(void*, int){};
    uint64_t (*GetFriendByIndex)(void*, int, int){};
    bool (*HasFriend)(void*, uint64_t, int){};
    const char* (*GetFriendPersonaName)(void*, uint64_t){};
    int (*GetFriendPersonaState)(void*, uint64_t){};
    bool (*GetFriendGamePlayed)(void*, uint64_t, FriendGameInfo*){};
    bool (*SetRichPresence)(void*, const char*, const char*){};
    void (*ClearRichPresence)(void*){};
    const char* (*GetFriendRichPresence)(void*, uint64_t, const char*){};
    void (*RequestFriendRichPresence)(void*, uint64_t){};
    bool (*InviteUserToGame)(void*, uint64_t, const char*){};
    void (*ActivateGameOverlayToUser)(void*, const char*, uint64_t){};
    int (*GetMediumFriendAvatar)(void*, uint64_t){};
    bool (*GetImageSize)(void*, int, uint32_t*, uint32_t*){};
    bool (*GetImageRGBA)(void*, int, uint8_t*, int){};
};

Exports s_exports{};
bool s_resolved = false;
bool s_ready = false;
void* s_pFriends = nullptr;
void* s_pUser = nullptr;
void* s_pUtils = nullptr;
ISteamNetworkingSockets* s_pSockets = nullptr;
ISteamNetworkingUtils* s_pNetworkingUtils = nullptr;
std::vector<std::unique_ptr<Callback>> s_callbacks;
// listened to before Steam was ready: registered once it is
std::vector<Callback*> s_unregistered;

template <class T> bool Resolve(HMODULE ahModule, T& aFunction, const char* acpName) noexcept
{
    aFunction = reinterpret_cast<T>(GetProcAddress(ahModule, acpName));
    if (!aFunction)
        spdlog::error("Steam: the game's steam_api64.dll has no {}", acpName);
    return aFunction != nullptr;
}

bool ResolveExports() noexcept
{
    HMODULE hModule = GetModuleHandleW(L"steam_api64.dll");
    if (!hModule)
        return false;
    auto& e = s_exports;
    bool ok = true;
    ok &= Resolve(hModule, e.SteamClient, "SteamClient");
    ok &= Resolve(hModule, e.SteamFriends, "SteamFriends");
    ok &= Resolve(hModule, e.SteamUser, "SteamUser");
    ok &= Resolve(hModule, e.SteamUtils, "SteamUtils");
    ok &= Resolve(hModule, e.GetHSteamUser, "SteamAPI_GetHSteamUser");
    ok &= Resolve(hModule, e.GetHSteamPipe, "SteamAPI_GetHSteamPipe");
    ok &= Resolve(hModule, e.RegisterCallback, "SteamAPI_RegisterCallback");
    ok &= Resolve(hModule, e.GetGenericInterface, "SteamAPI_ISteamClient_GetISteamGenericInterface");
    ok &= Resolve(hModule, e.GetSteamId, "SteamAPI_ISteamUser_GetSteamID");
    ok &= Resolve(hModule, e.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    ok &= Resolve(hModule, e.GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    ok &= Resolve(hModule, e.GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    ok &= Resolve(hModule, e.HasFriend, "SteamAPI_ISteamFriends_HasFriend");
    ok &= Resolve(hModule, e.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ok &= Resolve(hModule, e.GetFriendPersonaState, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    ok &= Resolve(hModule, e.GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    ok &= Resolve(hModule, e.SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    ok &= Resolve(hModule, e.ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence");
    ok &= Resolve(hModule, e.GetFriendRichPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence");
    ok &= Resolve(hModule, e.RequestFriendRichPresence, "SteamAPI_ISteamFriends_RequestFriendRichPresence");
    ok &= Resolve(hModule, e.InviteUserToGame, "SteamAPI_ISteamFriends_InviteUserToGame");
    ok &= Resolve(hModule, e.ActivateGameOverlayToUser, "SteamAPI_ISteamFriends_ActivateGameOverlayToUser");
    ok &= Resolve(hModule, e.GetMediumFriendAvatar, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    ok &= Resolve(hModule, e.GetImageSize, "SteamAPI_ISteamUtils_GetImageSize");
    ok &= Resolve(hModule, e.GetImageRGBA, "SteamAPI_ISteamUtils_GetImageRGBA");
    return ok;
}
} // namespace

bool Init() noexcept
{
    if (s_ready)
        return true;
    if (!s_resolved)
    {
        // (once: a DLL without these won't grow them)
        static bool s_tried = false;
        if (std::exchange(s_tried, true) && !s_resolved)
            return false;
        s_resolved = ResolveExports();
        if (!s_resolved)
            return false;
    }

    void* pClient = s_exports.SteamClient();
    s_pFriends = s_exports.SteamFriends();
    s_pUser = s_exports.SteamUser();
    s_pUtils = s_exports.SteamUtils();
    if (!pClient || !s_pFriends || !s_pUser || !s_pUtils)
        return false;

    const int32_t user = s_exports.GetHSteamUser();
    const int32_t pipe = s_exports.GetHSteamPipe();
    s_pSockets = static_cast<ISteamNetworkingSockets*>(s_exports.GetGenericInterface(pClient, user, pipe, STEAMNETWORKINGSOCKETS_INTERFACE_VERSION));
    s_pNetworkingUtils = static_cast<ISteamNetworkingUtils*>(s_exports.GetGenericInterface(pClient, user, pipe, STEAMNETWORKINGUTILS_INTERFACE_VERSION));
    if (!s_pSockets || !s_pNetworkingUtils)
        spdlog::error("Steam: the Steam client gave no {} / {}: no peer to peer", STEAMNETWORKINGSOCKETS_INTERFACE_VERSION, STEAMNETWORKINGUTILS_INTERFACE_VERSION);
    else
        // the relay network's routes looked up now, so a join doesn't wait for them
        s_pNetworkingUtils->InitRelayNetworkAccess();

    s_ready = true;
    for (Callback* pCallback : s_unregistered)
        s_exports.RegisterCallback(pCallback, pCallback->Id());
    s_unregistered.clear();
    spdlog::info("Steam: ready for {} ({}), peer to peer {}", MyName(), MySteamId(), s_pSockets ? "on" : "off");
    return true;
}

bool IsReady() noexcept
{
    return s_ready;
}

uint64_t MySteamId() noexcept
{
    return s_ready ? s_exports.GetSteamId(s_pUser) : 0;
}

std::string MyName() noexcept
{
    const char* pName = s_ready ? s_exports.GetPersonaName(s_pFriends) : nullptr;
    return pName ? pName : "";
}

std::vector<uint64_t> Friends() noexcept
{
    std::vector<uint64_t> friends;
    if (!s_ready)
        return friends;
    const int count = s_exports.GetFriendCount(s_pFriends, kImmediateFriends);
    friends.reserve(std::max(count, 0));
    for (int i = 0; i < count; ++i)
    {
        if (const uint64_t id = s_exports.GetFriendByIndex(s_pFriends, i, kImmediateFriends))
            friends.push_back(id);
    }
    return friends;
}

bool IsFriend(uint64_t aSteamId) noexcept
{
    return s_ready && s_exports.HasFriend(s_pFriends, aSteamId, kImmediateFriends);
}

std::string FriendName(uint64_t aSteamId) noexcept
{
    const char* pName = s_ready ? s_exports.GetFriendPersonaName(s_pFriends, aSteamId) : nullptr;
    return pName ? pName : "";
}

Persona FriendState(uint64_t aSteamId) noexcept
{
    return s_ready ? static_cast<Persona>(s_exports.GetFriendPersonaState(s_pFriends, aSteamId)) : Persona::kOffline;
}

uint32_t FriendAppId(uint64_t aSteamId) noexcept
{
    FriendGameInfo info{};
    if (!s_ready || !s_exports.GetFriendGamePlayed(s_pFriends, aSteamId, &info))
        return 0;
    return static_cast<uint32_t>(info.GameId & 0xFFFFFF);
}

std::string FriendPresence(uint64_t aSteamId, const char* acpKey) noexcept
{
    const char* pValue = s_ready ? s_exports.GetFriendRichPresence(s_pFriends, aSteamId, acpKey) : nullptr;
    return pValue ? pValue : "";
}

void RequestFriendPresence(uint64_t aSteamId) noexcept
{
    if (s_ready)
        s_exports.RequestFriendRichPresence(s_pFriends, aSteamId);
}

bool SetPresence(const char* acpKey, const char* acpValue) noexcept
{
    return s_ready && s_exports.SetRichPresence(s_pFriends, acpKey, acpValue);
}

void ClearPresence() noexcept
{
    if (s_ready)
        s_exports.ClearRichPresence(s_pFriends);
}

bool InviteToGame(uint64_t aSteamId, const std::string& acConnect) noexcept
{
    return s_ready && s_exports.InviteUserToGame(s_pFriends, aSteamId, acConnect.c_str());
}

void OpenOverlayTo(const char* acpDialog, uint64_t aSteamId) noexcept
{
    if (s_ready)
        s_exports.ActivateGameOverlayToUser(s_pFriends, acpDialog, aSteamId);
}

int FriendAvatar(uint64_t aSteamId) noexcept
{
    return s_ready ? s_exports.GetMediumFriendAvatar(s_pFriends, aSteamId) : 0;
}

bool ImageRgba(int aImage, uint32_t& aWidth, uint32_t& aHeight, std::vector<uint8_t>& aPixels) noexcept
{
    if (!s_ready || aImage <= 0 || !s_exports.GetImageSize(s_pUtils, aImage, &aWidth, &aHeight) || aWidth == 0 || aHeight == 0 || aWidth > 256 || aHeight > 256)
        return false;
    aPixels.resize(size_t(aWidth) * aHeight * 4);
    return s_exports.GetImageRGBA(s_pUtils, aImage, aPixels.data(), static_cast<int>(aPixels.size()));
}

ISteamNetworkingSockets* Sockets() noexcept
{
    return s_pSockets;
}

ISteamNetworkingUtils* NetworkingUtils() noexcept
{
    return s_pNetworkingUtils;
}

void Listen(int aCallbackId, int aSize, std::function<void(const void*)> aHandler) noexcept
{
    auto pCallback = std::make_unique<Callback>(aCallbackId, aSize, std::move(aHandler));
    if (s_ready)
        s_exports.RegisterCallback(pCallback.get(), aCallbackId);
    else
        s_unregistered.push_back(pCallback.get());
    s_callbacks.push_back(std::move(pCallback));
}
} // namespace SteamApi

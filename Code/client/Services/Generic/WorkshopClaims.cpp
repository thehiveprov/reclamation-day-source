#if TP_FALLOUT4

// Settlement claims, the client's side of WorkshopService (see WorkshopService.h and the server's WorkshopService)

#include <Services/WorkshopService.h>

#include <World.h>
#include <TESObjectREFR.h>
#include <Messages/RequestSettlementClaim.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <MiniJson.h>

namespace
{
using Clock = std::chrono::steady_clock;

void ShowHudMessage(const std::string& acText) noexcept
{
    // SendHUDMessage::ShowHUDMessage (CommonLibF4 id 1163005): message, sound, throttle, warning
    using TShowHUDMessage = void(const char*, const char*, bool, bool);
    POINTER_FALLOUT4(TShowHUDMessage, s_showHUDMessage, 1163006);
    s_showHUDMessage.Get()(acText.c_str(), "", false, false);
}

// What the game calls the settlement: the workbench's location (TESObjectREFR::GetCurrentLocation, CommonLibF4 id
// 1135470), named through its TESFullName
String GetSettlementName(TESObjectREFR* apWorkshop) noexcept
{
    using TGetCurrentLocation = TESForm*(const TESObjectREFR*);
    POINTER_FALLOUT4(TGetCurrentLocation, s_getCurrentLocation, 1135471);
    const TESForm* pLocation = s_getCurrentLocation.Get()(apWorkshop);
    const char* pName = pLocation ? pLocation->GetName() : "";
    if (!*pName)
        pName = apWorkshop->GetName();
    return *pName ? String(pName) : String("this settlement");
}

// The game's Yes/No box's callback (MessageMenuManager::Create, CommonLibF4 id 89563): an IMessageBoxCallback,
// BSIntrusiveRefCounted (vtable, reference count at 8), operator()(button) in slot 1. Read from the 1.10.163 exe: the
// game takes a reference (lock inc [cb+8]) when it keeps it in its MessageBoxData (+0x58), and when the last reference
// goes it calls slot 0 with 1, the deleting destructor: this one, freeing it with our own allocator.
class YesNoBoxCallback
{
public:
    YesNoBoxCallback(std::function<void(bool)> aAnswer) noexcept
        : m_answer(std::move(aAnswer))
    {
    }
    virtual ~YesNoBoxCallback() = default;

    // On the game's UI thread: answered from our next update. The first button is yes.
    virtual void Answer(uint8_t aButton)
    {
        World::Get().GetRunner().Queue([answer = m_answer, aButton]() { answer(aButton == 0); });
    }

private:
    uint32_t m_refCount = 0; // the game's (BSIntrusiveRefCounted::refCount, right after the vtable)
    std::function<void(bool)> m_answer;
};

std::string ToText(const GameId& acId) noexcept
{
    return fmt::format("{:X}:{:X}", acId.ModId, acId.BaseId);
}

bool FromText(const std::string& acText, GameId& aId) noexcept
{
    const auto colon = acText.find(':');
    if (colon == std::string::npos)
        return false;
    aId.ModId = std::strtoul(acText.substr(0, colon).c_str(), nullptr, 16);
    aId.BaseId = std::strtoul(acText.substr(colon + 1).c_str(), nullptr, 16);
    return aId != GameId{};
}
} // namespace

bool WorkshopService::ShowYesNoBox(const std::string& acBody, const char* apYes, const char* apNo, std::function<void(bool)> aAnswer) noexcept
{
    // MessageMenuManager's singleton (CommonLibF4 id 959572)
    POINTER_FALLOUT4(void*, s_manager, 959573);
    void* pManager = *s_manager.Get();
    if (!pManager)
        return false;

    using TCreate = void(void*, const char*, const char*, YesNoBoxCallback*, uint32_t, const char*, const char*, const char*, const char*, bool);
    POINTER_FALLOUT4(TCreate, s_create, 89564);

    // Warning type 0 (default): the game skips types the player switched off
    s_create.Get()(pManager, "", acBody.c_str(), new YesNoBoxCallback(std::move(aAnswer)), 0, apYes, apNo, nullptr, nullptr, true);
    return true;
}

void WorkshopService::LeaveBuildMode() noexcept
{
    // Workshop::RequestExitWorkshop (CommonLibF4 id 209891): sets the exit request and its argument, which build mode
    // acts on in its next frame. The game's own callers allow entering again (1), entering build mode clears both.
    using TRequestExit = void(bool);
    POINTER_FALLOUT4(TRequestExit, s_requestExit, 209892);
    s_requestExit.Get()(true);
}

bool WorkshopService::MayBuildHere() noexcept
{
    return !s_inBuildMode || (s_buildRights & kMayBuild) != 0;
}

bool WorkshopService::MayScrapHere() noexcept
{
    return !s_inBuildMode || (s_buildRights & kMayScrap) != 0;
}

void WorkshopService::ShowRefusal(bool aScrap) noexcept
{
    const auto now = Clock::now();
    if (now - m_refusalShownAt < std::chrono::seconds(4))
        return;
    m_refusalShownAt = now;

    const SettlementClaim* pClaim = FindClaim(m_buildWorkshopId);
    if (!pClaim)
        ShowHudMessage("Claim this settlement to build here.");
    else
        ShowHudMessage(fmt::format("{} lets you {} here, not {}.", pClaim->LeaderName.c_str(), aScrap ? "build" : "scrap", aScrap ? "scrap or store" : "build"));
}

uint8_t WorkshopService::GetRightsAt(uint32_t aWorkshopFormId) noexcept
{
    {
        std::scoped_lock _{m_rightsLock};
        const auto it = m_rightsByWorkshop.find(aWorkshopFormId);
        if (it != m_rightsByWorkshop.end())
            return it->second;
    }

    // Unclaimed: none. Placed while playing (not a C.A.M.P.'s module): no id the others know, can't be claimed (as
    // before claims)
    return (aWorkshopFormId & 0xFF000000) == 0xFF000000 ? kMayBuild | kMayScrap : 0;
}

bool WorkshopService::IsClaimedBy(uint32_t aWorkshopFormId, const Vector<uint32_t>& acPlayerIds) const noexcept
{
    const SettlementClaim* pClaim = FindClaim(aWorkshopFormId);
    if (!pClaim || !pClaim->LeaderPlayerId)
        return false;
    return pClaim->LeaderName == m_localName || std::find(acPlayerIds.begin(), acPlayerIds.end(), pClaim->LeaderPlayerId) != acPlayerIds.end();
}

const SettlementClaim* WorkshopService::FindClaim(uint32_t aWorkshopFormId) const noexcept
{
    GameId workshopId{};
    if (!aWorkshopFormId || !GetSharedWorkshopId(aWorkshopFormId, workshopId))
        return nullptr;
    const auto it = std::find_if(m_claims.begin(), m_claims.end(), [&workshopId](const SettlementClaim& acClaim) { return acClaim.WorkshopId == workshopId; });
    return it == m_claims.end() ? nullptr : &*it;
}

void WorkshopService::OnNotifySettlementClaims(const NotifySettlementClaims& acMessage) noexcept
{
    m_claims = acMessage.Claims;
    m_localName = acMessage.LocalName;
    m_isAdmin = acMessage.IsAdmin;

    RefreshRights();
    if (m_inBuildMode)
        CheckBuildModeClaim(false);
    SendClaimsUiState();
}

void WorkshopService::RefreshRights() noexcept
{
    std::scoped_lock _{m_rightsLock};
    m_rightsByWorkshop.clear();
    for (const SettlementClaim& claim : m_claims)
    {
        // The workbench here: a C.A.M.P.'s module once it's built here (loaded or not)
        uint32_t formId = 0;
        if (Camp::IsCamp(claim.WorkshopId))
        {
            const auto camp = std::find_if(m_camps.begin(), m_camps.end(), [&claim](const Camp& acCamp) { return acCamp.Id == claim.WorkshopId.BaseId; });
            const auto module = camp != m_camps.end() ? m_itemRefs.find(camp->ModuleItemId) : m_itemRefs.end();
            formId = module != m_itemRefs.end() ? module->second : 0;
        }
        else
            formId = m_world.GetModSystem().GetGameId(claim.WorkshopId);
        if (!formId)
            continue;

        uint8_t rights = 0;
        if (claim.LeaderName == m_localName)
            rights = kMayBuild | kMayScrap;
        for (const SettlementClaim::Right& right : claim.Rights)
        {
            if (right.Name == m_localName)
                rights = (right.Build ? kMayBuild : 0) | (right.Scrap ? kMayScrap : 0);
        }
        m_rightsByWorkshop[formId] = rights;
    }
}

void WorkshopService::CheckBuildModeClaim(bool aEntered) noexcept
{
    if (!m_inBuildMode || !m_transport.IsConnected())
    {
        s_buildRights = kMayBuild | kMayScrap;
        m_askedWorkshopId = 0;
        return;
    }

    auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(m_buildWorkshopId));
    GameId workshopId{};
    // A workbench placed while playing (not a C.A.M.P.'s module) has no id the others know: it can't be claimed, as
    // before claims
    if (!pWorkshop || !GetSharedWorkshopId(m_buildWorkshopId, workshopId))
    {
        s_buildRights = kMayBuild | kMayScrap;
        return;
    }

    const bool camp = Camp::IsCamp(workshopId);
    const SettlementClaim* pClaim = FindClaim(m_buildWorkshopId);
    if (!pClaim)
    {
        s_buildRights = 0;
        // A C.A.M.P.'s claim comes with it
        if (!camp)
            AskToClaim(pWorkshop);
        return;
    }

    const uint8_t rights = GetRightsAt(m_buildWorkshopId);
    const uint8_t previous = s_buildRights.exchange(rights);
    if (!rights)
    {
        spdlog::info("Build mode at {} ({:X}): {}'s, no rights here, leaving", pClaim->Name.c_str(), m_buildWorkshopId, pClaim->LeaderName.c_str());
        LeaveBuildMode();
        ShowHudMessage(camp ? fmt::format("This is {}.", pClaim->Name.c_str()) : fmt::format("{} is {}'s settlement.", pClaim->Name.c_str(), pClaim->LeaderName.c_str()));
        return;
    }

    // Said when entering, and when they change
    m_askedWorkshopId = 0;
    if (!aEntered && previous == rights)
        return;

    if (pClaim->LeaderName == m_localName)
        ShowHudMessage(camp ? std::string("Your C.A.M.P.") : fmt::format("{}: your settlement.", pClaim->Name.c_str()));
    else if (rights == (kMayBuild | kMayScrap))
        ShowHudMessage(fmt::format("{} ({}'s): you can build and scrap.", pClaim->Name.c_str(), pClaim->LeaderName.c_str()));
    else
        ShowHudMessage(fmt::format("{} ({}'s): you can {}, not {}.", pClaim->Name.c_str(), pClaim->LeaderName.c_str(), rights == kMayBuild ? "build" : "scrap",
                                   rights == kMayBuild ? "scrap" : "build"));
}

void WorkshopService::AskToClaim(TESObjectREFR* apWorkshop) noexcept
{
    // Once per visit: the box answers for it
    if (m_askedWorkshopId == apWorkshop->formID)
        return;

    const String name = GetSettlementName(apWorkshop);
    spdlog::info("Build mode at {} ({:X}): nobody claimed it, asking", name.c_str(), apWorkshop->formID);
    const uint32_t workshopFormId = apWorkshop->formID;
    const std::string body = fmt::format("Claim {}? You lead it: you build and scrap there, and give the players of your party the right to.", name.c_str());
    if (ShowYesNoBox(body, "Claim", "Not now", [workshopFormId](bool aClaim) { World::Get().ctx().at<WorkshopService>().OnClaimAnswer(workshopFormId, aClaim); }))
        m_askedWorkshopId = apWorkshop->formID;
    else
    {
        spdlog::warn("Couldn't show the claim box, leaving build mode");
        LeaveBuildMode();
    }
}

void WorkshopService::OnClaimAnswer(uint32_t aWorkshopFormId, bool aClaim) noexcept
{
    auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(aWorkshopFormId));
    GameId workshopId{};
    if (!pWorkshop || !GetSharedWorkshopId(aWorkshopFormId, workshopId))
        return;

    if (!aClaim)
    {
        spdlog::info("Not claiming {:X}", aWorkshopFormId);
        m_askedWorkshopId = 0;
        if (m_inBuildMode && m_buildWorkshopId == aWorkshopFormId)
            LeaveBuildMode();
        return;
    }

    // The claims come back from the server, with ours or with whoever was first
    SendClaimRequest(RequestSettlementClaim::kClaim, workshopId, GetSettlementName(pWorkshop));
}

void WorkshopService::SendClaimRequest(uint8_t aAction, const GameId& acWorkshopId, const String& acSettlementName, const String& acPlayerName, bool aBuild, bool aScrap) noexcept
{
    if (!m_transport.IsConnected())
        return;

    RequestSettlementClaim request;
    request.Action = aAction;
    request.WorkshopId = acWorkshopId;
    request.SettlementName = acSettlementName;
    request.PlayerName = acPlayerName;
    request.Build = aBuild;
    request.Scrap = aScrap;
    m_transport.Send(request);
}

void WorkshopService::ResetClaims() noexcept
{
    m_claims.clear();
    {
        std::scoped_lock _{m_rightsLock};
        m_rightsByWorkshop.clear();
    }
    m_localName.clear();
    m_isAdmin = false;
    m_askedWorkshopId = 0;
    s_buildRights = kMayBuild | kMayScrap;
    SendClaimsUiState();
}

void WorkshopService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    if (acRequest == "get")
    {
        SendClaimsUiState();
        return;
    }

    // The C.A.M.P. card's answer (WorkshopCamps.cpp)
    if (acRequest == "campMove" || acRequest == "campPickUp" || acRequest == "campCancel")
    {
        CloseCampCard();
        AnswerCampMove(acRequest == "campMove");
        if (acRequest == "campPickUp")
            PickUpCamp();
        return;
    }

    // <id> or <id>:<build>:<scrap>:<name> (the name last, it may have colons)
    GameId workshopId{};
    std::string id = acPayload;
    std::string rest;
    if (acRequest == "rights")
    {
        const auto second = acPayload.find(':', acPayload.find(':') + 1);
        id = acPayload.substr(0, second);
        rest = second == std::string::npos ? "" : acPayload.substr(second + 1);
    }
    if (!FromText(id, workshopId))
    {
        spdlog::warn("Settlements: {} with a bad settlement '{}'", acRequest, acPayload);
        return;
    }

    if (acRequest == "abandon" || acRequest == "release")
        SendClaimRequest(RequestSettlementClaim::kAbandon, workshopId);
    else if (acRequest == "rights" && rest.size() >= 4 && rest[1] == ':' && rest[3] == ':')
        SendClaimRequest(RequestSettlementClaim::kSetRights, workshopId, {}, String(rest.substr(4).c_str()), rest[0] == '1', rest[2] == '1');
    else if (acRequest == "pickup")
    {
        const Camp* pCamp = FindLocalCamp();
        if (pCamp && pCamp->GetWorkshopId() == workshopId)
            PickUpCamp();
        else
            spdlog::warn("Settlements: pickup of {}, not the local player's C.A.M.P.", acPayload);
    }
    else
        spdlog::warn("Settlements: unknown request {} '{}'", acRequest, acPayload);
}

// The Settlements tab: every claim, the local player's rights, and for their own settlements everyone with rights and
// the rest of their party
void WorkshopService::SendClaimsUiState() const noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto& players = party.GetPlayers();

    const auto isOnline = [&](const String& acName)
    {
        if (acName == m_localName)
            return true;
        return std::any_of(players.begin(), players.end(), [&acName](const auto& acPlayer) { return acPlayer.second == acName; });
    };

    Vector<String> partyNames;
    if (party.IsInParty())
    {
        for (const uint32_t playerId : party.GetPartyMembers())
        {
            const auto it = players.find(playerId);
            if (it != players.end() && it->second != m_localName)
                partyNames.push_back(it->second);
        }
    }

    GameId currentId{};
    if (m_inBuildMode && m_buildWorkshopId)
        GetSharedWorkshopId(m_buildWorkshopId, currentId);

    std::string settlementsJson;
    for (const SettlementClaim& claim : m_claims)
    {
        const bool mine = claim.LeaderName == m_localName;
        bool canBuild = mine, canScrap = mine;

        std::string rightsJson;
        const auto addRight = [&](const String& acName, bool aBuild, bool aScrap)
        {
            const bool inParty = std::find(partyNames.begin(), partyNames.end(), acName) != partyNames.end();
            rightsJson += fmt::format(R"({}{{"name":{},"online":{},"inParty":{},"build":{},"scrap":{}}})", rightsJson.empty() ? "" : ",", MiniJson::Quote(acName.c_str()),
                                      isOnline(acName) ? "true" : "false", inParty ? "true" : "false", aBuild ? "true" : "false", aScrap ? "true" : "false");
        };

        for (const SettlementClaim::Right& right : claim.Rights)
        {
            if (right.Name == m_localName)
            {
                canBuild = right.Build;
                canScrap = right.Scrap;
            }
            if (mine)
                addRight(right.Name, right.Build, right.Scrap);
        }

        if (mine)
        {
            for (const String& name : partyNames)
            {
                if (std::none_of(claim.Rights.begin(), claim.Rights.end(), [&name](const SettlementClaim::Right& acRight) { return acRight.Name == name; }))
                    addRight(name, false, false);
            }
        }

        const bool stored = Camp::IsCamp(claim.WorkshopId) &&
                            std::any_of(m_camps.begin(), m_camps.end(), [&claim](const Camp& acCamp) { return acCamp.GetWorkshopId() == claim.WorkshopId && acCamp.Stored; });
        settlementsJson += fmt::format(R"({}{{"id":"{}","name":{},"leader":{},"leaderOnline":{},"camp":{},"campStored":{},"mine":{},"canBuild":{},"canScrap":{},"rights":[{}]}})",
                                       settlementsJson.empty() ? "" : ",", ToText(claim.WorkshopId), MiniJson::Quote(claim.Name.c_str()), MiniJson::Quote(claim.LeaderName.c_str()),
                                       claim.LeaderPlayerId ? "true" : "false", Camp::IsCamp(claim.WorkshopId) ? "true" : "false", stored ? "true" : "false",
                                       mine ? "true" : "false", canBuild ? "true" : "false", canScrap ? "true" : "false", rightsJson);
    }

    m_world.GetOverlayService().SendEvent("settlementsState",
                                          fmt::format(R"({{"connected":{},"localName":{},"isHost":{},"currentWorkshop":"{}","settlements":[{}]}})",
                                                      m_transport.IsConnected() ? "true" : "false", MiniJson::Quote(m_localName.c_str()), m_isAdmin ? "true" : "false",
                                                      currentId != GameId{} ? ToText(currentId) : "", settlementsJson));
}

#endif

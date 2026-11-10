#include <HivemindPCH.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Services/QuestService.h>
#include <Services/ImguiService.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Games/Overrides.h>

#include <Events/EventDispatcher.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/RequestQuestTeam.h>
#include <Messages/NotifyQuestTeam.h>

#if TP_FALLOUT4
#include <MiniJson.h>
#endif
#include <PerfProbe.h>

static TESQuest* FindQuestByNameId(const String& name)
{
    auto& questRegistry = ModManager::Get()->quests;
    auto it = std::find_if(questRegistry.begin(), questRegistry.end(), [name](auto* it) { return std::strcmp(it->idName.AsAscii(), name.c_str()); });

    return it != questRegistry.end() ? *it : nullptr;
}

#if TP_FALLOUT4
namespace
{
void ShowHudMessage(const std::string& acText) noexcept
{
    // SendHUDMessage::ShowHUDMessage (CommonLibF4 id 1163005): message, sound, throttle, warning
    using TShowHUDMessage = void(const char*, const char*, bool, bool);
    POINTER_FALLOUT4(TShowHUDMessage, s_showHUDMessage, 1163006);
    s_showHUDMessage.Get()(acText.c_str(), "", false, false);
}

uint8_t GetProgress(const TESQuest* apQuest, const Vector<uint16_t>& acDoneStages) noexcept
{
    if (apQuest->flags & (TESQuest::Completed | TESQuest::Failed))
        return RequestQuestTeam::kCompleted;
    if (apQuest->IsRunning())
        return RequestQuestTeam::kRunning;
    // Stopped part way: it can't be continued
    return acDoneStages.empty() ? RequestQuestTeam::kNotStarted : RequestQuestTeam::kCompleted;
}

bool Contains(const Vector<uint32_t>& acIds, uint32_t aId) noexcept
{
    return std::find(acIds.begin(), acIds.end(), aId) != acIds.end();
}

std::string IdList(const Vector<uint32_t>& acIds) noexcept
{
    std::string out;
    for (const uint32_t id : acIds)
        out += fmt::format("{}{}", out.empty() ? "" : ",", id);
    return out;
}

// "<quest form id in hex>:<number>"
bool ParseQuestAndId(const std::string& acPayload, uint32_t& aQuestId, uint32_t& aId) noexcept
{
    const auto colon = acPayload.find(':');
    if (colon == std::string::npos)
        return false;

    char* pEnd = nullptr;
    aQuestId = std::strtoul(acPayload.c_str(), &pEnd, 16);
    aId = std::strtoul(acPayload.c_str() + colon + 1, &pEnd, 10);
    return aQuestId != 0;
}
} // namespace
#endif

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_joinedConnection = aDispatcher.sink<ConnectedEvent>().connect<&QuestService::OnConnected>(this);
    m_questUpdateConnection = aDispatcher.sink<NotifyQuestUpdate>().connect<&QuestService::OnQuestUpdate>(this);
#if TP_FALLOUT4
    m_questTeamConnection = aDispatcher.sink<NotifyQuestTeam>().connect<&QuestService::OnQuestTeam>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&QuestService::OnDisconnected>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnUpdate>(this);
#endif

    // A note about the Gameevents:
    // TESQuestStageItemDoneEvent gets fired to late, we instead use TESQuestStageEvent, because it responds immediately.
    // TESQuestInitEvent can be instead managed by start stop quest management.
#if TP_FALLOUT
    GetEventDispatcher_TESQuestStartStopEvent()->RegisterSink(this);
    GetEventDispatcher_TESQuestStageEvent()->RegisterSink(this);
#else
    // bind game event listeners
    auto* pEventList = EventDispatcherManager::Get();
    pEventList->questStartStopEvent.RegisterSink(this);
    pEventList->questStageEvent.RegisterSink(this);
#endif
}

void QuestService::OnConnected(const ConnectedEvent&) noexcept
{
}

// Every quest's progress goes to the server: it only passes it on to the players teamed up with us on that quest.
BSTEventResult QuestService::OnEvent(const TESQuestStartStopEvent* apEvent, const EventDispatcher<TESQuestStartStopEvent>*)
{
    if (ScopedQuestOverride::IsOverriden())
        return BSTEventResult::kOk;

    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
            return BSTEventResult::kOk;

        spdlog::info("Quest {:X} {}", apEvent->formId, pQuest->IsStopped() ? "stopped" : "started");

        m_world.GetRunner().Queue(
            [&, formId = pQuest->formID, stageId = pQuest->currentStage, stopped = pQuest->IsStopped()]()
            {
#if TP_FALLOUT4
                // Done with it: the server takes us off its team too
                if (stopped && m_teams.erase(formId))
                    SendUiState();
#endif
                if (!m_world.GetTransport().IsConnected())
                    return;

                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (modSys.GetServerModId(formId, Id))
                {
                    RequestQuestUpdate update;
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = stopped ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started;

                    m_world.GetTransport().Send(update);
                }
            });
    }

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*)
{
    if (ScopedQuestOverride::IsOverriden())
        return BSTEventResult::kOk;

    // there is no reason to even fetch the quest object, since the event provides everything already....
    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
            return BSTEventResult::kOk;

        spdlog::info("Quest {:X} stage {}", apEvent->formId, apEvent->stageId);

        m_world.GetRunner().Queue(
            [&, formId = apEvent->formId, stageId = apEvent->stageId, mainQuest = pQuest->type == TESQuest::Type::QTYPE_MAINQUEST]()
            {
                if (!m_world.GetTransport().IsConnected())
                    return;

#if TP_FALLOUT4
                // A main quest the party has no team for yet: ours becomes its progress (the stage itself follows)
                if (mainQuest && m_mainQuestSync && !m_teams.contains(formId) && m_mainQuestsJoined.insert(formId).second)
                    SendTeamRequest(RequestQuestTeam::kJoinMainQuest, formId, 0);
#endif

                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (modSys.GetServerModId(formId, Id))
                {
                    RequestQuestUpdate update;
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = RequestQuestUpdate::StageUpdate;

                    m_world.GetTransport().Send(update);
                }
            });
    }

    return BSTEventResult::kOk;
}

// A stage a teammate reached on a quest we're doing together
void QuestService::OnQuestUpdate(const NotifyQuestUpdate& aUpdate) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();
    uint32_t formId = modSystem.GetGameId(aUpdate.Id);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(formId));
    if (!pQuest)
    {
        spdlog::error("Failed to find quest, base id: {:X}, mod id: {:X}", aUpdate.Id.BaseId, aUpdate.Id.ModId);
        return;
    }

#if TP_FALLOUT4
    // Left the team since (or only helping): our own progress
    if (!IsParticipant(formId))
    {
        spdlog::info("Teammate's quest {:X} stage {} ignored: not doing it together (anymore)", formId, aUpdate.Stage);
        return;
    }

    // Setting a stage would start it over
    if (pQuest->flags & (TESQuest::Completed | TESQuest::Failed))
    {
        spdlog::info("Teammate's quest {:X} stage {} ignored: we finished it", formId, aUpdate.Stage);
        return;
    }
#endif

    bool bResult = false;
    switch (aUpdate.Status)
    {
    case NotifyQuestUpdate::Started:
    {
        pQuest->ScriptSetStage(aUpdate.Stage);
        pQuest->SetActive(true);
        bResult = true;
        spdlog::info("Remote quest started: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    }
    case NotifyQuestUpdate::StageUpdate:
        pQuest->ScriptSetStage(aUpdate.Stage);
        bResult = true;
        spdlog::info("Remote quest updated: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    case NotifyQuestUpdate::Stopped:
        bResult = StopQuest(formId);
        spdlog::info("Remote quest stopped: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    default: break;
    }

    if (!bResult)
        spdlog::error("Failed to update the client quest state, quest: {:X}, stage: {}, status: {}", formId, aUpdate.Stage, aUpdate.Status);
}

bool QuestService::StopQuest(uint32_t aformId)
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aformId));
    if (pQuest)
    {
        pQuest->SetActive(false);
        pQuest->SetStopped();
        return true;
    }

    return false;
}

// Fallout 4's quest types match TESQuest::Type (None = the game's internal quests, never shown in the Pip-Boy).
bool QuestService::IsNonSyncableQuest(TESQuest* apQuest)
{
    auto& stages = apQuest->stages;
#if TP_FALLOUT4
    // Miscellaneous quests are real quests in Fallout 4 (Pickman Gallery, ...), they can be done together too
    return apQuest->type == TESQuest::Type::None || stages.Empty();
#else
    // non story quests are "blocked" and not synced
    return apQuest->type == TESQuest::Type::None // internal event
           || apQuest->type == TESQuest::Type::Miscellaneous || stages.Empty();
#endif
}

void QuestService::DebugDumpQuests()
{
    auto& quests = ModManager::Get()->quests;
    for (TESQuest* pQuest : quests)
        spdlog::info("{:X}|{}|{}|{}", pQuest->formID, (uint8_t)pQuest->type, pQuest->priority, pQuest->idName.AsAscii());
}

#if TP_FALLOUT4
void QuestService::OnQuestTeam(const NotifyQuestTeam& acMessage) noexcept
{
    // About the party, not a quest
    if (acMessage.Type == NotifyQuestTeam::kMainQuestSync)
    {
        OnMainQuestSync(acMessage.PlayerId != 0);
        return;
    }

    const uint32_t questId = m_world.GetModSystem().GetGameId(acMessage.QuestId);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(questId));
    if (!pQuest)
    {
        spdlog::warn("Quest team message for quest {:X}:{:X}, not loaded here", acMessage.QuestId.ModId, acMessage.QuestId.BaseId);
        return;
    }

    const char* pName = pQuest->GetName();
    const uint32_t localId = m_world.GetTransport().GetLocalPlayerId();

    switch (acMessage.Type)
    {
    case NotifyQuestTeam::kInvite:
    {
        std::erase_if(m_invites, [&](const Invite& acInvite) { return acInvite.QuestId == questId && acInvite.InviterId == acMessage.PlayerId; });
        m_invites.push_back({acMessage.PlayerId, questId});

        spdlog::info("{} invites us to do quest {:X} ({}) together", GetPlayerName(acMessage.PlayerId), questId, pName);
        m_world.GetOverlayService().SendEvent(
            "questTeamInvite", fmt::format(R"({{"questId":"{:08X}","questName":{},"inviterId":{}}})", questId, MiniJson::Quote(pName), acMessage.PlayerId));
        break;
    }
    case NotifyQuestTeam::kTeam:
    {
        const bool wasOnTeam = m_teams.contains(questId);
        auto& team = m_teams[questId];
        team.Participants = acMessage.Participants;
        team.Helpers = acMessage.Helpers;

        std::erase_if(m_invites, [questId](const Invite& acInvite) { return acInvite.QuestId == questId; });

        const bool participant = Contains(team.Participants, localId);
        spdlog::info("Team for quest {:X} ({}): participants [{}], helpers [{}], we're {}, {} stage(s) done", questId, pName, IdList(team.Participants), IdList(team.Helpers),
                     participant ? "a participant" : "a helper", acMessage.Stages.size());

        if (participant)
            CatchUp(pQuest, acMessage.Stages);

        if (!wasOnTeam)
            ShowHudMessage(participant ? fmt::format("Doing {} together", pName) : fmt::format("Helping with {}", pName));

        break;
    }
    case NotifyQuestTeam::kDisbanded:
        if (m_teams.erase(questId))
        {
            spdlog::info("No longer teamed up on quest {:X} ({})", questId, pName);
            ShowHudMessage(fmt::format("No longer doing {} together", pName));
        }
        break;
    case NotifyQuestTeam::kDeclined:
        spdlog::info("{} declined to do quest {:X} ({}) together", GetPlayerName(acMessage.PlayerId), questId, pName);
        ShowHudMessage(fmt::format("{} declined to do {} together", GetPlayerName(acMessage.PlayerId), pName));
        break;
    default: break;
    }

    SendUiState();
}

void QuestService::OnMainQuestSync(bool aSync) noexcept
{
    if (m_mainQuestSync != aSync)
        ShowHudMessage(aSync ? "Main quests: shared with the party" : "Main quests: your own again");

    m_mainQuestSync = aSync;
    m_mainQuestsJoined.clear();
    spdlog::info("Main quest sync {}", aSync ? "on" : "off");

    if (aSync)
        ReportMainQuests();

    SendUiState();
}

void QuestService::ReportMainQuests() noexcept
{
    uint32_t count = 0;
    for (TESQuest* pQuest : ModManager::Get()->quests)
    {
        if (!pQuest || pQuest->type != TESQuest::Type::QTYPE_MAINQUEST || IsNonSyncableQuest(pQuest))
            continue;

        // Only what we've got somewhere with: the party's teams take us along on the others
        if (!pQuest->IsRunning() && pQuest->GetDoneStages().empty())
            continue;

        m_mainQuestsJoined.insert(pQuest->formID);
        SendTeamRequest(RequestQuestTeam::kJoinMainQuest, pQuest->formID, 0);
        ++count;
    }

    spdlog::info("Reported our progress in {} main quest(s) to the party", count);
}

void QuestService::InviteAll(uint32_t aQuestId) const noexcept
{
    const uint32_t localId = m_world.GetTransport().GetLocalPlayerId();
    auto& party = m_world.GetPartyService();

    Vector<uint32_t> targets;
    if (party.IsInParty() && party.GetPartyMembers().size() > 1)
        targets = party.GetPartyMembers();
    else
    {
        for (const auto& [playerId, name] : party.GetPlayers())
            targets.push_back(playerId);
    }

    const auto teamIt = m_teams.find(aQuestId);
    for (const uint32_t playerId : targets)
    {
        if (playerId == localId)
            continue;
        if (teamIt != m_teams.end() && (Contains(teamIt->second.Participants, playerId) || Contains(teamIt->second.Helpers, playerId)))
            continue;

        SendTeamRequest(RequestQuestTeam::kInvite, aQuestId, playerId);
    }
}

void QuestService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfProbe::Section _perf("Quest");
    if (!m_world.GetTransport().IsConnected())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastQuestScan < 2s)
        return;
    m_lastQuestScan = now;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    Vector<TESQuest*> fresh;
    for (auto& objective : pPlayer->objectives)
    {
        TESQuest* pQuest = objective.instance ? objective.instance->quest : nullptr;
        if (!pQuest || !pQuest->IsRunning() || IsNonSyncableQuest(pQuest) || !*pQuest->GetName())
            continue;

        if (m_seenQuests.insert(pQuest->formID).second)
            fresh.push_back(pQuest);
    }

    // What was in the Pip-Boy when we connected; several at once is a save loading, not new quests
    if (!m_seenQuestsReady || fresh.size() > 3)
    {
        m_seenQuestsReady = true;
        return;
    }

    if (m_world.GetPartyService().GetPlayers().empty())
        return;

    const bool party = m_world.GetPartyService().IsInParty() && m_world.GetPartyService().GetPartyMembers().size() > 1;
    for (TESQuest* pQuest : fresh)
    {
        // Already together on it: invited, or the party shares its main quests
        if (m_teams.contains(pQuest->formID) || (m_mainQuestSync && pQuest->type == TESQuest::Type::QTYPE_MAINQUEST))
            continue;

        spdlog::info("New quest {:X} ({}): offering to do it together", pQuest->formID, pQuest->GetName());
        m_world.GetOverlayService().SendEvent(
            "questStarted", fmt::format(R"({{"questId":"{:08X}","questName":{},"party":{}}})", pQuest->formID, MiniJson::Quote(pQuest->GetName()), party ? "true" : "false"));
    }
}

void QuestService::CatchUp(TESQuest* apQuest, const Vector<uint16_t>& acStages) const noexcept
{
    // Setting a stage would start it over
    if (apQuest->flags & (TESQuest::Completed | TESQuest::Failed))
        return;

    // Only along the same path: with stages the team doesn't have, ours went another way (or the server hasn't placed
    // us yet, our progress is on its way)
    const auto done = apQuest->GetDoneStages();
    if (!std::includes(acStages.begin(), acStages.end(), done.begin(), done.end()))
    {
        spdlog::info("Not catching up on quest {:X}: our progress has stages the team's doesn't", apQuest->formID);
        return;
    }

    uint32_t count = 0;
    for (const uint16_t stage : acStages)
    {
        if (apQuest->IsStageDone(stage))
            continue;

        apQuest->ScriptSetStage(stage);
        ++count;
    }

    if (count)
        spdlog::info("Caught up with the team on quest {:X}: set {} stage(s)", apQuest->formID, count);
}

void QuestService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_teams.clear();
    m_invites.clear();
    m_mainQuestSync = false;
    m_mainQuestsJoined.clear();
    m_seenQuests.clear();
    m_seenQuestsReady = false;
    SendUiState();
}

bool QuestService::IsParticipant(uint32_t aQuestId) const noexcept
{
    const auto it = m_teams.find(aQuestId);
    return it != m_teams.end() && Contains(it->second.Participants, m_world.GetTransport().GetLocalPlayerId());
}

uint32_t QuestService::FindTeamedQuest(const Vector<uint32_t>& acQuestIds) const noexcept
{
    for (const uint32_t questId : acQuestIds)
    {
        if (IsParticipant(questId))
            return questId;
    }

    return 0;
}

std::string QuestService::GetPlayerName(uint32_t aPlayerId) const noexcept
{
    const auto& players = m_world.GetPartyService().GetPlayers();
    if (const auto it = players.find(aPlayerId); it != players.end())
        return it->second.c_str();

    return fmt::format("Player {}", aPlayerId);
}

void QuestService::TeamUp(uint32_t aQuestId, uint32_t aPlayerId, bool aInvite) const noexcept
{
    SendTeamRequest(aInvite ? RequestQuestTeam::kInvite : RequestQuestTeam::kAccept, aQuestId, aPlayerId);
}

void QuestService::SendTeamRequest(uint8_t aAction, uint32_t aQuestId, uint32_t aPlayerId) const noexcept
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aQuestId));
    if (!pQuest || !m_world.GetTransport().IsConnected())
        return;

    RequestQuestTeam request{};
    if (!m_world.GetModSystem().GetServerModId(aQuestId, request.QuestId))
    {
        spdlog::warn("Quest {:X} has no id the server knows", aQuestId);
        return;
    }

    request.Action = aAction;
    request.PlayerId = aPlayerId;
    request.Stages = pQuest->GetDoneStages();
    request.QuestProgress = GetProgress(pQuest, request.Stages);

    m_world.GetTransport().Send(request);
    spdlog::info("Quest team request {} for quest {:X} ({}), player {}, our progress {} ({} stage(s) done)", aAction, aQuestId, pQuest->GetName(), aPlayerId, request.QuestProgress,
                 request.Stages.size());
}

void QuestService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    uint32_t questId = 0;
    uint32_t playerId = 0;

    if (acRequest == "invite" && ParseQuestAndId(acPayload, questId, playerId))
        SendTeamRequest(RequestQuestTeam::kInvite, questId, playerId);
    else if ((acRequest == "accept" || acRequest == "decline") && ParseQuestAndId(acPayload, questId, playerId))
    {
        std::erase_if(m_invites, [&](const Invite& acInvite) { return acInvite.QuestId == questId && acInvite.InviterId == playerId; });
        SendTeamRequest(acRequest == "accept" ? RequestQuestTeam::kAccept : RequestQuestTeam::kDecline, questId, playerId);
    }
    else if (acRequest == "leave")
        SendTeamRequest(RequestQuestTeam::kLeave, std::strtoul(acPayload.c_str(), nullptr, 16), 0);
    else if (acRequest == "inviteAll")
        InviteAll(std::strtoul(acPayload.c_str(), nullptr, 16));
    else if (acRequest == "mainQuestSync" && m_world.GetTransport().IsConnected())
    {
        // The party leader's switch, the server checks it
        RequestQuestTeam request{};
        request.Action = RequestQuestTeam::kSetMainQuestSync;
        request.PlayerId = acPayload == "1" ? 1 : 0;
        m_world.GetTransport().Send(request);
        spdlog::info("Asked to switch main quest sync {}", request.PlayerId ? "on" : "off");
    }

    SendUiState();
}

// The quests the player can do together (what the Pip-Boy lists as running) and the teams they're on
void QuestService::SendUiState() const noexcept
{
    const uint32_t localId = m_world.GetTransport().GetLocalPlayerId();

    Vector<TESQuest*> quests;
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
    {
        for (auto& objective : pPlayer->objectives)
        {
            TESQuest* pQuest = objective.instance ? objective.instance->quest : nullptr;
            if (pQuest && pQuest->IsRunning() && !IsNonSyncableQuest(pQuest) && *pQuest->GetName() && std::find(quests.begin(), quests.end(), pQuest) == quests.end())
                quests.push_back(pQuest);
        }
    }

    for (const auto& [questId, team] : m_teams)
    {
        TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(questId));
        if (pQuest && std::find(quests.begin(), quests.end(), pQuest) == quests.end())
            quests.push_back(pQuest);
    }

    std::string questsJson;
    for (TESQuest* pQuest : quests)
    {
        std::string teamJson = "null";
        const char* pRole = "";
        if (const auto it = m_teams.find(pQuest->formID); it != m_teams.end())
        {
            teamJson = fmt::format(R"({{"participants":[{}],"helpers":[{}]}})", IdList(it->second.Participants), IdList(it->second.Helpers));
            pRole = Contains(it->second.Participants, localId) ? "participant" : "helper";
        }

        questsJson += fmt::format(R"({}{{"id":"{:08X}","name":{},"active":{},"team":{},"role":"{}"}})", questsJson.empty() ? "" : ",", pQuest->formID, MiniJson::Quote(pQuest->GetName()),
                                  pQuest->IsActive() ? "true" : "false", teamJson, pRole);
    }

    std::string invitesJson;
    for (const auto& invite : m_invites)
    {
        TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(invite.QuestId));
        invitesJson += fmt::format(R"({}{{"questId":"{:08X}","questName":{},"inviterId":{}}})", invitesJson.empty() ? "" : ",", invite.QuestId,
                                   MiniJson::Quote(pQuest ? pQuest->GetName() : ""), invite.InviterId);
    }

    m_world.GetOverlayService().SendEvent("questsState", fmt::format(R"({{"localPlayerId":{},"connected":{},"mainQuestSync":{},"quests":[{}],"invites":[{}]}})", localId,
                                                                     m_world.GetTransport().IsConnected() ? "true" : "false", m_mainQuestSync ? "true" : "false",
                                                                     questsJson, invitesJson));
}
#endif

#include <Services/DialogueVoteService.h>

#if TP_FALLOUT4

#include <Services/OverlayService.h>
#include <Services/TransportService.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/DialogueLockEvent.h>
#include <Messages/RequestDialogueVote.h>
#include <Messages/NotifyDialogueVote.h>
#include <World.h>
#include <Utils.h>
#include <Components.h>

#include <Actor.h>
#include <PlayerCharacter.h>
#include <PerfProbe.h>

namespace
{
// A vote the server doesn't answer within this (an older server) is no vote: a pick waiting for it goes
constexpr float kAnswerTimeout = 3.f;

DialogueVoteService* s_pService = nullptr;

void ShowOverlayCursor(bool aShow) noexcept
{
    auto& overlay = World::Get().ctx().at<OverlayService>();
    if (overlay.GetActive() == aShow)
        return;
    overlay.SetActive(aShow);
    if (aShow)
        while (ShowCursor(TRUE) <= 0)
            ;
    else
        while (ShowCursor(FALSE) >= 0)
            ;
}

std::string JsonString(const std::string& acText) noexcept
{
    std::string out = "\"";
    for (const unsigned char c : acText)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
                out += fmt::format("\\u{:04x}", c);
            else
                out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

// The server's id of a character here (its FormIdComponent's entity)
std::optional<uint32_t> ServerIdOf(World& aWorld, uint32_t aFormId) noexcept
{
    auto view = aWorld.view<FormIdComponent>();
    for (const auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == aFormId)
            return Utils::GetServerId(entity);
    }
    return std::nullopt;
}

const char* StageName(uint8_t aStage) noexcept
{
    switch (aStage)
    {
    case NotifyDialogueVote::kOpen: return "open";
    case NotifyDialogueVote::kTally: return "tally";
    case NotifyDialogueVote::kResult: return "result";
    default: return "closed";
    }
}
} // namespace

DialogueVoteService::DialogueVoteService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&DialogueVoteService::OnUpdate>(this);
    m_lockConnection = aDispatcher.sink<DialogueLockEvent>().connect<&DialogueVoteService::OnDialogueLock>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&DialogueVoteService::OnDisconnected>(this);
    m_notifyConnection = aDispatcher.sink<NotifyDialogueVote>().connect<&DialogueVoteService::OnNotify>(this);
    s_pService = this;
    DialogueGame::SetPickHandler(&DialogueVoteService::OnPick);
}

DialogueVoteService::~DialogueVoteService() noexcept
{
    DialogueGame::SetPickHandler(nullptr);
    s_pService = nullptr;
}

// --- the talker

void DialogueVoteService::OnDialogueLock(const DialogueLockEvent& acEvent) noexcept
{
    if (acEvent.Lock)
    {
        if (m_npcFormId != acEvent.ActorId)
            EndTalk();
        m_npcFormId = acEvent.ActorId;
    }
    else if (acEvent.ActorId == m_npcFormId)
        EndTalk();
}

void DialogueVoteService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_npcFormId = 0;
    m_talk = Talk::kNone;
    m_options.clear();
    m_voteId = 0;
    m_pendingPick = -1;
    CloseCard();
}

void DialogueVoteService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("DialogueVote");
    // The pick hook goes on once the player exists, when F4SE has loaded XDI (DialogueGame::InstallHook)
    if (PlayerCharacter::Get())
        DialogueGame::InstallHook();
    // online or not: a line of the player's that never ends keeps every conversation shut (DialogueGame)
    DialogueGame::UpdatePlayerVoice(static_cast<float>(acEvent.Delta));

    if (!m_transport.IsOnline() || !m_npcFormId)
    {
        m_wasAwaiting = false;
        return;
    }

    // a new round of options: the menu started waiting, or waits on another of the scene's player dialogue actions
    const bool awaiting = DialogueGame::AwaitingPick();
    const uintptr_t action = awaiting ? DialogueGame::CurrentAction() : 0;
    if (awaiting && action && (!m_wasAwaiting || action != m_action))
        NewOptions(action);
    m_wasAwaiting = awaiting;

    if (m_talk == Talk::kAsked)
    {
        m_askedFor += static_cast<float>(acEvent.Delta);
        if (m_askedFor > kAnswerTimeout)
        {
            spdlog::warn("Dialogue vote: no answer from the server, the pick is the talker's");
            m_talk = Talk::kSolo;
            if (m_pendingPick >= 0)
                DialogueGame::Pick(std::exchange(m_pendingPick, -1));
        }
    }
}

void DialogueVoteService::NewOptions(uintptr_t aAction) noexcept
{
    // the vote on the options before is left behind
    if (m_talk == Talk::kVoting || m_talk == Talk::kAsked)
        SendClose();

    m_action = aAction;
    m_options = DialogueGame::ReadOptions();
    m_pendingPick = -1;
    m_voteId = 0;
    m_askedFor = 0.f;

    const auto enabled = std::count_if(m_options.begin(), m_options.end(), [](const DialogueGame::Option& acOption) { return acOption.Enabled; });
    if (enabled < 2)
    {
        m_talk = Talk::kSolo;
        return;
    }

    RequestDialogueVote request{};
    request.Action = RequestDialogueVote::kOpen;
    if (const auto serverId = ServerIdOf(m_world, m_npcFormId))
        request.NpcServerId = *serverId;
    if (auto* pNpc = Cast<Actor>(TESForm::GetById(m_npcFormId)); pNpc && pNpc->baseForm && pNpc->baseForm->GetName())
        request.NpcName = pNpc->baseForm->GetName();
    for (const DialogueGame::Option& option : m_options)
    {
        RequestDialogueVote::Option entry{};
        entry.Prompt = option.Prompt.c_str();
        entry.Line = option.Line.c_str();
        entry.Challenge = option.Challenge;
        entry.Enabled = option.Enabled;
        request.Options.push_back(entry);
    }
    m_transport.Send(request);
    m_talk = Talk::kAsked;
    spdlog::info("Dialogue vote: {} options up with {}", m_options.size(), request.NpcName.c_str());
}

void DialogueVoteService::EndTalk() noexcept
{
    if (m_talk == Talk::kVoting || m_talk == Talk::kAsked)
        SendClose();
    m_npcFormId = 0;
    m_action = 0;
    m_wasAwaiting = false;
    m_options.clear();
    m_talk = Talk::kNone;
    m_voteId = 0;
    m_pendingPick = -1;
    if (m_cardTalker)
        CloseCard();
}

bool DialogueVoteService::OnPick(int32_t aOption) noexcept
{
    return !s_pService || s_pService->HandlePick(aOption);
}

bool DialogueVoteService::HandlePick(int32_t aOption) noexcept
{
    if (!m_npcFormId || !m_transport.IsOnline())
        return true;
    const int index = IndexOf(aOption);
    // not one of the options (4: none, the conversation ends): it goes
    if (index < 0)
        return true;

    switch (m_talk)
    {
    case Talk::kAsked:
        // the server's answer comes in a moment: the pick waits for it
        m_pendingPick = aOption;
        return false;
    case Talk::kVoting:
        if (m_options[index].Enabled)
        {
            SendCast(m_voteId, static_cast<uint8_t>(index));
            spdlog::info("Dialogue vote: the talker votes for \"{}\"", m_options[index].Prompt);
        }
        return false;
    default: return true;
    }
}

void DialogueVoteService::SendCast(uint32_t aVoteId, uint8_t aChoice) noexcept
{
    RequestDialogueVote request{};
    request.Action = RequestDialogueVote::kCast;
    request.VoteId = aVoteId;
    request.Choice = aChoice;
    m_transport.Send(request);
}

void DialogueVoteService::SendClose() noexcept
{
    if (!m_voteId)
        return;
    RequestDialogueVote request{};
    request.Action = RequestDialogueVote::kClose;
    request.VoteId = m_voteId;
    m_transport.Send(request);
}

int DialogueVoteService::IndexOf(int32_t aOption) const noexcept
{
    for (size_t i = 0; i < m_options.size(); ++i)
    {
        if (m_options[i].Id == aOption)
            return static_cast<int>(i);
    }
    return -1;
}

// --- the server's answers

void DialogueVoteService::OnNotify(const NotifyDialogueVote& acMessage) noexcept
{
    const bool talker = acMessage.TalkerId == m_transport.GetLocalPlayerId();

    if (talker)
    {
        if (acMessage.Stage == NotifyDialogueVote::kOpen)
        {
            if (m_talk != Talk::kAsked)
                return;
            m_voteId = acMessage.VoteId;
            if (acMessage.Voters.size() <= 1)
            {
                // nobody near to vote with: the talker's pick is the pick
                m_talk = Talk::kSolo;
                if (m_pendingPick >= 0)
                    DialogueGame::Pick(std::exchange(m_pendingPick, -1));
                return;
            }
            m_talk = Talk::kVoting;
            if (m_pendingPick >= 0)
            {
                const int index = IndexOf(std::exchange(m_pendingPick, -1));
                if (index >= 0 && m_options[index].Enabled)
                    SendCast(m_voteId, static_cast<uint8_t>(index));
            }
        }
        else if (acMessage.VoteId != m_voteId)
            return;
        else if (acMessage.Stage == NotifyDialogueVote::kResult)
        {
            m_talk = Talk::kDone;
            const int choice = acMessage.Result;
            if (choice >= 0 && static_cast<size_t>(choice) < m_options.size())
            {
                const bool picked = DialogueGame::Pick(m_options[choice].Id);
                spdlog::info("Dialogue vote {}: \"{}\" chosen{}{}", acMessage.VoteId, m_options[choice].Prompt, acMessage.HostBrokeTie ? " (the host broke a tie)" : "",
                             picked ? "" : ", but the dialogue had moved on");
            }
            else
                spdlog::info("Dialogue vote {}: nobody voted, the talker picks", acMessage.VoteId);
        }
        else if (acMessage.Stage == NotifyDialogueVote::kClosed)
            m_talk = Talk::kDone;
    }

    ShowCard(acMessage);
}

// --- the card

void DialogueVoteService::ShowCard(const NotifyDialogueVote& acMessage) noexcept
{
    const uint32_t me = m_transport.GetLocalPlayerId();
    const bool talker = acMessage.TalkerId == me;

    if (acMessage.Stage == NotifyDialogueVote::kOpen)
    {
        // alone: no card
        if (acMessage.Voters.size() <= 1)
            return;
        m_cardVoteId = acMessage.VoteId;
        m_cardTalker = talker;
        m_cardOptions.clear();
        for (const auto& option : acMessage.Options)
        {
            DialogueGame::Option entry{};
            entry.Prompt = option.Prompt.c_str();
            entry.Line = option.Line.c_str();
            entry.Challenge = option.Challenge;
            entry.Enabled = option.Enabled;
            m_cardOptions.push_back(std::move(entry));
        }
        // a voter's card takes the mouse and keys until they vote (the talker votes in the game's dialogue menu)
        if (!talker && !m_cardInput)
        {
            m_cardInput = true;
            ShowOverlayCursor(true);
        }
    }
    else if (acMessage.VoteId != m_cardVoteId)
        return;

    int myChoice = -1;
    std::string voters;
    for (const auto& voter : acMessage.Voters)
    {
        if (voter.PlayerId == me)
            myChoice = voter.Choice;
        voters += fmt::format("{}{{\"name\":{},\"choice\":{},\"talker\":{},\"you\":{}}}", voters.empty() ? "" : ",", JsonString(voter.Name.c_str()), voter.Choice,
                              voter.PlayerId == acMessage.TalkerId ? "true" : "false", voter.PlayerId == me ? "true" : "false");
    }
    std::string options;
    for (const auto& option : m_cardOptions)
    {
        options += fmt::format("{}{{\"prompt\":{},\"line\":{},\"challenge\":{},\"enabled\":{}}}", options.empty() ? "" : ",", JsonString(option.Prompt),
                               JsonString(option.Line), option.Challenge, option.Enabled ? "true" : "false");
    }

    // once the voter has voted, or it's over, the card lets go of the input
    const bool over = acMessage.Stage == NotifyDialogueVote::kResult || acMessage.Stage == NotifyDialogueVote::kClosed;
    if (m_cardInput && (myChoice >= 0 || over))
    {
        m_cardInput = false;
        ShowOverlayCursor(false);
    }

    World::Get().ctx().at<OverlayService>().SendEvent(
        "dialogueVote", fmt::format("{{\"voteId\":{},\"stage\":\"{}\",\"talker\":{},\"npc\":{},\"options\":[{}],\"voters\":[{}],\"msLeft\":{},\"result\":{},"
                                    "\"hostBrokeTie\":{},\"input\":{}}}",
                                    acMessage.VoteId, StageName(acMessage.Stage), talker ? "true" : "false", JsonString(acMessage.NpcName.c_str()), options, voters,
                                    acMessage.MillisecondsLeft, acMessage.Result, acMessage.HostBrokeTie ? "true" : "false", m_cardInput ? "true" : "false"));

    if (over)
    {
        m_cardVoteId = 0;
        m_cardOptions.clear();
    }
}

void DialogueVoteService::CloseCard() noexcept
{
    if (m_cardInput)
    {
        m_cardInput = false;
        ShowOverlayCursor(false);
    }
    m_cardVoteId = 0;
    m_cardOptions.clear();
    World::Get().ctx().at<OverlayService>().SendEvent("dialogueVote", "null");
}

void DialogueVoteService::OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept
{
    if (acRequest == "cast" && m_cardVoteId && !m_cardTalker)
    {
        int choice = -1;
        try
        {
            choice = std::stoi(acPayload);
        }
        catch (...)
        {
        }
        if (choice >= 0 && static_cast<size_t>(choice) < m_cardOptions.size() && m_cardOptions[choice].Enabled)
            SendCast(m_cardVoteId, static_cast<uint8_t>(choice));
    }
    else if (acRequest == "close")
    {
        // the voter let the vote go: the card stays as the tally, without the input
        if (m_cardInput)
        {
            m_cardInput = false;
            ShowOverlayCursor(false);
        }
    }
}

#endif

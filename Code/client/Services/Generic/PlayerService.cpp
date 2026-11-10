#include <Services/PlayerService.h>

#include <World.h>
#include <Services/OverlayService.h>

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/GridCellChangeEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/PlayerDialogueEvent.h>
#include <Events/PlayerLevelEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/BeastFormChangeEvent.h>

#include <Messages/PlayerRespawnRequest.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Messages/PlayerLevelRequest.h>
#include <Messages/RequestDifficulty.h>

#include <Structs/ServerSettings.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESGlobal.h>
#include <Games/Overrides.h>
#include <Games/References.h>
#include <AI/AIProcess.h>
#include <EquipManager.h>
#include <Forms/TESRace.h>
#include <Components.h>
#if TP_FALLOUT4
#include <Forms/TESWorldSpace.h>
#include <Havok/bhkCharacterController.h>
#include <RandomEncounters.h>
#if TP_FALLOUT4
#include <Interface/MainMenuIntegration.h>
#endif
#include <PerfProbe.h>
#endif

namespace
{
// From going down to the respawn: one countdown
constexpr auto kRespawnDelay = 10s;

#if TP_FALLOUT4
// The game's idle camera (AutoVanityState, PlayerCamera state 1). It goes in on an event saying the player's idle and comes out on the one saying that's over (sink 141249C90), and the
// state itself takes no input (its vtable's input calls are empty). Lying down for a countdown is idle to it, and a respawn moves the player with nothing to say it's over, so the
// camera stayed idle after a respawn (seen 11-05). Out the game's own way: 14124E740 (id 760254), back to the state it came from.
bool LeaveIdleCamera() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    uint8_t* pCamera = *s_playerCamera.Get();
    const uint8_t* pState = pCamera ? *reinterpret_cast<const uint8_t* const*>(pCamera + 0x28) : nullptr;
    constexpr uint32_t kAutoVanityState = 1;
    if (!pState || *reinterpret_cast<const uint32_t*>(pState + 0x20) != kAutoVanityState)
        return false;
    using TLeave = void(uint8_t*);
    POINTER_FALLOUT4(TLeave, s_leave, 760255);
    s_leave.Get()(pCamera);
    return true;
}
#endif
} // namespace

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&PlayerService::OnUpdate>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&PlayerService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&PlayerService::OnDisconnected>(this);
    m_settingsConnection = m_dispatcher.sink<ServerSettings>().connect<&PlayerService::OnServerSettingsReceived>(this);
    m_notifyRespawnConnection = m_dispatcher.sink<NotifyPlayerRespawn>().connect<&PlayerService::OnNotifyPlayerRespawn>(this);
    m_gridCellChangeConnection = m_dispatcher.sink<GridCellChangeEvent>().connect<&PlayerService::OnGridCellChangeEvent>(this);
    m_cellChangeConnection = m_dispatcher.sink<CellChangeEvent>().connect<&PlayerService::OnCellChangeEvent>(this);
    m_playerDialogueConnection = m_dispatcher.sink<PlayerDialogueEvent>().connect<&PlayerService::OnPlayerDialogueEvent>(this);
    m_playerLevelConnection = m_dispatcher.sink<PlayerLevelEvent>().connect<&PlayerService::OnPlayerLevelEvent>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&PlayerService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&PlayerService::OnPartyLeftEvent>(this);

#if TP_FALLOUT4
    GetEventDispatcher_TESLoadGameEvent()->RegisterSink(this);
#endif
}

void PlayerService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Player");
    RunRespawnUpdates(acEvent.Delta);
    RunPostDeathUpdates(acEvent.Delta);
    RunDifficultyUpdates();
    RunLevelUpdates();
    RunBeastFormDetection();
#if TP_FALLOUT4
    RunRandomEncounterUpdates(acEvent.Delta);
#endif
}

void PlayerService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    // TODO: SkyrimTogether.esm
    TESGlobal* pKillMove = Cast<TESGlobal>(TESForm::GetById(0x100F19));
    pKillMove->f = 0.f;

#if TP_SKYRIM64
    TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
    pWorldEncountersEnabled->f = 0.f;
#elif TP_FALLOUT4
    // Makes it so that VATS doesn't slow down time
    float* vatsTargetingMult = Settings::GetVATSSelectTargetTimeMultiplier();
    *vatsTargetingMult = 0.f;

    // Random encounters: RunRandomEncounterUpdates decides every second whether this game starts them
#endif
}

void PlayerService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    PlayerCharacter::Get()->SetDifficulty(m_previousDifficulty);
    m_serverDifficulty = m_previousDifficulty = 6;

    ToggleDeathSystem(false);

    TESGlobal* pKillMove = Cast<TESGlobal>(TESForm::GetById(0x100F19));
    pKillMove->f = 1.f;

    // Restore to the default value (150 in skyrim, 175 in fallout 4)
    float* greetDistance = Settings::GetGreetDistance();
#if TP_SKYRIM64
    *greetDistance = 150.f;

    TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
    pWorldEncountersEnabled->f = 1.f;
#elif TP_FALLOUT4
    *greetDistance = 175.f;

    // Restore VATS slow time (default is 0.04)
    float* vatsTargetingMult = Settings::GetVATSSelectTargetTimeMultiplier();
    *vatsTargetingMult = 0.04f;

    RandomEncounters::SetAllowed(true);
    m_randomEncounterTimer = 0.0;
#endif
}

void PlayerService::OnServerSettingsReceived(const ServerSettings& acSettings) noexcept
{
    m_previousDifficulty = *Settings::GetDifficulty();
    // the host keeps its own: the server takes the host's (RunDifficultyUpdates)
#if TP_FALLOUT4
    if (!IsHostingWorld())
#endif
        PlayerCharacter::Get()->SetDifficulty(acSettings.Difficulty);
    m_serverDifficulty = acSettings.Difficulty;

    if (!acSettings.GreetingsEnabled)
    {
        float* greetDistance = Settings::GetGreetDistance();
        *greetDistance = 0.f;
    }

    ToggleDeathSystem(acSettings.DeathSystemEnabled);
}

void PlayerService::OnNotifyPlayerRespawn(const NotifyPlayerRespawn& acMessage) const noexcept
{
    PlayerCharacter::Get()->PayGold(acMessage.GoldLost);

    std::string message = fmt::format("You died and lost {} gold.", acMessage.GoldLost);
    Utils::ShowHudMessage(String(message));
}

void PlayerService::OnGridCellChangeEvent(const GridCellChangeEvent& acEvent) const noexcept
{
    uint32_t baseId = 0;
    uint32_t modId = 0;

    if (m_world.GetModSystem().GetServerModId(acEvent.WorldSpaceId, modId, baseId))
    {
        ShiftGridCellRequest request;
        request.WorldSpaceId = GameId(modId, baseId);
        request.PlayerCell = acEvent.PlayerCell;
        request.CenterCoords = acEvent.CenterCoords;
        request.GridsToLoad = static_cast<uint8_t>(std::clamp(acEvent.GridsToLoad, 1, 255));
        request.Cells = acEvent.Cells;

        m_transport.Send(request);
    }
}

void PlayerService::OnCellChangeEvent(const CellChangeEvent& acEvent) const noexcept
{
    if (acEvent.WorldSpaceId)
    {
        EnterExteriorCellRequest message;
        message.CellId = acEvent.CellId;
        message.WorldSpaceId = acEvent.WorldSpaceId;
        message.CurrentCoords = acEvent.CurrentCoords;

        m_transport.Send(message);
    }
    else
    {
        EnterInteriorCellRequest message;
        message.CellId = acEvent.CellId;

        m_transport.Send(message);
    }
}

void PlayerService::OnPlayerDialogueEvent(const PlayerDialogueEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || !partyService.IsLeader())
        return;

    PlayerDialogueRequest request{};
    request.Text = acEvent.Text;

    m_transport.Send(request);
}

void PlayerService::OnPlayerLevelEvent(const PlayerLevelEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerLevelRequest request{};
    request.NewLevel = PlayerCharacter::Get()->GetLevel();

    m_transport.Send(request);
}

void PlayerService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // TODO: this can be done a bit prettier
#if TP_SKYRIM64
    if (acEvent.IsLeader)
    {
        TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
        pWorldEncountersEnabled->f = 1.f;
    }
#elif TP_FALLOUT4
    // Random encounters: see RunRandomEncounterUpdates
#endif
}

void PlayerService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    // TODO: this can be done a bit prettier
#if TP_SKYRIM64
    if (World::Get().GetTransport().IsConnected())
    {
        TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
        pWorldEncountersEnabled->f = 0.f;
    }
#elif TP_FALLOUT4
    // Random encounters: see RunRandomEncounterUpdates
#endif
}

// The game saves the player's Actor+0x43C flags whole, the death system's essential flag and bleedout recovery lock
// with them, so a save made on a server (or, before v1.6.7-182, after a respawn in single player) loaded without one
// kept the player down for good the first time it went down: nothing counts down to get it up.
BSTEventResult PlayerService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
#if TP_FALLOUT4
    // On a server the death system wants both. Play loads the save before connecting, so it comes here first and
    // the server's settings turn them back on.
    if (m_isDeathSystemEnabled)
        return BSTEventResult::kOk;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return BSTEventResult::kOk;

    const bool essential = pPlayer->IsEssential();
    const bool locked = pPlayer->IsBleedoutRecoveryLocked();
    spdlog::info("Death system: save loaded with the player essential {}, bleedout recovery locked {}, down {}", essential, locked,
                 pPlayer->actorState.IsBleedingOut());

    // The lock is the death system's (the game never locks the player's recovery), and with it the essential flag
    // (SetPlayerRespawnMode sets both). Essential alone can be another mod's: left alone.
    if (!locked)
        return BSTEventResult::kOk;

    // Like leaving a server: a player still down gets up where it is, while still essential, then the flags go
    CancelRespawn();
    if (essential)
        pPlayer->SetPlayerRespawnMode(false);
    else
        pPlayer->SetNoBleedoutRecovery(false);

    spdlog::info("Death system: the save's respawn mode cleared, essential {}, bleedout recovery locked {}", pPlayer->IsEssential(),
                 pPlayer->IsBleedoutRecoveryLocked());
#endif

    return BSTEventResult::kOk;
}

// Verified on Fallout 4, including repeated deaths.
void PlayerService::RunRespawnUpdates(const double acDeltaTime) noexcept
{
    if (!m_isDeathSystemEnabled)
        return;

    // Still recovering from the previous respawn. Fallout 4 doesn't stand the player back up immediately, so the
    // bleedout check below would re-trigger, zero health again and loop the respawn every few seconds.
    if (m_knockdownStart || m_godmodeStart)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer->actorState.IsBleedingOut())
    {
#if TP_SKYRIM64
        m_cachedMainSpellId = pPlayer->magicItems[0] ? pPlayer->magicItems[0]->formID : 0;
        m_cachedSecondarySpellId = pPlayer->magicItems[1] ? pPlayer->magicItems[1]->formID : 0;
        m_cachedPowerId = pPlayer->equippedShout ? pPlayer->equippedShout->formID : 0;
#endif

#if TP_FALLOUT4
        RecordRespawnPosition(acDeltaTime);
#endif

        m_respawnCountdown = false;
        return;
    }

    if (!m_respawnCountdown)
    {
        m_respawnCountdown = true;
        m_respawnDeadline = std::chrono::steady_clock::now() + kRespawnDelay;
        FadeOutGame(true, true, 3.0f, true, 2.0f);
        m_world.GetOverlayService().SetRespawnCountdown(std::chrono::duration<double>(kRespawnDelay).count());

        // If a player dies not by its health reaching 0, getting it up from its bleedout state isn't possible
        // just by setting its health back to max. Therefore, put it to 0.
        if (pPlayer->GetActorValue(ActorValueInfo::kHealth) > 0.f)
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, 0);

        pPlayer->PayCrimeGoldToAllFactions();
    }

    if (std::chrono::steady_clock::now() >= m_respawnDeadline)
    {
        m_world.GetOverlayService().SetRespawnCountdown(0.0);

#if TP_FALLOUT4
        const NiPoint3 down = pPlayer->position;
        bool inBand = false;
        const RespawnSpot* pSpot = PickRespawnPosition(down, &inBand);
        const NiPoint3 to = pPlayer->RespawnPlayer(pSpot ? &pSpot->position : nullptr);
        const float away = glm::distance(static_cast<const glm::vec3&>(to), static_cast<const glm::vec3&>(down));
        if (pSpot)
            spdlog::info("Death system: respawning {:.0f} units ({:.0f} m) from where the player went down, on ground they stood on {:.1f} s before going down{}",
                         away, away / 70.f, std::chrono::duration<double>(std::chrono::steady_clock::now() - pSpot->at).count() - std::chrono::duration<double>(kRespawnDelay).count(),
                         inBand ? "" : " (they'd been nowhere 100-120 m from it here: the farthest they had)");
        else
            spdlog::info("Death system: nowhere the player stood within reach, respawning {:.0f} units ({:.0f} m) from where they went down ({})", away, away / 70.f,
                         pPlayer->GetWorldSpace() ? "the same spot" : "the cell's start");
#else
        pPlayer->RespawnPlayer();
#endif

        m_knockdownDeadline = std::chrono::steady_clock::now() + 1500ms;
        m_knockdownStart = true;

        m_transport.Send(PlayerRespawnRequest());

        m_respawnCountdown = false;

#if TP_SKYRIM64
        auto* pEquipManager = EquipManager::Get();
        TESForm* pSpell = TESForm::GetById(m_cachedMainSpellId);
        if (pSpell)
            pEquipManager->EquipSpell(pPlayer, pSpell, 0);
        pSpell = TESForm::GetById(m_cachedSecondarySpellId);
        if (pSpell)
            pEquipManager->EquipSpell(pPlayer, pSpell, 1);
        pSpell = TESForm::GetById(m_cachedPowerId);
        if (pSpell)
            pEquipManager->EquipShout(pPlayer, pSpell);
#endif
    }
}

// Verified on Fallout 4.
void PlayerService::RunPostDeathUpdates(const double acDeltaTime) noexcept
{
    if (!m_isDeathSystemEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();

    // If a player dies in ragdoll, it gets stuck.
    // This code ragdolls the player again upon respawning.
    // It also makes the player invincible for 10 seconds.
    if (m_knockdownStart)
    {
        if (now >= m_knockdownDeadline)
        {
            PlayerCharacter::SetGodMode(true);
            m_godmodeStart = true;
            m_godmodeDeadline = now + 10s;

#if TP_SKYRIM64
            // Not on Fallout 4: the respawn always goes through MoveTo, which already resets the player's pose,
            // and the player could get stuck in the knockdown (knock state 5) and look dead.
            PlayerCharacter* pPlayer = PlayerCharacter::Get();
            pPlayer->currentProcess->KnockExplosion(pPlayer, &pPlayer->position, 0.f);
#endif

            FadeOutGame(false, true, 0.5f, true, 2.f);

            m_knockdownStart = false;
        }
    }

    if (m_godmodeStart)
    {
#if TP_FALLOUT4
        if (LeaveIdleCamera())
            spdlog::info("Death system: the respawn left the camera in the game's idle view: back out of it");
#endif
        if (now >= m_godmodeDeadline)
        {
            PlayerCharacter::SetGodMode(false);

#if TP_FALLOUT4
            // Make bleedout state unrecoverable again for when the player goes down the next time
            // (deferred from RespawnPlayer, see the comment there)
            PlayerCharacter::Get()->SetNoBleedoutRecovery(true);
#endif

            m_godmodeStart = false;
        }
    }
}

#if TP_FALLOUT4
void PlayerService::RecordRespawnPosition(const double acDeltaTime) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
    // (the reference's own cell: the virtual GetParentCell is GetSaveParentCell, no cell for the player; see RespawnPlayer)
    TESObjectCELL* pCell = pPlayer->GetParentCellEx();

    // Positions are only meaningful within one worldspace, or one interior cell; start over when that changes
    const uint32_t place = pWorldSpace ? pWorldSpace->formID : pCell ? pCell->formID : 0;
    if (place != m_respawnHistoryPlace)
    {
        m_respawnHistoryPlace = place;
        m_respawnHistoryCount = 0;
        m_respawnHistoryNext = 0;
        m_respawnHistoryTimer = 0.0;
    }

    if (!place)
        return;

    m_respawnHistoryTimer -= acDeltaTime;
    if (m_respawnHistoryTimer > 0.0)
        return;

    // Only ground they stood on: not mid-jump, falling, on a grapple line or climbing (the character controller's fall
    // time is 0 on the ground, the same test MovementService uses)
    const bhkCharacterController* pController = pPlayer->GetCharController();
    if (!pController || pController->GetFallTime() > 0.f)
        return;

    m_respawnHistoryTimer = kRespawnHistoryEvery;

    // Only somewhere new: a few metres from the last spot kept
    if (m_respawnHistoryCount)
    {
        const RespawnSpot& last = m_respawnHistory[(m_respawnHistoryNext + kRespawnHistorySize - 1) % kRespawnHistorySize];
        if (glm::distance(static_cast<const glm::vec3&>(last.position), static_cast<const glm::vec3&>(pPlayer->position)) < kRespawnSpotApart)
            return;
    }

    m_respawnHistory[m_respawnHistoryNext] = {pPlayer->position, std::chrono::steady_clock::now()};
    m_respawnHistoryNext = (m_respawnHistoryNext + 1) % kRespawnHistorySize;
    m_respawnHistoryCount = std::min(m_respawnHistoryCount + 1, kRespawnHistorySize);
}

const PlayerService::RespawnSpot* PlayerService::PickRespawnPosition(const NiPoint3& acDeathPosition, bool* apInBand) const noexcept
{
    // Back along where they came from: the newest spot they stood on 100 to 120 m from where they went down (70 units a
    // metre). Spots are 3 m apart walking, about 10 on a bike, so a 20 m band always has one where they passed
    // through it. Only ground they stood on: never water, a roof or inside a rock.
    constexpr float kNear = 7000.f;
    constexpr float kFarthest = 8400.f;
    if (apInBand)
        *apInBand = false;

    const RespawnSpot* pFarthestNear = nullptr;
    float farthestNear = -1.f;
    for (size_t i = 1; i <= m_respawnHistoryCount; ++i)
    {
        const size_t index = (m_respawnHistoryNext + kRespawnHistorySize - i) % kRespawnHistorySize;
        const RespawnSpot& candidate = m_respawnHistory[index];
        const float distance = glm::distance(static_cast<const glm::vec3&>(candidate.position), static_cast<const glm::vec3&>(acDeathPosition));

        if (distance >= kNear && distance <= kFarthest)
        {
            if (apInBand)
                *apInBand = true;
            return &candidate;
        }

        if (distance < kNear && distance > farthestNear)
        {
            farthestNear = distance;
            pFarthestNear = &candidate;
        }
    }

    // Nowhere 100 m off since they came into this worldspace or cell (a save loaded beside the fight, say): the farthest
    // they stood from it, rather than anywhere they haven't been
    return pFarthestNear;
}

// Every game evaluates the random encounter story nodes on its own, so two players near each other would each spawn a
// copy of the same encounter. Only one game per area runs them: while another player who outranks us has their
// character loaded in our game, ours stays quiet and their encounters reach us through the normal NPC sync. When they
// walk away, ours run again.
void PlayerService::RunRandomEncounterUpdates(const double acDeltaTime) noexcept
{
    m_randomEncounterTimer -= acDeltaTime;
    if (m_randomEncounterTimer > 0.0)
        return;
    m_randomEncounterTimer = 1.0;

    bool allowed = true;
    if (m_transport.IsConnected())
    {
        auto view = m_world.view<PlayerComponent, FormIdComponent>();
        for (const auto entity : view)
        {
            const uint32_t playerId = view.get<PlayerComponent>(entity).Id;
            if (!OutranksForEncounters(playerId))
                continue;

            Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
            if (pActor && pActor->GetNiNode())
            {
                allowed = false;
                break;
            }
        }
    }

    RandomEncounters::SetAllowed(allowed);
}

// The party leader first (like Skyrim, where only the leader runs world encounters), then the lowest player id. Both
// games come to the same answer, so exactly one of two nearby players runs encounters.
bool PlayerService::OutranksForEncounters(const uint32_t aPlayerId) const noexcept
{
    const uint32_t localId = m_transport.GetLocalPlayerId();
    if (aPlayerId == localId)
        return false;

    const auto& partyService = m_world.GetPartyService();
    if (partyService.IsInParty())
    {
        if (partyService.GetLeaderPlayerId() == aPlayerId)
            return true;

        const auto& members = partyService.GetPartyMembers();
        if (partyService.IsLeader() && std::find(members.begin(), members.end(), aPlayerId) != members.end())
            return false;
    }

    return aPlayerId < localId;
}
#endif

void PlayerService::RunDifficultyUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    // Once a second, and at once when the server's changes: every SetDifficulty sends the game's difficulty changed
    // event, and Survival's HC_ManagerScript handles each one in Papyrus. Every update (several a frame) piled up
    // hundreds of them and held every other script back (10-22: 733 waiting, the prologue's Pip-Boy never handed over).
    const auto now = std::chrono::steady_clock::now();

#if TP_FALLOUT4
    // The host's difficulty is the server's, as its weather and time are: when the host changes it (pause menu), the
    // server takes it and sends it to everyone; never forced back onto the host
    if (IsHostingWorld())
    {
        const int32_t local = *Settings::GetDifficulty();
        if (local >= 0 && local <= 5 && local != m_serverDifficulty && now - m_lastDifficultyUpdate >= 2000ms)
        {
            RequestDifficulty request{};
            request.Difficulty = static_cast<uint8_t>(local);
            m_transport.Send(request);
            m_lastDifficultyUpdate = now;
            spdlog::info("Difficulty: the host plays at {}, the server at {}: asking the server to follow", local, m_serverDifficulty);
        }
        return;
    }
#endif

    if (m_appliedDifficulty == m_serverDifficulty && now - m_lastDifficultyUpdate < 1000ms)
        return;

    m_lastDifficultyUpdate = now;
    m_appliedDifficulty = m_serverDifficulty;
    PlayerCharacter::Get()->SetDifficulty(m_serverDifficulty);
}

void PlayerService::RunLevelUpdates() const noexcept
{
    // The LevelUp hook is kinda weird, so ehh, just check periodically, doesn't really cost anything.

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static uint16_t oldLevel = PlayerCharacter::Get()->GetLevel();

    uint16_t newLevel = PlayerCharacter::Get()->GetLevel();
    if (newLevel != oldLevel)
    {
        PlayerLevelRequest request{};
        request.NewLevel = newLevel;

        m_transport.Send(request);

        oldLevel = newLevel;
    }
}

void PlayerService::RunBeastFormDetection() const noexcept
{
#if TP_SKYRIM64
    static uint32_t lastRaceFormID = 0;
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer->race)
        return;
    
    if (pPlayer->race->formID == lastRaceFormID)
        return;

    if (pPlayer->race->formID == 0x200283A || pPlayer->race->formID == 0xCDD84)
        m_world.GetDispatcher().trigger(BeastFormChangeEvent());

    lastRaceFormID = pPlayer->race->formID;
#endif
}

void PlayerService::ToggleDeathSystem(bool aSet) noexcept
{
    if (!aSet && m_isDeathSystemEnabled)
        CancelRespawn();

    m_isDeathSystemEnabled = aSet;

    PlayerCharacter::Get()->SetPlayerRespawnMode(aSet);
}

void PlayerService::CancelRespawn() noexcept
{
    // The countdown and its fade, or the knockdown's fade
    if (m_respawnCountdown || m_knockdownStart)
    {
        m_world.GetOverlayService().SetRespawnCountdown(0.0);
        FadeOutGame(false, true, 0.5f, true, 2.f);
    }
    m_respawnCountdown = false;
    m_knockdownStart = false;

    if (m_godmodeStart)
    {
        PlayerCharacter::SetGodMode(false);
        m_godmodeStart = false;
    }

#if TP_FALLOUT4
    // Left down, with no countdown to get it up
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (pPlayer->actorState.IsBleedingOut())
        pPlayer->GetUpInPlace();
#endif
}

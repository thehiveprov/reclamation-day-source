#include "PlayerCharacter.h"

#include <Games/References.h>
#include <AI/AIProcess.h>
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <Structs/GridCellCoords.h>
#include <Services/PapyrusService.h>
#include <World.h>

int32_t PlayerCharacter::LastUsedCombatSkill = -1;

void PlayerCharacter::SetDifficulty(const int32_t aDifficulty, bool aForceUpdate, bool aExpectGameDataLoaded) noexcept
{
    if (aDifficulty > 5 || aDifficulty < 0)
        return;

    TP_THIS_FUNCTION(TSetDifficulty, void, PlayerCharacter, const int32_t aDifficulty, bool aForceUpdate, bool aExpectGameDataLoaded);
    POINTER_FALLOUT4(TSetDifficulty, setDifficulty, 657345);
    TiltedPhoques::ThisCall(setDifficulty, this, aDifficulty, aForceUpdate, aExpectGameDataLoaded);
}

void PlayerCharacter::PayCrimeGoldToAllFactions() noexcept
{
    // Fallout 4 has no bounties to pay off, but crimes still leave guards/factions hostile after a respawn.
    // StopCombatAlarm on the player clears that; it's looked up by name, skip it if this game doesn't have it.
    if (!World::Get().ctx().at<PapyrusService>().Get("Actor", "StopCombatAlarm"))
    {
        spdlog::warn("{}: Actor.StopCombatAlarm not found, hostility won't be cleared on respawn", __FUNCTION__);
        return;
    }

    PAPYRUS_FUNCTION(void, Actor, StopCombatAlarm);
    s_pStopCombatAlarm(this);
}

// Verified on Fallout 4: bleedout, loading screen, nearby spot, getting back up.
NiPoint3 PlayerCharacter::RespawnPlayer() noexcept
{
    return RespawnPlayer(nullptr);
}

void PlayerCharacter::GetUpInPlace() noexcept
{
    // Make bleedout state recoverable
    SetNoBleedoutRecovery(false);

    // Reset health to max
    // TODO(cosideci): there's a cleaner way to do this
    ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, 1000000);

    // Up at the next update. The game only gets a bleeding out actor up once its down timer has run out, and that
    // timer (10 s) doesn't count while recovery is locked, so the player stayed down 10 s past the respawn plus the
    // get-up, and PlayerService's bleedout check started a second countdown.
    if (currentProcess)
        currentProcess->essentialDownTimer = 0.f;
}

NiPoint3 PlayerCharacter::RespawnPlayer(const NiPoint3* apTarget) noexcept
{
    DispelAllSpells();
    GetUpInPlace();

    NiPoint3 pos = position;

    // The cell they're in (the reference's own). The virtual named GetParentCell here is slot 0xBE, GetSaveParentCell in
    // 1.10.163 (CommonLibF4): it gave no cell for the player outdoors, so every respawn returned here without moving them
    // (11-06 log: 21 respawns, each "0 units from where the player went down").
    TESObjectCELL* pCell = GetParentCellEx();
    if (!pCell)
        return pos;

    // Exterior cells in Fallout 4 generally have no COC marker, so the Skyrim approach (teleport to the center grid
    // cell's COC placement) sent the player to a bogus position and an endless loading screen. A caller supplied spot
    // instead: a recent position of the player, so known walkable ground, near where they went down.
    if (apTarget)
    {
        pos = *apTarget;

        // Outdoors the target can be in a neighbouring cell; attaching the player to the death cell instead put them
        // where that cell has no terrain, i.e. underground. (Indoors it's the same cell: the history starts over on
        // a new one.)
        if (GetWorldSpace())
        {
            const GridCellCoords coords = GridCellCoords::CalculateGridCellCoords(pos.x, pos.y);
            if (TESObjectCELL* pTargetCell = ModManager::Get()->GetCellFromCoordinates(coords.X, coords.Y, GetWorldSpace(), false))
                pCell = pTargetCell;
        }

        // Recorded positions are at foot level; drop the player onto the ground rather than into it.
        constexpr float kSpawnHeightMargin = 100.f; // ~1.4m
        pos.z += kSpawnHeightMargin;
    }
    else if (!GetWorldSpace())
    {
        // Indoors with nowhere of theirs to go: the start of the cell
        NiPoint3 rot{};
        pCell->GetCOCPlacementInfo(&pos, &rot, true);
    }

    MoveTo(pCell, pos);

    // Unlike Skyrim, Fallout 4 only recovers from bleedout later during the actor update, so re-locking recovery here
    // (same frame) kept the player down forever. PlayerService re-locks it once the post-respawn window is over.

    return pos;
}

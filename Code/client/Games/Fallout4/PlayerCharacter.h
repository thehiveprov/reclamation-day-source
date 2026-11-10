#pragma once

#include <Actor.h>

struct TESQuest;

struct PlayerCharacter : Actor
{
    static PlayerCharacter* Get() noexcept;

    static void SetGodMode(bool aSet) noexcept;

    static int32_t LastUsedCombatSkill;

    void SetDifficulty(const int32_t aDifficulty, bool aForceUpdate = true, bool aExpectGameDataLoaded = true) noexcept;

    void PayCrimeGoldToAllFactions() noexcept;
    // Out of a bleedout where the player is: recovery unlocked, health back, up at the game's next update
    void GetUpInPlace() noexcept;
    NiPoint3 RespawnPlayer() noexcept;
    // apTarget: where to put the player (a spot they stood on, at foot level), outdoors or in the same interior cell.
    // nullptr: outdoors where they died, in an interior the cell's start. The teleport goes through MoveTo, so the
    // player gets a loading screen either way. Returns where they were put.
    NiPoint3 RespawnPlayer(const NiPoint3* apTarget) noexcept;

    struct Objective
    {
        uint64_t pad0;
        TESQuest* quest;
    };

    struct ObjectiveInstance
    {
        Objective* instance;
        uint64_t instanceCount;
    };

    char pad490[0x7D8 - sizeof(Actor)];
    GameArray<ObjectiveInstance> objectives;
    char pad7F0[0x4F0 - 24];
    TESForm* locationForm; // actually a bgslocation
    uint8_t pad_CD0[0x140];
};

static_assert(sizeof(PlayerCharacter) == 0xE10);
static_assert(offsetof(PlayerCharacter, objectives) == 0x7D8);
static_assert(offsetof(PlayerCharacter, locationForm) == 0xCC8);

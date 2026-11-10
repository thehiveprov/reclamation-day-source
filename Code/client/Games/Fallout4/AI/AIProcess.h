#pragma once

struct MiddleProcess;
struct TESIdleForm;
struct TESObjectREFR;

struct AIProcess
{
    void KnockExplosion(Actor* apActor, const NiPoint3* aSourceLocation, float afMagnitude);
    // Plays apIdle on apActor like Papyrus PlayIdle does; false when it can't play right now or (when tested) its
    // conditions fail
    bool PlayIdle(Actor* apActor, TESIdleForm* apIdle, TESObjectREFR* apTarget = nullptr, bool aTestConditions = true) noexcept;

    uint8_t unk0[0x8];
    MiddleProcess* middleProcess;
    uint8_t unk10[0x98 - 0x10];
    // Set when the actor goes down (10 s for the player, fEssentialCompanionDeathTime), counts down while its bleedout
    // recovery isn't locked; the game gets the actor up once it has run out
    float essentialDownTimer;
};

static_assert(offsetof(AIProcess, middleProcess) == 0x8);
static_assert(offsetof(AIProcess, essentialDownTimer) == 0x98);

#pragma once

#include <Forms/TESForm.h>
#include <Games/Magic/MagicSystem.h>

struct EffectSetting : TESForm
{
    // Fallout 4's archetypes are compared by number (CommonLibF4 EffectArchetypes::ArchetypeID): the shared enum is
    // Skyrim's, and Fallout 4's differ from 24 on

    // What the other players see on the target, so their copies of it get it too: hidden (Invisibility 11; Chameleon
    // 49, a Stealth Boy and the legendary armor effect) or frozen in place (Paralysis 21, paralyzing poison). Cryo isn't
    // one: its freeze is a Script effect (cryoFreezeSCRIPT) setting the target's AnimationMult, which reaches the copies
    // through the actor value sync. Numbers read from Fallout4.esm's MGEF records (jetpack 48, Chameleon 49, Stimpak 31)
    bool IsSeenByOthers() const noexcept
    {
        const auto archetype = static_cast<int32_t>(eArchetype);
        return archetype == 11 || archetype == 21 || archetype == 49;
    }

    // Brings something into the world or takes it out (Command Summoned 10, Summon Creature 18, Reanimate 22, Spawn
    // Hazard 40, Banish 42, Spawn Scripted Ref 43, Permanent Reanimate 47): the game that cast it did that already
    bool ChangesTheWorld() const noexcept
    {
        switch (static_cast<int32_t>(eArchetype))
        {
        case 10:
        case 18:
        case 22:
        case 40:
        case 42:
        case 43:
        case 47: return true;
        default: return false;
        }
    }

    uint8_t pad20[0xD0 - 0x20];
    EffectArchetypes::ArchetypeID eArchetype;
    uint8_t padD4[0x1B0 - 0xD4];
};

static_assert(sizeof(EffectSetting) == 0x1B0);

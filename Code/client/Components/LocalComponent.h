#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActionEvent.h>
#include <Structs/Movement.h>

#include <chrono>

struct LocalComponent
{
    LocalComponent(uint32_t aId, uint32_t aOwnershipEpoch) noexcept
        : Id(aId)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    uint32_t Id;
    uint32_t OwnershipEpoch;
    ActionEvent CurrentAction;
    bool IsDead = false;
    bool IsWeaponDrawn = false;
    // What the others last got of the actor and when: AnimationSystem::Serialize leaves it out while it hasn't changed
    Movement LastSentMovement;
    std::chrono::steady_clock::time_point LastSentTime{};
};

#pragma once

/**
 * @brief Dispatched when the local player hits a character or a character hits the local player (Fallout 4,
 * Actor::HookDamageActor). Nemeses learn who fought whom from it.
 */
struct ActorDamageEvent
{
    ActorDamageEvent(uint32_t aHitteeId, uint32_t aHitterId, float aDamage)
        : HitteeId(aHitteeId)
        , HitterId(aHitterId)
        , Damage(aDamage)
    {
    }

    uint32_t HitteeId;
    uint32_t HitterId;
    float Damage;
};

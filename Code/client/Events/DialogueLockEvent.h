#pragma once

/**
 * @brief The local player started (Lock) or ended (!Lock) a conversation with an NPC.
 */
struct DialogueLockEvent
{
    DialogueLockEvent(uint32_t aActorId, bool aLock)
        : ActorId(aActorId)
        , Lock(aLock)
    {
    }

    uint32_t ActorId;
    bool Lock;
};

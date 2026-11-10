#pragma once

/**
 * @brief The local player dropped an item into the world (Fallout 4).
 */
struct DropItemEvent
{
    DropItemEvent(uint32_t aRefId, int32_t aCount)
        : RefId(aRefId)
        , Count(aCount)
    {
    }

    uint32_t RefId; // the reference created in the world
    int32_t Count;
};

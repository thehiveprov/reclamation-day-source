#pragma once

/**
 * @brief A destructible object's stage went up in the local game (see Games/Fallout4/Destruction.h).
 */
struct DestructionStageEvent
{
    uint32_t ObjectId;
    uint32_t OldStage;
    uint32_t NewStage;
};

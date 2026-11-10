#pragma once

/**
 * @brief The overlay's page started again after its process ended (OverlayService): everything the old page was told is
 * gone. What sends the page its state only when that changes sends it again. On the game's main loop.
 */
struct OverlayRestartedEvent
{
};

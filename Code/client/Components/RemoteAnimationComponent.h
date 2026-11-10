#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

struct RemoteAnimationComponent
{
    List<ActionEvent> TimePoints;
    ActionEvent LastRanAction;
    ActionEvent LastProcessedAction;
    // How many of the queued actions are a replay of what the actor did before we started seeing it
    uint32_t ReplayCount{0};
    // Put the graph back to its base state before the replay (the actor left what it was doing since)
    bool ResetAnimationGraphForReplay{false};
};

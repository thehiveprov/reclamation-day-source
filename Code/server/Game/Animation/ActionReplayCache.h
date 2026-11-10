#pragma once

#include <Structs/ActionEvent.h>
#include <Structs/ActionReplayChain.h>

//
// Note: the _best_ solution to the 'proper animation state' problem is to implement animation graphs serialization,
// and then send the graphs over the network. But since that is a pretty hard reverse engineering task,
// let's do simple action replays until better days.
//
// The recent actions of an actor, replayed by players who start seeing it (it spawns for them or gets assigned to them
// as a remote actor) so it shows up in the right animation state instead of sliding around in its default pose.
//
class ActionReplayCache
{
public:
    static constexpr uint32_t kReplayCacheMaxSize = 32;

    ActionReplayChain FormRefinedReplayChain() noexcept;

    /// Appends actions to the replay cache
    void AppendAll(const Vector<ActionEvent>& acActions) noexcept;

    const Vector<ActionEvent>& GetActions() const noexcept { return m_actions; }

private:
    // Returns true if clients should reset the animation graph of the Actor before replaying
    bool RefineReplayCache() noexcept;

    Vector<ActionEvent> m_actions{};
};

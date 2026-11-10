#include "ActionReplayCache.h"

#include <Game/Animation/AnimationEventLists.h>

ActionReplayChain ActionReplayCache::FormRefinedReplayChain() noexcept
{
    const bool shouldResetAnimGraph = RefineReplayCache();

    ActionReplayChain chain;
    chain.ResetAnimationGraph = shouldResetAnimGraph;
    chain.Actions = (shouldResetAnimGraph && !m_actions.empty())
        ? Vector<ActionEvent>{m_actions.begin() + 1, m_actions.end()} // Drop the exit action
        : m_actions;

    return chain;
}

void ActionReplayCache::AppendAll(const Vector<ActionEvent>& acActions) noexcept
{
    for (const auto& action : acActions)
    {
        if (AnimationEventLists::IsIgnored(action.EventName))
            continue;

        m_actions.push_back(action);
    }

    if (m_actions.size() > kReplayCacheMaxSize)
        m_actions.erase(m_actions.begin(), m_actions.end() - kReplayCacheMaxSize);
}

bool ActionReplayCache::RefineReplayCache() noexcept
{
    int32_t dropAllUpToIndex = -1;

    for (int32_t i = static_cast<int32_t>(m_actions.size()) - 1; i >= 0; --i)
    {
        ActionEvent& action = m_actions[i];

        // Instant counterparts of actions are highly preferred for animation replay. They're sent to the graph as they
        // are (no action, no idle).
        if (const auto instantAnimName = AnimationEventLists::FindInstantCounterpart(action.EventName))
        {
            action.EventName = String{*instantAnimName};
            action.TargetEventName = String{*instantAnimName};
            action.ActionId = 0;
            action.IdleId = 0;
        }

        if (AnimationEventLists::IsExit(action.EventName))
        {
            // An "exit" action is found, meaning the actor has likely returned to its root/normal
            // state, and the clients should reset the animation state before running replay
            dropAllUpToIndex = i;
            break;
        }
    }

    if (dropAllUpToIndex == -1)
        return false;

    m_actions.erase(m_actions.begin(), m_actions.begin() + dropAllUpToIndex);
    return true;
}

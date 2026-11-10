#include "DialogueVotes.h"

#include <algorithm>

namespace DialogueVotes
{
Outcome Decide(const std::vector<int>& acChoices, size_t aOptionCount, int aHost, int aTalker) noexcept
{
    std::vector<int> counts(aOptionCount, 0);
    for (const int choice : acChoices)
    {
        if (choice >= 0 && static_cast<size_t>(choice) < aOptionCount)
            ++counts[choice];
    }

    Outcome outcome{};
    const auto most = std::max_element(counts.begin(), counts.end());
    if (most == counts.end() || *most == 0)
        return outcome;

    const int top = *most;
    const auto tied = [&](int aVoter) {
        if (aVoter < 0 || static_cast<size_t>(aVoter) >= acChoices.size())
            return -1;
        const int choice = acChoices[aVoter];
        return choice >= 0 && static_cast<size_t>(choice) < aOptionCount && counts[choice] == top ? choice : -1;
    };

    const bool tie = std::count(counts.begin(), counts.end(), top) > 1;
    if (!tie)
    {
        outcome.Choice = static_cast<int>(most - counts.begin());
        return outcome;
    }
    if (const int host = tied(aHost); host >= 0)
    {
        outcome.Choice = host;
        outcome.HostBrokeTie = true;
        return outcome;
    }
    if (const int talker = tied(aTalker); talker >= 0)
    {
        outcome.Choice = talker;
        return outcome;
    }
    outcome.Choice = static_cast<int>(most - counts.begin());
    return outcome;
}

bool AllVoted(const std::vector<int>& acChoices) noexcept
{
    return !acChoices.empty() && std::all_of(acChoices.begin(), acChoices.end(), [](int aChoice) { return aChoice >= 0; });
}
} // namespace DialogueVotes

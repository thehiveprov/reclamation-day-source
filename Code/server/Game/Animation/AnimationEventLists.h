#pragma once

#include <optional>
#include <string_view>

// Animation event names the action replay cache treats specially. Matched ignoring case: Fallout 4 uses the same event
// with different casings (g_IdleSitInstant / g_idleSitInstant), and its graphs don't care either.
namespace AnimationEventLists
{
// The event that puts the actor straight into the state another one transitions into ("instant" counterpart), if any
std::optional<std::string_view> FindInstantCounterpart(std::string_view aEventName) noexcept;

// The actor is back in its root/normal state after this event: what happened before it doesn't need replaying
bool IsExit(std::string_view aEventName) noexcept;

// Not worth replaying (spam, one-shot transitions, or synced some other way)
bool IsIgnored(std::string_view aEventName) noexcept;
} // namespace AnimationEventLists

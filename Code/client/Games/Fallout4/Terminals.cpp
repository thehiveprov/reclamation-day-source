#include <Terminals.h>

#include <Games/References.h>
#include <Forms/TESForm.h>

#include <Events/TerminalItemRunEvent.h>

#include <World.h>

namespace
{
// What the terminal menu sends when an item is picked (read from GameScript::FragmentEventHandler's sink, 496469:
// the handler reads nothing else; the event source isn't used either)
struct TerminalMenuItemRunEvent
{
    void* pUnknown;              // not read by the handler
    TESObjectREFR* pTerminalRef; // the terminal used, akTerminalRef in the scripts
    uint32_t TerminalFormId;     // the TERM whose item it is: the one the fragments are looked up for
    uint16_t MenuItemId;         // auiMenuItemID
};
static_assert(offsetof(TerminalMenuItemRunEvent, pTerminalRef) == 0x8);
static_assert(offsetof(TerminalMenuItemRunEvent, TerminalFormId) == 0x10);
static_assert(offsetof(TerminalMenuItemRunEvent, MenuItemId) == 0x14);

// GameScript::FragmentEventHandler::ProcessEvent(const TerminalMenuItemRunEvent&, BSTEventSource*): runs the item's
// fragment and sends OnMenuItemRun to the terminal's script
using TProcessTerminalEvent = int(void* apHandler, const TerminalMenuItemRunEvent* apEvent, void* apSource);
TProcessTerminalEvent* RealProcessTerminalEvent = nullptr;

int HookProcessTerminalEvent(void* apHandler, const TerminalMenuItemRunEvent* apEvent, void* apSource)
{
    // Runs we replay for another player call the real function directly, so everything here is the local player's.
    // Not a holotape played in the Pip-Boy (its terminal is the player): that's theirs alone.
    if (apEvent && apEvent->pTerminalRef && apEvent->pTerminalRef->formType != FormType::Character)
    {
        spdlog::debug("Terminal menu item {} of {:X} run on {:X}", apEvent->MenuItemId, apEvent->TerminalFormId, apEvent->pTerminalRef->formID);
        World::Get().GetRunner().Trigger(TerminalItemRunEvent{apEvent->pTerminalRef->formID, apEvent->TerminalFormId, apEvent->MenuItemId});
    }

    return RealProcessTerminalEvent(apHandler, apEvent, apSource);
}
} // namespace

namespace Terminals
{
bool RunMenuItem(TESObjectREFR* apTerminalRef, uint32_t aTerminalFormId, uint16_t aMenuItemId) noexcept
{
    // The game's one FragmentEventHandler
    POINTER_FALLOUT4(void*, s_fragmentEventHandler, 1417044);

    void* pHandler = *s_fragmentEventHandler.Get();
    if (!pHandler || !apTerminalRef || !RealProcessTerminalEvent)
        return false;

    const TerminalMenuItemRunEvent event{nullptr, apTerminalRef, aTerminalFormId, aMenuItemId};
    RealProcessTerminalEvent(pHandler, &event, nullptr);
    return true;
}
} // namespace Terminals

static TiltedPhoques::Initializer s_terminalHooks(
    []()
    {
        POINTER_FALLOUT4(TProcessTerminalEvent, s_processTerminalEvent, 496470);

        RealProcessTerminalEvent = s_processTerminalEvent.Get();

        TP_HOOK(&RealProcessTerminalEvent, HookProcessTerminalEvent);
    });

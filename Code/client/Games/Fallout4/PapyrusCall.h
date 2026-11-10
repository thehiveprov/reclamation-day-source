#pragma once

#include <variant>
#include <vector>

struct TESForm;

// Papyrus functions called from the client, run on the VM's own time (the call is queued, it returns at once): a global
// function of a script, or a method of the script on a quest. Arguments are ints, numbers and booleans. Main thread.
// (NemesisGame and CompanionService have their own copies of the quest call from before this.)
namespace PapyrusCall
{
using Argument = std::variant<int32_t, float, bool>;

// IVirtualMachine::DispatchStaticCall (slot 0x2C): "Function X(...) global" of the script
bool Static(const char* acpScript, const char* acpFunction, const std::vector<Argument>& acArguments = {}) noexcept;
// IVirtualMachine::DispatchMethodCall by handle (slot 0x2E): a function of the script attached to the quest
bool OnQuest(TESForm* apQuest, const char* acpScript, const char* acpFunction, const std::vector<Argument>& acArguments = {}) noexcept;
} // namespace PapyrusCall

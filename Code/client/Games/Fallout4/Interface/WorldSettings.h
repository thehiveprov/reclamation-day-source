#pragma once

#include <string>

// The F2 overlay's WORLD SETTINGS: the host's weather and NAC X's settings (which NAC only offers on its holotape),
// drawn by the overlay from Data\ReclamationDay\WorldSettings.json (GameFiles/Fallout4/ReclamationDay). Values are NAC's
// globals, read and written here, or the page's own (config\WorldSettings.ini); what takes more than a value runs in
// Papyrus (ReclamationDay:WorldSettings, NAC's own script). The overlay gets the open page as the "worldSettings" event
// and answers with mainMenuAction "worldSettings.<action>". docs/MENUS.md. Main thread.
namespace WorldSettings
{
// Every frame: the open page's values, read again now and then (scripts change them too)
void Run() noexcept;
// One of the overlay's choices: open, close, page <n>, set <entry>:<value>, press <entry>
void OnAction(const std::string& acAction, const std::string& acPayload) noexcept;
} // namespace WorldSettings

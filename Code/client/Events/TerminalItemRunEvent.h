#pragma once

/**
 * @brief The local player ran a terminal menu item (see Games/Fallout4/Terminals.h).
 */
struct TerminalItemRunEvent
{
    uint32_t TerminalRefId;  // the terminal used
    uint32_t TerminalFormId; // the TERM the item belongs to (the terminal's, or a holotape's played on it)
    uint16_t MenuItemId;
};

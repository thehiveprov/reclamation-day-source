#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// The player ran a terminal menu item (a terminal's choice, or a holotape's played on a terminal): the players nearby
// run the same item's script on their copy of the terminal, so what it does to the world (turrets, doors, robots)
// happens in every game.
struct RequestTerminalItemRun final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestTerminalItemRun;

    RequestTerminalItemRun()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestTerminalItemRun& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && TerminalRefId == acRhs.TerminalRefId && TerminalFormId == acRhs.TerminalFormId && MenuItemId == acRhs.MenuItemId;
    }

    GameId TerminalRefId{};  // the terminal used
    GameId TerminalFormId{}; // the TERM the item belongs to
    uint16_t MenuItemId{};
};

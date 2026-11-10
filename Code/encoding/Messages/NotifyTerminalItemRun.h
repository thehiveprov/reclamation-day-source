#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// A player nearby ran a terminal menu item (see RequestTerminalItemRun): run the same item's script here.
struct NotifyTerminalItemRun final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyTerminalItemRun;

    NotifyTerminalItemRun()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyTerminalItemRun& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && PlayerId == acRhs.PlayerId && TerminalRefId == acRhs.TerminalRefId && TerminalFormId == acRhs.TerminalFormId && MenuItemId == acRhs.MenuItemId;
    }

    uint32_t PlayerId{}; // who ran it
    GameId TerminalRefId{};
    GameId TerminalFormId{};
    uint16_t MenuItemId{};
};

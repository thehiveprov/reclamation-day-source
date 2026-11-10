#pragma once

#include "Message.h"

#include <Structs/Inventory.h>

// A player traded with a companion or settler that another player's game runs (the trade menu): what went into and out
// of its inventory, and what it was given to wear or had taken off. Copies of a character take no item changes of their
// own, so the server passes this to the game that owns it, which makes the same trade (its worn set then reaches
// everyone as usual).
struct RequestActorTrade final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestActorTrade;

    RequestActorTrade()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestActorTrade& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Changes == acRhs.Changes && Worn == acRhs.Worn;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{}; // the owner's, as the trading player's copy knew it
    Inventory Changes{};       // negative counts for what was taken out
    Inventory Worn{};          // put on in the trade (count 1) or taken off (-1)
};

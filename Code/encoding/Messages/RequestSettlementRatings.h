#pragma once

#include "Message.h"
#include <Structs/SettlementRatings.h>

// The host's game (the one that runs the settlements' day while connected): the ratings of claimed settlements that
// changed, or all of them now and then (Full). Also sent, empty, when the host connects: from then on the others hold
// their own day. Only the host's (an admin session) are taken.
struct RequestSettlementRatings final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSettlementRatings;

    RequestSettlementRatings()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestSettlementRatings& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Full == acRhs.Full && Settlements == acRhs.Settlements; }

    bool Full{};
    Vector<SettlementRatings> Settlements{};
};

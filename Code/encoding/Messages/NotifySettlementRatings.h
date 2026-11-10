#pragma once

#include "Message.h"
#include <Structs/SettlementRatings.h>

// Who runs the settlements' day (the host's player id, 0 while no host is in the world: every game runs its own), and
// the host's ratings: the changed ones, or all the server has (to a player joining, or after the host's full resend).
struct NotifySettlementRatings final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySettlementRatings;

    NotifySettlementRatings()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifySettlementRatings& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && HostPlayerId == acRhs.HostPlayerId && Settlements == acRhs.Settlements;
    }

    uint32_t HostPlayerId{};
    Vector<SettlementRatings> Settlements{};
};

#pragma once

#include "Message.h"

#include <Structs/SettlementClaim.h>

// Every claimed settlement, see RequestSettlementClaim. Sent to everyone whenever a claim or a right changes or a
// leader comes or goes, and to each player joining.
struct NotifySettlementClaims final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySettlementClaims;

    NotifySettlementClaims()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifySettlementClaims& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Claims == acRhs.Claims && LocalName == acRhs.LocalName && IsAdmin == acRhs.IsAdmin;
    }

    Vector<SettlementClaim> Claims{};
    String LocalName{};  // the receiver's name as the server knows it (it may have tidied the one typed in)
    bool IsAdmin{false}; // the receiver may release anyone's claim (the host is an admin)
};

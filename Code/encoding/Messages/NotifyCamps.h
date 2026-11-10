#pragma once

#include "Message.h"

#include <Structs/Camp.h>

// Every C.A.M.P., see RequestCamp. Sent to everyone when one is set down, moved or removed, and to each player joining.
struct NotifyCamps final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCamps;

    NotifyCamps()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyCamps& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Camps == acRhs.Camps; }

    Vector<Camp> Camps{};
};

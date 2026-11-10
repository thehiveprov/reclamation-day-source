#pragma once

#include "Message.h"

// An NPC's conversation lock changed: Locked by player PlayerId, or released. See RequestDialogueLock.
struct NotifyDialogueLock final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDialogueLock;

    NotifyDialogueLock()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDialogueLock& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && NpcServerId == acRhs.NpcServerId && PlayerId == acRhs.PlayerId && Locked == acRhs.Locked; }

    uint32_t NpcServerId{};
    uint32_t PlayerId{};
    bool Locked{};
};

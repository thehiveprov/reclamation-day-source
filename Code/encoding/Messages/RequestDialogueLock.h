#pragma once

#include "Message.h"

// A player started (Lock) or ended (!Lock) a conversation with an NPC. Whoever starts the conversation makes the
// choices; while it's locked, other players can't talk to that NPC and just follow along.
struct RequestDialogueLock final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDialogueLock;

    RequestDialogueLock()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDialogueLock& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && NpcServerId == acRhs.NpcServerId && Lock == acRhs.Lock; }

    uint32_t NpcServerId{};
    bool Lock{};
};

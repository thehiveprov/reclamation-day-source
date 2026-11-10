#pragma once

#include "Message.h"

#include <Structs/GameId.h>

// C.A.M.P.s (see Camp): the local player sets theirs down, moves it (where they used the C.A.M.P. device) or picks it
// up, or the host removes one.
struct RequestCamp final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCamp;

    enum Action : uint8_t
    {
        kPlace,  // the player has none yet, or theirs is stored (built again here, free): ModuleBaseId, WorldSpaceId, Position, Heading
        kMove,   // the player's own, with everything built at it: WorldSpaceId, Position, Heading
        kRemove, // the host (or an admin): CampId, with everything built at it
        kPickUp, // the player's own: everything built at it goes, kept as its layout for where it's set down next
    };

    RequestCamp()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestCamp& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && CampId == acRhs.CampId && ModuleBaseId == acRhs.ModuleBaseId && WorldSpaceId == acRhs.WorldSpaceId &&
               Position == acRhs.Position && Heading == acRhs.Heading;
    }

    uint8_t Action{};
    uint32_t CampId{};
    GameId ModuleBaseId{}; // RD_Camps.esp's module, as the server knows plugins
    GameId WorldSpaceId{};
    std::array<float, 3> Position{};
    float Heading{};
};

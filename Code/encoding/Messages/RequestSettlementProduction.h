#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Inventory.h>

// The host's game: what a settlement's day put into its workbench there, taken out again for the settlement's leader
// (connected, not the host). The server hands it to the leader, or back to the host if the leader just left.
struct RequestSettlementProduction final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSettlementProduction;

    RequestSettlementProduction()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestSettlementProduction& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && WorkshopId == acRhs.WorkshopId && Items == acRhs.Items; }

    GameId WorkshopId{};
    Inventory Items{};
};

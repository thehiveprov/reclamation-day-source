#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Inventory.h>

// A settlement's production from the host's day, for its workbench in this game (see RequestSettlementProduction).
struct NotifySettlementProduction final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySettlementProduction;

    NotifySettlementProduction()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifySettlementProduction& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && WorkshopId == acRhs.WorkshopId && Items == acRhs.Items; }

    GameId WorkshopId{};
    Inventory Items{};
};

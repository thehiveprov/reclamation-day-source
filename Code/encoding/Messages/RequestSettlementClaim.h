#pragma once

#include "Message.h"

#include <Structs/GameId.h>

using TiltedPhoques::String;

// Settlement claims (Fallout 4): the first player to claim a settlement at its workbench leads it, and gives the
// players of their party the right to build (place, move) and to scrap (scrap, store) there. Nobody builds at a
// settlement nobody claimed. The leader can abandon it for someone else to claim; the host can release anyone's.
// Players are known by name (see SettlementClaim).
struct RequestSettlementClaim final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSettlementClaim;

    enum Action : uint8_t
    {
        kClaim,     // SettlementName: what the claimer's game calls it
        kAbandon,   // the leader, or the host (releasing someone else's)
        kSetRights, // the leader: PlayerName's Build and Scrap
    };

    RequestSettlementClaim()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestSettlementClaim& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && WorkshopId == acRhs.WorkshopId && SettlementName == acRhs.SettlementName &&
               PlayerName == acRhs.PlayerName && Build == acRhs.Build && Scrap == acRhs.Scrap;
    }

    uint8_t Action{};
    GameId WorkshopId{}; // the settlement's workbench
    String SettlementName{};
    String PlayerName{};
    bool Build{false};
    bool Scrap{false};
};

#pragma once

#include "Message.h"
#include "RequestMovementAction.h"

// A player's movement action (RequestMovementAction::Action), to play on the receiver's copy of their character, or a
// parkour move of an NPC their game runs, on the copy of that NPC
struct NotifyMovementAction final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyMovementAction;

    NotifyMovementAction()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyMovementAction& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && Type == acRhs.Type && Parkour == acRhs.Parkour && OffHand == acRhs.OffHand &&
               Grapple == acRhs.Grapple && Emote == acRhs.Emote && Bike == acRhs.Bike && Parked == acRhs.Parked && Interact == acRhs.Interact && Truck == acRhs.Truck;
    }

    uint32_t ServerId{}; // the player's character, or the NPC (Parkour.IsNpc)
    uint8_t Type{};
    // a parkour move, as RequestMovementAction has it
    RequestMovementAction::ParkourPath Parkour{};
    // the left hand's weapon or what it did, as RequestMovementAction has it
    RequestMovementAction::OffHandState OffHand{};
    // the grappling gun's line, as RequestMovementAction has it
    RequestMovementAction::GrappleLine Grapple{};
    // an emote, as RequestMovementAction has it
    RequestMovementAction::EmoteCue Emote{};
    // their motorcycle, as RequestMovementAction has it
    RequestMovementAction::BikeState Bike{};
    RequestMovementAction::BikeParked Parked{};
    // their left hand going out to something, as RequestMovementAction has it
    RequestMovementAction::InteractCue Interact{};
    // what a caravan truck's doing, as RequestMovementAction has it
    RequestMovementAction::TruckState Truck{};
};

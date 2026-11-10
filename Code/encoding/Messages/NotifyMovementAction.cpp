#include <Messages/NotifyMovementAction.h>

void NotifyMovementAction::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(Type, 8);
    if (Type == RequestMovementAction::kParkour)
        Parkour.Serialize(aWriter);
    else if (Type == RequestMovementAction::kOffHand || Type == RequestMovementAction::kOffHandCue)
        OffHand.Serialize(aWriter);
    else if (Type == RequestMovementAction::kGrapple)
        Grapple.Serialize(aWriter);
    else if (Type == RequestMovementAction::kEmote)
        Emote.Serialize(aWriter);
    else if (Type == RequestMovementAction::kBike)
        Bike.Serialize(aWriter);
    else if (Type == RequestMovementAction::kBikeParked)
        Parked.Serialize(aWriter);
    else if (Type == RequestMovementAction::kInteract)
        Interact.Serialize(aWriter);
    else if (Type == RequestMovementAction::kTruck)
        Truck.Serialize(aWriter);
}

void NotifyMovementAction::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Type = static_cast<uint8_t>(value);
    if (Type == RequestMovementAction::kParkour)
        Parkour.Deserialize(aReader);
    else if (Type == RequestMovementAction::kOffHand || Type == RequestMovementAction::kOffHandCue)
        OffHand.Deserialize(aReader);
    else if (Type == RequestMovementAction::kGrapple)
        Grapple.Deserialize(aReader);
    else if (Type == RequestMovementAction::kEmote)
        Emote.Deserialize(aReader);
    else if (Type == RequestMovementAction::kBike)
        Bike.Deserialize(aReader);
    else if (Type == RequestMovementAction::kBikeParked)
        Parked.Deserialize(aReader);
    else if (Type == RequestMovementAction::kInteract)
        Interact.Deserialize(aReader);
    else if (Type == RequestMovementAction::kTruck)
        Truck.Deserialize(aReader);
}

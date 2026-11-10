#include <Messages/RequestMovementAction.h>

void RequestMovementAction::OffHandState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Weapon.Serialize(aWriter);
    Inventory::ObjectMod::SerializeList(aWriter, Mods);
    aWriter.WriteBits(Cue, 8);
}

void RequestMovementAction::OffHandState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Weapon.Deserialize(aReader);
    Inventory::ObjectMod::DeserializeList(aReader, Mods);
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Cue = static_cast<uint8_t>(value);
}

void RequestMovementAction::GrappleLine::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Stage, 8);
    for (const float value : From)
        Serialization::WriteFloat(aWriter, value);
    for (const float value : To)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteFloat(aWriter, Speed);
    Serialization::WriteVarInt(aWriter, Target);
}

void RequestMovementAction::GrappleLine::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Stage = static_cast<uint8_t>(value);
    for (float& from : From)
        from = Serialization::ReadFloat(aReader);
    for (float& to : To)
        to = Serialization::ReadFloat(aReader);
    Speed = Serialization::ReadFloat(aReader);
    Target = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}

void RequestMovementAction::EmoteCue::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Emote, 8);
    aWriter.WriteBits(Stage, 8);
}

void RequestMovementAction::EmoteCue::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Emote = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Stage = static_cast<uint8_t>(value);
}

void RequestMovementAction::ParkourPath::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Move, 8);
    for (const float value : Start)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteFloat(aWriter, Heading);
    for (const float value : Ideal)
        Serialization::WriteFloat(aWriter, value);
    for (const float value : End)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteFloat(aWriter, Bump);
    Serialization::WriteBool(aWriter, IsNpc);
    if (IsNpc)
        Serialization::WriteVarInt(aWriter, Npc);
}

void RequestMovementAction::ParkourPath::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Move = static_cast<uint8_t>(value);
    for (float& start : Start)
        start = Serialization::ReadFloat(aReader);
    Heading = Serialization::ReadFloat(aReader);
    for (float& ideal : Ideal)
        ideal = Serialization::ReadFloat(aReader);
    for (float& end : End)
        end = Serialization::ReadFloat(aReader);
    Bump = Serialization::ReadFloat(aReader);
    IsNpc = Serialization::ReadBool(aReader);
    Npc = IsNpc ? Serialization::ReadVarInt(aReader) & 0xFFFFFFFF : 0;
}

void RequestMovementAction::BikeState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Stage, 8);
    aWriter.WriteBits(static_cast<uint8_t>(Side), 8);
    aWriter.WriteBits(Paint, 8);
    aWriter.WriteBits(Flags, 8);
    for (const float value : {Heading, Lean, Pitch, Steer, Suspension, WheelSpeed, Rpm, Throttle, StageTime, Stand, StandGround})
        Serialization::WriteFloat(aWriter, value);
}

void RequestMovementAction::BikeState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Stage = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Side = static_cast<int8_t>(static_cast<uint8_t>(value));
    aReader.ReadBits(value, 8);
    Paint = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Flags = static_cast<uint8_t>(value);
    for (float* pValue : {&Heading, &Lean, &Pitch, &Steer, &Suspension, &WheelSpeed, &Rpm, &Throttle, &StageTime, &Stand, &StandGround})
        *pValue = Serialization::ReadFloat(aReader);
}

void RequestMovementAction::BikeParked::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, Shown);
    if (!Shown)
        return;
    aWriter.WriteBits(Paint, 8);
    for (const float value : At)
        Serialization::WriteFloat(aWriter, value);
    Serialization::WriteFloat(aWriter, Heading);
    Serialization::WriteFloat(aWriter, Roll);
    CellOrWorld.Serialize(aWriter);
}

void RequestMovementAction::BikeParked::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Shown = Serialization::ReadBool(aReader);
    if (!Shown)
        return;
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Paint = static_cast<uint8_t>(value);
    for (float& at : At)
        at = Serialization::ReadFloat(aReader);
    Heading = Serialization::ReadFloat(aReader);
    Roll = Serialization::ReadFloat(aReader);
    CellOrWorld.Deserialize(aReader);
}

void RequestMovementAction::InteractCue::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Kind, 8);
    Item.Serialize(aWriter);
    Serialization::WriteBool(aWriter, HasTarget);
    if (!HasTarget)
        return;
    for (const float value : Target)
        Serialization::WriteFloat(aWriter, value);
}

void RequestMovementAction::InteractCue::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Kind = static_cast<uint8_t>(value);
    Item.Deserialize(aReader);
    HasTarget = Serialization::ReadBool(aReader);
    if (!HasTarget)
        return;
    for (float& target : Target)
        target = Serialization::ReadFloat(aReader);
}

void RequestMovementAction::TruckState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Index, 8);
    aWriter.WriteBits(Flags, 8);
    Serialization::WriteVarInt(aWriter, Trader);
}

void RequestMovementAction::TruckState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Index = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Flags = static_cast<uint8_t>(value);
    Trader = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}

void RequestMovementAction::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Type, 8);
    if (Type == kParkour)
        Parkour.Serialize(aWriter);
    else if (Type == kOffHand || Type == kOffHandCue)
        OffHand.Serialize(aWriter);
    else if (Type == kGrapple)
        Grapple.Serialize(aWriter);
    else if (Type == kEmote)
        Emote.Serialize(aWriter);
    else if (Type == kNpcSlide)
        Serialization::WriteVarInt(aWriter, Npc);
    else if (Type == kBike)
        Bike.Serialize(aWriter);
    else if (Type == kBikeParked)
        Parked.Serialize(aWriter);
    else if (Type == kInteract)
        Interact.Serialize(aWriter);
    else if (Type == kNpcBike)
    {
        Serialization::WriteVarInt(aWriter, Npc);
        Bike.Serialize(aWriter);
    }
    else if (Type == kTruck)
        Truck.Serialize(aWriter);
}

void RequestMovementAction::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Type = static_cast<uint8_t>(value);
    if (Type == kParkour)
        Parkour.Deserialize(aReader);
    else if (Type == kOffHand || Type == kOffHandCue)
        OffHand.Deserialize(aReader);
    else if (Type == kGrapple)
        Grapple.Deserialize(aReader);
    else if (Type == kEmote)
        Emote.Deserialize(aReader);
    else if (Type == kNpcSlide)
        Npc = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    else if (Type == kBike)
        Bike.Deserialize(aReader);
    else if (Type == kBikeParked)
        Parked.Deserialize(aReader);
    else if (Type == kInteract)
        Interact.Deserialize(aReader);
    else if (Type == kNpcBike)
    {
        Npc = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        Bike.Deserialize(aReader);
    }
    else if (Type == kTruck)
        Truck.Deserialize(aReader);
}

#include <Messages/RequestSettlementClaim.h>

void RequestSettlementClaim::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    WorkshopId.Serialize(aWriter);
    Serialization::WriteString(aWriter, SettlementName);
    Serialization::WriteString(aWriter, PlayerName);
    Serialization::WriteBool(aWriter, Build);
    Serialization::WriteBool(aWriter, Scrap);
}

void RequestSettlementClaim::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Action = static_cast<uint8_t>(value);
    WorkshopId.Deserialize(aReader);
    SettlementName = Serialization::ReadString(aReader);
    PlayerName = Serialization::ReadString(aReader);
    Build = Serialization::ReadBool(aReader);
    Scrap = Serialization::ReadBool(aReader);
}
